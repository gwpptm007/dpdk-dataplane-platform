#!/usr/bin/env python3
"""Test daemon isolation/retry/restart using an explicit flow API fault fixture."""

import argparse
import errno
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

from batch_update import check, fields


def wait_for(daemon, predicate, label, seconds=10):
    deadline = time.monotonic() + seconds
    while not predicate():
        check(daemon.poll() is None, f"daemon exited while waiting for {label}")
        check(time.monotonic() < deadline, f"timeout waiting for {label}")
        time.sleep(0.02)


def stop(daemon):
    if daemon.poll() is None:
        daemon.send_signal(signal.SIGINT)
        try:
            daemon.wait(timeout=10)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait()
            raise AssertionError("daemon did not stop")


def run_case(build, library, lcores, fault):
    with tempfile.TemporaryDirectory(prefix="dppd-recovery-") as temporary:
        directory = Path(temporary)
        sock, state = directory / "ctl.sock", directory / "state.bin"
        command = [str(build / "dppd"), "-l", lcores, "--no-huge", "--no-pci",
                   "-m", "64", "--file-prefix=" + directory.name,
                   "--vdev=net_ring0", "--vdev=net_ring1", "--", "--ports", "0,1",
                   "--queues", "1", "--mbufs", "1024", "--cache", "64",
                   "--rule-capacity", "4", "--control-socket", str(sock),
                   "--state-path", str(state)]
        client_env = dict(os.environ, LC_ALL="C")
        # The CLI never loads the fixture. Only this dedicated daemon receives it.
        client_env.pop("LD_PRELOAD", None)
        environment = dict(client_env, LD_PRELOAD=str(library), DPPD_TEST_FLOW_FAULT_MODE=fault)

        def ctl(*args, error=None):
            result = subprocess.run([str(build / "dppctl"), "--socket", str(sock),
                                     *map(str, args)], capture_output=True, text=True,
                                    timeout=5, env=client_env)
            if error is None:
                check(result.returncode == 0, result.stderr)
            else:
                check(result.returncode != 0 and f"({-error})" in result.stderr,
                      f"expected errno {error}: {result.stdout}{result.stderr}")
            return result.stdout

        for replay in (False, True):
            log = directory / ("replay.log" if replay else "fault.log")
            with log.open("w") as output:
                daemon = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                          env=environment)
            try:
                wait_for(daemon, sock.exists, "management socket", seconds=15)
                if replay:
                    page = fields(ctl("list").splitlines()[0])
                    check(page["total"] == "2" and page["repository-generation"] == "2",
                          str(page))
                    for rule_id, generation in [(700, 1), (701, 2)]:
                        rule = fields(ctl("get", rule_id).splitlines()[0])
                        check(rule["generation"] == str(generation) and rule["priority"] == "0",
                              str(rule))
                    check(fields(ctl("reconcile-status"))["state"] == "ready", "replay not ready")
                    stop(daemon)
                    check(daemon.returncode == 0, log.read_text())
                else:
                    created = ctl("apply-drop-batch", 0, 700, 701)
                    check(all(fields(line)["backend"] == "1" for line in created.splitlines()[1:]),
                          "flow interposition was not active: " + created)
                    saved = state.read_bytes()
                    ctl("update-drop-batch", 0, 20, "require", 700, 1, 701, 2, error=errno.EUCLEAN)
                    # This line is emitted only after dppd_runtime_wait joins launched workers.
                    wait_for(daemon, lambda: "workers stopped" in log.read_text(), "worker stop")
                    status = fields(ctl("reconcile-status"))
                    check(status["state"] == "reconciliation-required", str(status))
                    check(status["residual-objects"] == ("3" if fault == "create-rollback" else "1"),
                          str(status))
                    # 隔离仍可证明管理线程存活，但停止的转发线程不能被报告为已就绪
                    health = fields(ctl("health"))
                    check(health["live"] == "yes" and health["ready"] == "no", str(health))
                    check(health["workers"] == "0/1" and "recovery-required" in health["reasons"],
                          str(health))
                    readiness = subprocess.run([str(build / "dppctl"), "--socket", str(sock), "ready"],
                                               env=client_env, capture_output=True, text=True, timeout=5)
                    check(readiness.returncode == 1 and fields(readiness.stdout)["ready"] == "no",
                          readiness.stdout + readiness.stderr)
                    for request in [("ping",), ("list",), ("get", 700), ("persistence-flush",),
                                    ("update-drop-batch", 0, 30, "require", 700, 1, 701, 2)]:
                        ctl(*request, error=errno.EUCLEAN)
                    check(state.read_bytes() == saved, "isolation changed the old snapshot")
                    ctl("reconcile-retry", error=errno.EUCLEAN)
                    status = fields(ctl("reconcile-status"))
                    check(status["state"] == "reconciliation-required" and
                          status["residual-objects"] == "1", str(status))
                    ctl("ping", error=errno.EUCLEAN)
                    retried = fields(ctl("reconcile-retry"))
                    check(retried["state"] == "restart-required" and
                          retried["residual-objects"] == "0", str(retried))
                    daemon.wait(timeout=10)
                    check(daemon.returncode == 1, f"unexpected exit status {daemon.returncode}")
                    check(state.read_bytes() == saved, "retry changed the saved desired state")
                check(not sock.exists(), "control socket was not removed")
                check("live-flows=0" in log.read_text(), "fixture handles leaked")
            except Exception:
                print(log.read_text(), flush=True)
                raise
            finally:
                stop(daemon)
        print(f"PASS {fault}: worker stop, isolation, failed/successful retry, exit, snapshot replay")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--lcores", default="0,1")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    library = build / "tests/integration/libdppd_flow_faults.so"
    check(library.is_file(), f"build with -Dtests=true first: {library}")
    for fault in ("create-rollback", "delete-restore"):
        run_case(build, library, args.lcores, fault)


if __name__ == "__main__":
    main()
