#!/usr/bin/env python3
"""用真实持久 TAP 验证独立监护，不接管任何已有接口，也不执行恢复删除"""

import argparse
import errno
import fcntl
import os
from pathlib import Path
import signal
import struct
import subprocess
import tempfile
import time

from batch_update import check, fields
from health_readiness import clean_environment, daemon_command
from recovery_isolation import wait_for


def run_case(build, mode, native):
    """分别强制结束两个独立进程，验证持久证据不会因进程重启重新变成可信"""
    environment = clean_environment()
    owned = {}
    monitor = daemon = None

    def run(*arguments, error=None):
        result = subprocess.run(list(map(str, arguments)), env=environment, capture_output=True,
                                text=True, timeout=10)
        check(result.returncode == 0 if error is None else result.returncode != 0, result.stdout + result.stderr)
        if error:
            check(f"({-error})" in result.stderr, result.stdout + result.stderr)
        return result.stdout

    def remove(name):
        check(Path(f"/sys/class/net/{name}/ifindex").read_text() == owned[name], "test interface replaced externally")
        run("ip", "link", "delete", "dev", name)
        del owned[name]

    with tempfile.TemporaryDirectory(prefix="dppd-monitor-") as temporary:
        directory = Path(temporary)
        (directory / "storage").mkdir()
        guard, state = directory / "recovery", directory / "storage/state.bin"
        session, log, daemon_log = directory / "session", directory / "monitor.log", directory / "daemon.log"
        interfaces = (f"dm{os.getpid()}a", f"dm{os.getpid()}b")
        mapping = [f"{index}:{name}" for index, name in enumerate(interfaces)]
        executable = build / "tests/integration/test_recovery_monitor_lost" if mode == "lost" else build / "dppd-monitor"
        command = [str(executable), "run", str(guard), str(state), str(session), *mapping]

        def show(expected, reason=None):
            output = run(build / "dppd-monitor", "show", session)
            check(f"state={expected} " in output and "freshness=unverified executable=no" in output, output)
            if reason:
                check(f"reason={reason} " in output, output)
            return output

        def kill(process):
            process.kill()
            process.wait(timeout=10)
            check(process.returncode == -signal.SIGKILL, "wrong kill status")

        try:
            for name in interfaces:
                check(not Path(f"/sys/class/net/{name}").exists(), "existing interface")
                run("ip", "tuntap", "add", "dev", name, "mode", "tap", "multi_queue")
                owned[name] = Path(f"/sys/class/net/{name}/ifindex").read_text()
            # 参数错误必须在创建恢复文件和会话文件之前退出
            run(*command[:-2], mapping[0], mapping[0], error=errno.EINVAL)
            check(not guard.exists() and not session.exists(), "invalid mapping created files")
            with log.open("w") as output:
                monitor = subprocess.Popen(command, env=environment, stdout=output, stderr=subprocess.STDOUT)
            wait_for(monitor, lambda: "monitor ready session=" in log.read_text(), "monitor readiness")
            initial = show("active")
            check("writer-lock=held" in initial and (session.stat().st_mode & 0o777) == 0o600, initial)
            nonce = fields(initial.splitlines()[1])["session"]
            check(len(nonce) == 32 and int(nonce, 16) != 0 and session.stat().st_size == 5760, initial)

            if mode in ("business-first", "monitor-first", "both", "stop-after-business"):
                daemon_cmd = daemon_command(build, directory, 1, interfaces) + ["--recovery-path", str(guard)]
                if native:
                    daemon_cmd.append("--tap-owner-cookie")
                with daemon_log.open("w") as output:
                    daemon = subprocess.Popen(daemon_cmd, env=environment, stdout=output, stderr=subprocess.STDOUT)
                sock = directory / "ctl.sock"
                wait_for(daemon, sock.exists, "business socket")
                wait_for(daemon, lambda: fields(run(build / "dppctl", "--socket", sock, "health"))["ready"] == "yes",
                         "business readiness")
                run(build / "dppctl", "--socket", sock, "apply-drop", "970", "0", "0", "9", "require")
            else:
                state.write_bytes(b"read-only snapshot sentinel\n")
            baseline = guard.read_bytes(), state.read_bytes()

            # 普通属性和 TC 更新不应结束接口生命周期监护
            run("ip", "link", "set", "dev", interfaces[0], "alias", "dppd-monitor-test")
            if daemon is None:
                run("tc", "qdisc", "add", "dev", interfaces[0], "root", "handle", "1:", "multiq")
                run("tc", "qdisc", "del", "dev", interfaces[0], "root")
            if mode != "lost":
                time.sleep(0.35)
                check(monitor.poll() is None, log.read_text())
                show("active")

            if mode in ("monitor-first", "both"):
                kill(monitor)
                frozen = session.read_bytes()
                show("interrupted", "writer-missing")
                if mode == "monitor-first":
                    # 当前阶段没有业务握手，不能把监护死亡误报成业务已被隔离
                    check(fields(run(build / "dppctl", "--socket", sock, "health"))["live"] == "yes",
                          "business unexpectedly stopped")
                    run(*command[:4], str(directory / "late-session"), *mapping, error=errno.EBUSY)
                kill(daemon)
                show("interrupted", "writer-missing")
                run(*command[:4], str(directory / "late-session"), *mapping, error=errno.EUCLEAN)
                check(session.read_bytes() == frozen and not (directory / "late-session").exists(),
                      "late monitor erased interrupted evidence")
            elif mode in ("business-first", "stop-after-business"):
                kill(daemon)
                time.sleep(0.35)
                check(monitor.poll() is None, log.read_text())
                show("active")
                if mode == "business-first":
                    remove(interfaces[0])
                    monitor.wait(timeout=10)
                    check(monitor.returncode != 0, log.read_text())
                    show("invalid", "interface-changed")
                else:
                    monitor.terminate()
                    monitor.wait(timeout=10)
                    check(monitor.returncode == 0, log.read_text())
                    show("stopped", "stop-requested")
            elif mode == "stopped":
                monitor.terminate()
                monitor.wait(timeout=10)
                check(monitor.returncode == 0, log.read_text())
                show("stopped", "stop-requested")
                frozen = session.read_bytes()
                run(*command, error=errno.EEXIST)
                check(session.read_bytes() == frozen, "restart replaced terminal evidence")
            elif mode == "paused-recreate":
                monitor.send_signal(signal.SIGSTOP)
                wait_for(monitor, lambda: "State:\tT" in Path(f"/proc/{monitor.pid}/status").read_text(), "monitor pause")
                show("active")  # 锁仍被占用，因此必须同时明确 freshness 未验证
                original = int(owned[interfaces[0]])
                remove(interfaces[0])
                fd = os.open("/dev/net/tun", os.O_RDWR | os.O_CLOEXEC)
                try:
                    fcntl.ioctl(fd, 0x400454DA, struct.pack("I", original))
                    request = struct.pack("16sH22x", interfaces[0].encode(), 0x0002 | 0x0100 | 0x1000 | 0x8000)
                    fcntl.ioctl(fd, 0x400454CA, request)
                    fcntl.ioctl(fd, 0x400454CB, 1)
                    owned[interfaces[0]] = Path(f"/sys/class/net/{interfaces[0]}/ifindex").read_text()
                    check(int(owned[interfaces[0]]) == original, "index not reused")
                finally:
                    os.close(fd)
                monitor.send_signal(signal.SIGCONT)
                monitor.wait(timeout=10)
                check(monitor.returncode != 0, log.read_text())
                show("invalid", "interface-changed")
            elif mode == "rename":
                renamed = f"dm{os.getpid()}c"
                check(not Path(f"/sys/class/net/{renamed}").exists(), "rename target exists")
                run("ip", "link", "set", "dev", interfaces[0], "name", renamed)
                owned[renamed] = owned.pop(interfaces[0])
                monitor.wait(timeout=10)
                check(monitor.returncode != 0, log.read_text())
                show("invalid", "interface-changed")
            elif mode == "guard-replaced":
                guard.rename(directory / "original-recovery")
                guard.write_bytes(baseline[0])
                guard.chmod(0o600)
                monitor.wait(timeout=10)
                check(monitor.returncode != 0, log.read_text())
                show("invalid", "recovery-record-replaced")
                check((directory / "original-recovery").read_bytes() == baseline[0], "original recovery changed")
            elif mode == "lost":
                wait_for(monitor, lambda: "State:\tT" in Path(f"/proc/{monitor.pid}/status").read_text(), "fault pause")
                monitor.send_signal(signal.SIGCONT)
                monitor.wait(timeout=10)
                check(monitor.returncode != 0, log.read_text())
                output = show("invalid", "observer-lost")
                check(f"error={-errno.ENOBUFS}" in output, output)
            check((guard.read_bytes(), state.read_bytes()) == baseline, "monitor changed recovery file or snapshot")
            output = run(build / "dppd-monitor", "show", session)
            check(f"session={nonce} " in output, "session identity changed")
        except Exception as error:
            details = "\n".join(path.read_text() for path in (log, daemon_log) if path.exists())
            raise AssertionError(f"case={mode}\n{details}") from error
        finally:
            for process in (monitor, daemon):
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait(timeout=10)
            for name in tuple(owned):
                remove(name)
    print(f"PASS independent recovery monitor {mode} native={native}: durable evidence, unchanged recovery/snapshot",
          flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--native", action="store_true", help="use patched TAP owner cookies for business rules")
    arguments = parser.parse_args()
    check(os.geteuid() == 0, "monitor integration tests need root for test-owned TAPs")
    for mode in ("business-first", "monitor-first", "both", "stop-after-business", "stopped", "paused-recreate",
                 "rename", "guard-replaced", "lost"):
        run_case(arguments.build_dir.resolve(), mode, arguments.native)


if __name__ == "__main__":
    main()
