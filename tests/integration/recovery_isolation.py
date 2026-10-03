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
from telemetry_client import Telemetry


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
            telemetry = None
            try:
                wait_for(daemon, sock.exists, "management socket", seconds=15)
                telemetry = Telemetry(daemon, log)
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
                    # telemetry 从已发布副本读取隔离状态，同时保留原始错误和补偿错误
                    observed = fields(ctl("rule-metrics"))
                    telemetry.wait(lambda: telemetry.query("/dppd/rule_failures")["operations"] ==
                                   int(observed["operations"]))
                    report = telemetry.query("/dppd/rule_failures")
                    check(report["operation"] == "update-batch" and report["rule_count"] == 2,
                          str(report))
                    check(report["response_error"] == -errno.EUCLEAN and not report["last_applied"],
                          str(report))
                    check(report["stage"] == ("commit" if fault == "create-rollback" else "remove"),
                          str(report))
                    check(report["cause_error"] == (-errno.EIO if fault == "create-rollback" else -errno.EFAULT)
                          and report["compensation_error"] == (-errno.EFAULT if fault == "create-rollback"
                                                               else -errno.EIO), str(report))
                    page = telemetry.query("/dppd/rules")
                    check(page["total"] == 2 and page["unavailable_rules"] == 2 and
                          page["hardware_objects"] == int(status["residual-objects"]), str(page))
                    row = telemetry.query("/dppd/rule", 700)
                    check(row["status_error"] == -errno.EUCLEAN and row["backend"] == "unknown", str(row))
                    # 隔离仍可证明管理线程存活，但停止的转发线程不能被报告为已就绪
                    health = fields(ctl("health"))
                    check(health["live"] == "yes" and health["ready"] == "no", str(health))
                    check(health["workers"] == "0/1" and "recovery-required" in health["reasons"],
                          str(health))
                    readiness = subprocess.run([str(build / "dppctl"), "--socket", str(sock), "ready"],
                                               env=client_env, capture_output=True, text=True, timeout=5)
                    check(readiness.returncode == 1 and fields(readiness.stdout)["ready"] == "no",
                          readiness.stdout + readiness.stderr)
                    # 隔离期间只允许读取已有画像，不能重新探测驱动或改变缓存版本
                    profile = ctl("capability-show", 0)
                    check(fields(profile)["started"] == "yes" and
                          int(fields(profile)["validations"]) > 0, profile)
                    check(ctl("capability-show", 0) == profile, "profile changed cached observations")
                    for request in [("ping",), ("list",), ("get", 700), ("rule-status", 700),
                                    ("probe-drop", 702, 0), ("probe-cache-clear", 0),
                                    ("persistence-flush",),
                                    ("update-drop-batch", 0, 30, "require", 700, 1, 701, 2)]:
                        ctl(*request, error=errno.EUCLEAN)
                    check(state.read_bytes() == saved, "isolation changed the old snapshot")
                    check(fields(ctl("rule-metrics")) == observed and
                          telemetry.query("/dppd/rule_failures") == report,
                          "rejected protocol requests changed completed control metrics")
                    ctl("reconcile-retry", error=errno.EUCLEAN)
                    telemetry.wait(lambda: telemetry.query("/dppd/rule_failures")["operations"] ==
                                   report["operations"] + 1)
                    retried_failure = telemetry.query("/dppd/rule_failures")
                    check(retried_failure["stage"] == "reconcile" and
                          retried_failure["cause_error"] == -errno.EFAULT, str(retried_failure))
                    status = fields(ctl("reconcile-status"))
                    check(status["state"] == "reconciliation-required" and
                          status["residual-objects"] == "1", str(status))
                    ctl("ping", error=errno.EUCLEAN)
                    telemetry.close()
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
                if telemetry is not None:
                    telemetry.close()
                stop(daemon)
        print(f"PASS {fault}: worker stop, isolation, failed/successful retry, exit, snapshot replay")


def run_startup_case(build, library, lcores):
    """非空快照重放的回滚失败时不启动 worker，但仍可查询失败原因和清理残留对象"""
    with tempfile.TemporaryDirectory(prefix="dppd-replay-telemetry-") as temporary:
        directory = Path(temporary)
        sock, state = directory / "ctl.sock", directory / "state.bin"
        command = [str(build / "dppd"), "-l", lcores, "--no-huge", "--no-pci", "-m", "64",
                   "--file-prefix=" + directory.name, "--vdev=net_ring0", "--vdev=net_ring1",
                   "--", "--ports", "0,1", "--queues", "1", "--mbufs", "1024", "--cache", "0",
                   "--rule-capacity", "4", "--control-socket", str(sock), "--state-path", str(state)]
        client_env = {key: value for key, value in os.environ.items()
                      if key != "LD_PRELOAD" and not key.startswith("DPPD_TEST_")}
        client_env["LC_ALL"] = "C"

        def ctl(*tokens, error=None):
            """只向本实例管理接口发送请求，CLI 自身不加载故障注入库"""
            result = subprocess.run([str(build / "dppctl"), "--socket", str(sock), *map(str, tokens)],
                                    env=client_env, capture_output=True, text=True, timeout=5)
            check(result.returncode == (0 if error is None else 1), result.stdout + result.stderr)
            if error is not None:
                check(f"({-error})" in result.stderr, result.stderr)
            return result.stdout

        def start(name, mode):
            """首次建表和最后重放不触发故障，中间启动独立注入重放错误"""
            log = directory / name
            environment = dict(client_env, LD_PRELOAD=str(library), DPPD_TEST_FLOW_FAULT_MODE=mode)
            with log.open("w") as output:
                daemon = subprocess.Popen(command, env=environment, stdout=output, stderr=subprocess.STDOUT)
            return daemon, log

        daemon, log = start("baseline.log", "create-rollback")
        try:
            wait_for(daemon, sock.exists, "baseline management socket")
            wait_for(daemon, lambda: fields(ctl("health"))["ready"] == "yes", "baseline readiness")
            ctl("apply-drop-batch", 0, 700, 701)
            saved = state.read_bytes()
        finally:
            stop(daemon)
        check(daemon.returncode == 0 and "live-flows=0" in log.read_text(), log.read_text())

        daemon, log = start("isolated.log", "replay-rollback")
        telemetry = None
        try:
            wait_for(daemon, sock.exists, "startup isolation management socket")
            telemetry = Telemetry(daemon, log)
            report = telemetry.query("/dppd/rule_failures")
            check(report["operations"] == 1 and report["failed"] == 1 and report["operation"] == "restore",
                  str(report))
            check(report["stage"] == "commit" and report["rule_id"] == 701 and report["generation"] == 2,
                  str(report))
            check(report["cause_error"] == -errno.EIO and report["response_error"] == -errno.EUCLEAN and
                  report["compensation_error"] == -errno.EFAULT and report["compensation_rule_id"] == 700,
                  str(report))
            check(not report["last_applied"] and report["rule_count"] == 2, str(report))
            observed = fields(ctl("rule-metrics"))
            check(observed["operations"] == "1" and observed["cause-error"] == str(-errno.EIO), str(observed))
            health = fields(ctl("health"))
            check(health["ready"] == "no" and health["workers"].startswith("0/"), str(health))
            page = telemetry.query("/dppd/rules")
            check(page["total"] == 0 and page["hardware_objects"] == 1 and page["recovery_state"] != 0,
                  str(page))
            check(telemetry.query("/dppd/rule", 700) is None, "unpublished actual object became desired state")
            ctl("apply-drop", 702, 0, 0, error=errno.EUCLEAN)
            check(fields(ctl("rule-metrics")) == observed and state.read_bytes() == saved,
                  "blocked write changed snapshot")
            ctl("reconcile-retry", error=errno.EUCLEAN)
            telemetry.wait(lambda: telemetry.query("/dppd/rule_failures")["operations"] == 2)
            report = telemetry.query("/dppd/rule_failures")
            check(report["stage"] == "reconcile" and report["cause_error"] == -errno.EFAULT, str(report))
            telemetry.close()
            ctl("reconcile-retry")
            daemon.wait(timeout=10)
            check(daemon.returncode == 1 and state.read_bytes() == saved and not sock.exists(), log.read_text())
            check("live-flows=0" in log.read_text(), "startup isolation leaked handles")
        except Exception:
            print(log.read_text(), flush=True)
            raise
        finally:
            if telemetry is not None:
                telemetry.close()
            stop(daemon)

        daemon, log = start("recovered.log", "create-rollback")
        try:
            wait_for(daemon, sock.exists, "recovered socket")
            wait_for(daemon, lambda: fields(ctl("health"))["ready"] == "yes", "recovered readiness")
            check(fields(ctl("list"))["total"] == "2" and state.read_bytes() == saved,
                  "successful restart changed original snapshot")
            check(fields(ctl("rule-metrics"))["operations"] == "1", "restart reused failed epoch")
        finally:
            stop(daemon)
        check(daemon.returncode == 0 and "live-flows=0" in log.read_text(), log.read_text())
        print("PASS startup replay: no worker start, telemetry failure details, isolated retries, clean replay",
              flush=True)


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
    run_startup_case(build, library, args.lcores)


if __name__ == "__main__":
    main()
