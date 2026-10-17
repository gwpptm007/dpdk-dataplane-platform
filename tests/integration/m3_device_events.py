#!/usr/bin/env python3
"""正式 daemon 的设备事件隔离验收，flow/shared 仅为测试堆对象，不代表真实硬件通过"""

import argparse
import errno
import hashlib
import json
from pathlib import Path
import re
import signal
import subprocess
import tempfile

from batch_update import check, fields
from health_readiness import clean_environment, daemon_command
from recovery_isolation import wait_for


def fingerprint(path):
    """同时保存内容和文件身份，发现偷偷重写相同快照的行为"""
    status = path.stat()
    return path.read_bytes(), status.st_ino, status.st_mtime_ns, status.st_size


def terminate(process):
    """用正式 SIGTERM 退出路径等待清理，不把强杀当作回调已经排空"""
    if process is not None and process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
            raise AssertionError("daemon did not finish SIGTERM cleanup")


def fixture_environment(build, environment, trigger, event):
    """故障库只加载到独立 daemon，ASan 运行库必须排在所有测试库前面"""
    libraries = ["libdppd_flow_faults.so", "libdppd_shared_recovery_faults.so", "libdppd_removal_faults.so"]
    preload = [str(build / "tests/integration" / name) for name in libraries]
    linked = subprocess.check_output(["ldd", str(build / "dppd")], text=True, env=environment)
    sanitizer = re.search(r"\blibasan\.so\S*\s+=>\s+(/\S+)", linked)
    if sanitizer is not None:
        preload.insert(0, sanitizer.group(1))
    result = dict(environment, LD_PRELOAD=":".join(preload), DPPD_TEST_FLOW_FAULT_MODE="create-rollback",
                  DPPD_TEST_SHARED_RECOVERY_MODE="normal", DPPD_TEST_REMOVAL_MODE="callback",
                  DPPD_TEST_REMOVAL_FILE=str(trigger), DPPD_TEST_REMOVAL_PORT="1",
                  DPPD_TEST_DEVICE_EVENT="remove" if event == "normal" else event)
    if sanitizer is not None:
        # 隔离故意保留未知句柄，只有进程结束才回收夹具堆，不能当作生产代码已销毁对象
        result["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0:abort_on_error=1"
        result["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1"
    return result


def run_case(build, destination, event):
    """正常退出作对照，三种真实 DPDK 分发事件都必须保留未知旧对象和磁盘保护"""
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "commands.log").write_text("")
    report = {"success": False, "event": event, "scope": "formal_daemon_with_test_only_heap_objects",
              "hardware_acceptance": "not_run"}
    with tempfile.TemporaryDirectory(prefix="dppd-dev-events-") as temporary:
        directory = Path(temporary)
        (directory / "storage").mkdir()
        sock, state, guard = directory / "ctl.sock", directory / "storage/state.bin", directory / "recovery.bin"
        trigger = directory / "event"
        environment = clean_environment()
        daemon_environment = fixture_environment(build, environment, trigger, event)
        command = daemon_command(build, directory, 1) + ["--recovery-path", str(guard)]
        daemon = None

        def invoke(arguments, error=None):
            """保留完整 CLI 请求和原始输出，明确区分业务拒绝与传输失败"""
            result = subprocess.run(list(map(str, arguments)), env=environment, capture_output=True,
                                    text=True, timeout=10)
            with (destination / "commands.log").open("a") as output:
                output.write(f"$ {' '.join(map(str, arguments))}\nexit={result.returncode}\n")
                output.write(result.stdout)
                output.write(result.stderr)
                output.write("\n")
            check(result.returncode == (0 if error is None else 1), result.stdout + result.stderr)
            if error is not None:
                check(f"({-error})" in result.stderr, result.stdout + result.stderr)
                check("transport failed" not in result.stderr, "management socket disappeared")
            return result.stdout

        def ctl(*arguments, error=None):
            """真实客户端使用干净环境，绝不把 daemon 的测试库加载到管理工具"""
            return invoke([build / "dppctl", "--socket", sock, *arguments], error)

        def assert_objects(status):
            """逻辑账本和未知驱动对象都保留，隔离不能伪装成已经清理"""
            for name, value in {"desired-rules": "2", "actual-rules": "2",
                                "desired-shared": "1", "actual-shared": "1"}.items():
                check(status[name] == value, str(status))

        try:
            with (destination / "daemon.log").open("w") as output:
                daemon = subprocess.Popen(command, env=daemon_environment, stdout=output, stderr=subprocess.STDOUT)
            wait_for(daemon, sock.exists, "management socket")
            wait_for(daemon, lambda: fields(ctl("health"))["ready"] == "yes", "worker readiness")
            generations = {}
            for rule_id, port_id in ((19001, 0), (19002, 1)):
                generations[rule_id] = fields(ctl("apply-drop", rule_id, port_id, 0, 0, "require"))["generation"]
            shared_generation = fields(ctl("shared-count-create", 900, 0, "ingress"))["generation"]
            check("hits=0 bytes=0" in ctl("shared-count-query", 900, shared_generation), "shared fixture not active")
            ctl("count", 19001, generations[19001], error=errno.ENODATA)
            initial = fields(ctl("device-status"))
            check(initial["state"] == "running" and initial["sequence"] == "0" and
                  initial["quarantined"] == "no" and initial["workers-stopped"] == "no", str(initial))
            assert_objects(initial)
            saved_state = fingerprint(state)
            saved_guard = fingerprint(guard)
            (destination / "snapshot.bin").write_bytes(saved_state[0])

            if event != "normal":
                trigger.touch()
                wait_for(daemon, lambda: fields(ctl("device-status"))["state"] == "isolated", "event isolation")
                isolated_text = ctl("device-status")
                (destination / "isolated-status.txt").write_text(isolated_text)
                isolated = fields(isolated_text)
                check(isolated["sequence"] == "1" and isolated["approved-sequence"] == "0" and
                      isolated["quarantined"] == "yes" and isolated["workers-stopped"] == "yes", str(isolated))
                check(isolated["planned"] == "no" and isolated["resets"] == "0", str(isolated))
                for current in ("reset", "remove", "destroy"):
                    name = "removal" if current == "remove" else current
                    check(int(isolated[f"{name}-events"], 16) == (2 if current == event else 0), str(isolated))
                assert_objects(isolated)
                blocked = [("count", 19001, generations[19001]), ("probe-drop", 19003, 0, "refresh"),
                           ("shared-count-query", 900, shared_generation), ("apply-drop", 19003, 0, 0, 0, "require"),
                           ("delete", 19001, generations[19001]), ("shared-count-create", 901, 0, "ingress"),
                           ("shared-count-delete", 900, shared_generation), ("reconcile-retry",)]
                for arguments in blocked:
                    ctl(*arguments, error=errno.EUCLEAN)
                ctl("device-reset", isolated["sequence"], error=errno.EUCLEAN if event == "reset" else errno.ENODEV)
                health = fields(ctl("health"))
                check(health["live"] == "yes" and health["ready"] == "no" and health["workers"] == "0/1",
                      str(health))
                check(fields(ctl("shared-count-get", 900, shared_generation))["references"] == "0",
                      "read-only shared diagnostics lost")
                final_status = fields(ctl("device-status"))
                assert_objects(final_status)
                check(final_status["sequence"] == "1" and final_status["quarantined"] == "yes", str(final_status))
                check(fingerprint(guard) == saved_guard, "rejected command changed recovery evidence")
            check(fingerprint(state) == saved_state, "event or read-only request rewrote snapshot")
            terminate(daemon)
            log = (destination / "daemon.log").read_text()
            check(daemon.returncode == (0 if event == "normal" else 1), log)
            check(not sock.exists(), "management socket leaked")
            check(fingerprint(state) == saved_state, "exit rewrote desired snapshot")
            record_text = invoke([build / "dppd-recovery", "show", guard])
            (destination / "recovery.txt").write_text(record_text)
            (destination / "recovery.bin").write_bytes(guard.read_bytes())
            record = fields(record_text.splitlines()[0])
            if event == "normal":
                check(record["state"] == "clean" and "[dppd-test] live-flows=0" in log, record_text + log)
                check("shared-count-destroy live=0" in log and "available=1024 capacity=1024 in-use=0" in log, log)
            else:
                check(record["state"] == "external-reconciliation-required" and "format=7" in record_text, record_text)
                objects = [fields(line) for line in record_text.splitlines() if line.startswith("object ")]
                attempts = {fields(line)["attempt"]: fields(line) for line in record_text.splitlines()
                            if line.startswith("attempt=")}
                check(sum(item["kind"] == "flow" for item in objects) == 2 and
                      sum(item["kind"] == "shared-count" for item in objects) == 1, record_text)
                for item in objects:
                    attempt = attempts[item["attempt"]]
                    check(attempt["phase"] == "created" and attempt["remove-error"] == "0", record_text)
                    if item["kind"] == "shared-count":
                        check(item["domain"] == "ingress" and attempt["rule"] == "900" and
                              attempt["generation"] == shared_generation, record_text)
                    else:
                        # 旧 flow 回调没有完整 IR 域证据，保留 unknown，不能按 DROP 推测 ingress
                        rule_id = int(attempt["rule"])
                        check(rule_id in generations and attempt["generation"] == generations[rule_id], record_text)
                check("[dppd-test] live-flows=2" in log and "shared-count-destroy" not in log, log)
                check("[removal-fixture] callback port=1 delivered" in log and
                      "[removal-fixture] dispatch port=1 finished" in log and
                      "[removal-fixture] unregister port=1 rc=0" in log, log)
                before_restart = fingerprint(state), fingerprint(guard)
                restarted = subprocess.run(command, env=daemon_environment, capture_output=True, text=True, timeout=10)
                (destination / "restart-blocked.log").write_text(restarted.stdout + restarted.stderr)
                check(restarted.returncode == 1 and "requires external reconciliation" in restarted.stderr,
                      restarted.stdout + restarted.stderr)
                check("[dppd] port=" not in restarted.stdout and "rule snapshot ready:" not in restarted.stderr,
                      "blocked restart configured ports or replayed rules")
                check((fingerprint(state), fingerprint(guard)) == before_restart, "blocked restart modified evidence")
            report.update(success=True, exit_code=daemon.returncode, sequence=0 if event == "normal" else 1,
                          snapshot_sha256=hashlib.sha256(saved_state[0]).hexdigest(),
                          snapshot_inode=saved_state[1], snapshot_mtime_ns=saved_state[2],
                          fixture_live_flows_on_exit=0 if event == "normal" else 2,
                          fixture_shared_destroy_logged=event == "normal", snapshot_unchanged=True,
                          typed_guard_state=record["state"], blocked_commands=0 if event == "normal" else 9)
        except Exception:
            if (destination / "daemon.log").exists():
                print((destination / "daemon.log").read_text(), flush=True)
            raise
        finally:
            try:
                terminate(daemon)
            finally:
                (destination / "report.json").write_text(json.dumps(report, indent=2))
    print(f"PASS formal device event={event}: worker/generation/guard/snapshot isolation; hardware=not_run", flush=True)
    return report


def main():
    """四组只使用 net_ring 和测试堆对象，原始日志与 JSON 保存在本轮 build 内"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    arguments = parser.parse_args()
    build = arguments.build_dir.resolve()
    destination = build / "m3-device-events"
    destination.mkdir(parents=True, exist_ok=True)
    report = {"success": False, "hardware_acceptance": "not_run", "cases": []}
    try:
        for event in ("normal", "reset", "remove", "destroy"):
            report["cases"].append(run_case(build, destination / event, event))
        report["success"] = True
    finally:
        (destination / "report.json").write_text(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
