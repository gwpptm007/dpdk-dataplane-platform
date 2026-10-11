#!/usr/bin/env python3
"""真实 daemon 与独立监护握手，验证安装前拒绝、创建后回滚和启动重放"""

import argparse
import errno
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

from batch_update import check, fields
from health_readiness import clean_environment, daemon_command
from recovery_isolation import wait_for


def run_case(build, mode, native):
    environment = clean_environment()
    owned = {}
    monitor = daemon = client = None

    def run(*args, error=None):
        result = subprocess.run(list(map(str, args)), env=environment, text=True, capture_output=True, timeout=10)
        check(result.returncode == 0 if error is None else result.returncode != 0, result.stdout + result.stderr)
        if error:
            check(f"({-error})" in result.stderr, result.stdout + result.stderr)
        return result.stdout

    def terminate(process, expected=0):
        process.send_signal(signal.SIGINT)
        process.wait(timeout=10)
        check(process.returncode == expected, "unexpected graceful exit")

    def kill(process):
        process.kill()
        process.wait(timeout=10)
        check(process.returncode == -signal.SIGKILL, "unexpected kill status")

    def pause(process):
        process.send_signal(signal.SIGSTOP)
        wait_for(process, lambda: "State:\tT" in Path(f"/proc/{process.pid}/status").read_text(), "paused process")

    with tempfile.TemporaryDirectory(prefix="dppd-handshake-") as temporary:
        directory = Path(temporary)
        (directory / "storage").mkdir()
        guard, state = directory / "recovery", directory / "storage/state.bin"
        session, sock = directory / "session", directory / "ctl.sock"
        interfaces = (f"dhm{os.getpid()}a", f"dhm{os.getpid()}b")
        log_paths = []
        mapping = [f"{index}:{name}" for index, name in enumerate(interfaces)]
        if mode == "wrong-map":
            mapping = [f"{index}:{name}" for index, name in enumerate(reversed(interfaces))]
        if mode == "wrong-set":
            mapping = mapping[:1]

        def start_monitor(target):
            log = directory / f"monitor-{len(log_paths)}.log"
            log_paths.append(log)
            with log.open("w") as output:
                process = subprocess.Popen([str(build / "dppd-monitor"), "run", str(guard), str(state),
                                            str(target), *mapping], env=environment,
                                           stdout=output, stderr=subprocess.STDOUT)
            wait_for(process, lambda: "monitor ready" in log.read_text(), "monitor ready")
            return process

        def start_daemon(target=session, expect=None):
            log = directory / f"daemon-{len(log_paths)}.log"
            log_paths.append(log)
            command = daemon_command(build, directory, 1, interfaces)
            command += ["--recovery-path", str(directory / "other-recovery" if mode == "wrong-record" else guard)]
            if target is not None:
                command += ["--recovery-monitor", str(target)]
            if native:
                command.append("--tap-owner-cookie")
            current = environment
            if mode in ("before-create", "after-create"):
                current = dict(environment, LD_PRELOAD=str(build / "tests/integration/libdppd_recovery_attempt_faults.so"),
                               DPPD_TEST_RECOVERY_WINDOW=mode.split("-")[0])
            if mode == "before-install":
                command[0] = str(build / "tests/integration/test_monitor_install_window")
            with log.open("w") as output:
                process = subprocess.Popen(command, env=current, stdout=output, stderr=subprocess.STDOUT)
            if expect is not None:
                process.wait(timeout=10)
                check(process.returncode != 0 and expect in log.read_text(), log.read_text())
            else:
                try:
                    wait_for(process, sock.exists, "business socket")
                    wait_for(process, lambda: fields(run(build / "dppctl", "--socket", sock, "health"))["ready"] == "yes",
                             "business ready")
                except Exception:
                    if process.poll() is None:
                        kill(process)
                    raise
            return process

        def filters():
            rows = json.loads(run("tc", "-j", "filter", "show", "dev", interfaces[0], "parent", "1:"))
            return [row for row in rows if row.get("kind") == "flower" and "options" in row]

        try:
            for name in interfaces:
                check(not Path(f"/sys/class/net/{name}").exists(), "test interface exists")
                run("ip", "tuntap", "add", "dev", name, "mode", "tap", "multi_queue")
                owned[name] = Path(f"/sys/class/net/{name}/ifindex").read_text()
            monitor = start_monitor(session)
            initial = guard.read_bytes()
            if mode == "paused-start":
                pause(monitor)
            if mode in ("wrong-map", "wrong-set", "wrong-record", "paused-start"):
                expected = -errno.ETIMEDOUT if mode == "paused-start" else -errno.EXDEV
                started = time.monotonic()
                daemon = start_daemon(expect=f"recovery monitor handshake failed: {expected}")
                check(time.monotonic() - started < 5, "startup handshake did not return in bounded time")
                check(not state.exists() and guard.read_bytes() == initial and not filters(),
                      "rejected startup changed snapshot, original guard or application flows")
                if mode == "paused-start":
                    output = run(build / "dppd-monitor", "show", session)
                    check("state=active" in output and "freshness=unverified" in output, output)
            else:
                daemon = start_daemon()
                bound = guard.read_bytes()
                check(int.from_bytes(bound[8:12], "little") == 6, "guard did not upgrade on explicit binding")
                check(bound[9264:9280] == session.read_bytes()[32:48], "durable session ID mismatch")
                saved = state.read_bytes()
                if mode in ("before-install", "before-create", "after-create"):
                    client = subprocess.Popen([str(build / "dppctl"), "--socket", str(sock), "apply-drop",
                                               "980", "0", "0", "9", "require"], env=environment,
                                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    wait_for(daemon, lambda: "State:\tT" in Path(f"/proc/{daemon.pid}/status").read_text(),
                             "installation window")
                    check(len(filters()) == (1 if mode == "after-create" else 0), "wrong kernel create window")
                    kill(monitor)
                    daemon.send_signal(signal.SIGCONT)
                    output, error = client.communicate(timeout=10)
                    check(client.returncode != 0, output + error)
                    daemon.wait(timeout=10)
                    check(daemon.returncode != 0 and not filters() and state.read_bytes() == saved,
                          "monitor failure published snapshot or retained created flow")
                    recovery = run(build / "dppd-recovery", "show", guard)
                    if mode == "before-install":
                        check("state=external-reconciliation-required" in recovery and "phase=intent" in recovery,
                              recovery)
                    else:
                        check("state=clean" in recovery, recovery)
                else:
                    run(build / "dppctl", "--socket", sock, "apply-drop", "980", "0", "0", "9", "require")
                    check(len(filters()) == 1, "business flow absent")
                    saved = state.read_bytes()
                    if mode == "business-crash":
                        kill(daemon)
                        check(monitor.poll() is None and len(filters()) == 1, "independent observation or residual lost")
                        residual = guard.read_bytes()
                        check(residual[9264:9280] == session.read_bytes()[32:48], "crash lost durable session binding")
                        recovery = run(build / "dppd-recovery", "show", guard)
                        check("state=external-reconciliation-required" in recovery and "monitor-required=yes" in recovery,
                              recovery)
                        revision = fields(recovery.splitlines()[0])["revision"]
                        # 崩溃后的链路通知可能延迟到达，计划应先拒绝变化窗口，再允许稳定后的新查询
                        for query in range(20):
                            result = subprocess.run([str(build / "dppd-recovery"), "plan", str(guard), revision],
                                                    env=environment, text=True, capture_output=True, timeout=10)
                            planned = result.stdout
                            check(guard.read_bytes() == residual and state.read_bytes() == saved,
                                  "read-only crash plan changed persisted evidence")
                            if result.returncode == 0:
                                break
                            check(f"({-errno.ESTALE})" in result.stderr and "link-changed=yes" in planned and
                                  "candidates=0" in planned, planned + result.stderr)
                            time.sleep(0.1)
                        else:
                            raise AssertionError("crash link notifications did not settle: " + planned)
                        check("executable=no" in planned and "history-covered=no" in planned, planned)
                        check("candidates=1" in planned if native else "candidates=0" in planned, planned)
                        daemon = start_daemon(expect="prior hardware activity requires external reconciliation")
                        check(guard.read_bytes() == residual and state.read_bytes() == saved,
                              "crash inspection or refused restart changed persisted evidence")
                    elif mode == "normal-replay":
                        terminate(daemon)
                        check(not filters(), "normal shutdown retained owned flow")
                        recovery = run(build / "dppd-recovery", "show", guard)
                        check("state=clean" in recovery and "monitor-required=yes" in recovery, recovery)
                        clean = guard.read_bytes()
                        daemon = start_daemon(target=None, expect="requires --recovery-monitor")
                        daemon = start_daemon(expect=f"recovery monitor handshake failed: {-errno.EXDEV}")
                        check(guard.read_bytes() == clean and state.read_bytes() == saved and not filters(),
                              "missing monitor or old baseline bypassed replay protection")
                        terminate(monitor)
                        next_session = directory / "session-next"
                        monitor = start_monitor(next_session)
                        daemon = start_daemon(next_session)
                        check(len(filters()) == 1 and state.read_bytes() == saved, "monitored snapshot replay failed")
                        terminate(daemon)
                        check(not filters(), "replayed flow not removed on exit")
                    else:
                        if mode == "lost-live":
                            kill(monitor)
                        elif mode == "paused-live":
                            pause(monitor)
                        elif mode == "session-replaced":
                            data = session.read_bytes()
                            session.rename(directory / "original-session")
                            session.write_bytes(data)
                            session.chmod(0o600)
                        daemon.wait(timeout=10)
                        check(daemon.returncode != 0 and not filters() and state.read_bytes() == saved,
                              "live monitor failure did not stop and clean owned flows")
                        check(any("recovery monitor unavailable" in path.read_text() for path in log_paths),
                              "monitor failure not diagnosed")
        except Exception as error:
            logs = "\n".join(path.read_text() for path in log_paths)
            raise AssertionError(f"case={mode}\n{logs}") from error
        finally:
            for process in (client, daemon, monitor):
                if process is not None and process.poll() is None:
                    kill(process)
            for name, index in tuple(owned.items()):
                check(Path(f"/sys/class/net/{name}/ifindex").read_text() == index, "interface replaced externally")
                run("ip", "link", "delete", "dev", name)
                del owned[name]
    print(f"PASS monitor handshake {mode} native={native}: fresh session gate, kernel rules and snapshot checked", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--native", action="store_true")
    arguments = parser.parse_args()
    check(os.geteuid() == 0, "monitor handshake tests require root for test-owned TAPs")
    for mode in ("normal-replay", "business-crash", "wrong-map", "wrong-set", "wrong-record", "paused-start", "lost-live",
                 "paused-live", "session-replaced", "before-install", "before-create", "after-create"):
        run_case(arguments.build_dir.resolve(), mode, arguments.native)


if __name__ == "__main__":
    main()
