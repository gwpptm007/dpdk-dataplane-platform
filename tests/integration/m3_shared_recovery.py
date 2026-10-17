#!/usr/bin/env python3
"""正式 daemon 的共享 COUNT 恢复保护验收，驱动对象仅为测试堆内存，不计为硬件通过"""

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
from recovery_isolation import stop, wait_for


def run_case(build, destination):
    """保护文件、管理协议和重启均走正式入口，只替换真实设备无法提供的 indirect 对象"""
    report = {"success": False, "scope": "formal_process_with_test_only_indirect", "hardware_acceptance": "not_run"}
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="dppd-shared-journal-") as temporary:
        directory = Path(temporary)
        (directory / "storage").mkdir()
        sock, state, guard = directory / "ctl.sock", directory / "storage/state.bin", directory / "recovery.bin"
        environment = clean_environment()
        preload = [str(build / "tests/integration/libdppd_shared_recovery_faults.so")]
        # ASan 构建要求运行库先于测试故障库加载，普通构建不额外依赖 sanitizer
        linked = subprocess.check_output(["ldd", str(build / "dppd")], text=True, env=environment)
        sanitizer = re.search(r"\blibasan\.so\S*\s+=>\s+(/\S+)", linked)
        if sanitizer is not None:
            preload.insert(0, sanitizer.group(1))
        daemon_env = dict(environment, LD_PRELOAD=":".join(preload),
                          DPPD_TEST_SHARED_RECOVERY_MODE="normal")
        command = daemon_command(build, directory, 1) + ["--recovery-path", str(guard)]
        daemon = None

        def invoke(arguments, error=None):
            result = subprocess.run(list(map(str, arguments)), env=environment, capture_output=True, text=True, timeout=10)
            check(result.returncode == (0 if error is None else 1), result.stdout + result.stderr)
            if error is not None:
                check(f"({-error})" in result.stderr, result.stderr)
            return result.stdout

        def ctl(*arguments, error=None):
            return invoke([build / "dppctl", "--socket", sock, *arguments], error)

        def tool(*arguments, error=None):
            return invoke([build / "dppd-recovery", *arguments], error)

        def start(name):
            with (destination / name).open("w") as output:
                process = subprocess.Popen(command, env=daemon_env, stdout=output, stderr=subprocess.STDOUT)
            try:
                wait_for(process, sock.exists, "management socket")
                wait_for(process, lambda: fields(ctl("health"))["ready"] == "yes", "worker readiness")
            except Exception:
                stop(process)
                raise AssertionError((destination / name).read_text())
            return process

        def inspect(name):
            text = tool("show", guard)
            (destination / name).write_text(text)
            return fields(text.splitlines()[0]), text

        def blocked_restart(name):
            before = guard.read_bytes(), state.read_bytes()
            result = subprocess.run(command, env=daemon_env, capture_output=True, text=True, timeout=10)
            (destination / name).write_text(result.stdout + result.stderr)
            check(result.returncode != 0 and "requires external reconciliation" in result.stderr,
                  result.stdout + result.stderr)
            check("rule snapshot ready:" not in result.stderr and "[dppd] port=" not in result.stdout,
                  "blocked process configured ports or replayed state")
            check(before == (guard.read_bytes(), state.read_bytes()), "blocked startup changed durable state")

        try:
            daemon = start("normal.log")
            created = fields(ctl("shared-count-create", 900, 0, "ingress"))
            generation = created["generation"]
            count = fields(ctl("shared-count-query", 900, generation))
            check(count["hits"] == "0" and count["bytes"] == "0", str(count))
            ctl("shared-count-delete", 900, generation)
            stop(daemon)
            check(daemon.returncode == 0, (destination / "normal.log").read_text())
            status, text = inspect("normal-record.txt")
            check(status["state"] == "clean" and "format=7" in text and "monitor-required=yes" not in text, text)

            daemon = start("crashed.log")
            generation = fields(ctl("shared-count-create", 900, 0, "ingress"))["generation"]
            saved = state.read_bytes()
            daemon.kill()
            daemon.wait(timeout=10)
            check(daemon.returncode == -signal.SIGKILL, str(daemon.returncode))
            sock.unlink(missing_ok=True)
            status, text = inspect("crash-record.txt")
            check(status["state"] == "external-reconciliation-required" and "first-kind=shared-count" in text and
                  "kind=shared-count domain=ingress" in text and "phase=created" in text, text)
            blocked_restart("crash-restart-blocked.log")
            plan = invoke([build / "dppd-recovery", "plan", guard, status["revision"]], errno.ENOTSUP)
            (destination / "crash-plan.txt").write_text(plan)
            check("indirect-object-requires-external-cleanup" in plan and "candidates=0" in plan, plan)
            # 本夹具对象随已结束进程的堆一起消失，只有这里有明确外部清理依据，真实 ASIC 不适用
            tool("acknowledge-clean", guard, status["revision"], "--external-cleanup-complete")
            check(state.read_bytes() == saved, "external acknowledgement changed desired snapshot")
            daemon = start("restored.log")
            current = fields(ctl("shared-count-get", 900, generation))
            check(current["generation"] == generation, str(current))
            ctl("shared-count-delete", 900, generation)
            stop(daemon)
            check(daemon.returncode == 0, (destination / "restored.log").read_text())
            check(inspect("restored-record.txt")[0]["state"] == "clean", "restored exit not clean")

            daemon_env["DPPD_TEST_SHARED_RECOVERY_MODE"] = "create-failed"
            daemon = start("failed-create.log")
            saved = state.read_bytes()
            ctl("shared-count-create", 901, 0, "ingress", error=errno.ENOSPC)
            check(state.read_bytes() == saved, "failed create changed desired snapshot")
            stop(daemon)
            check(daemon.returncode != 0, "unknown failed create incorrectly acknowledged clean")
            status, text = inspect("failed-create-record.txt")
            check(status["state"] == "external-reconciliation-required" and "phase=create-failed" in text and
                  "kind=shared-count domain=ingress" in text, text)
            blocked_restart("failed-create-restart-blocked.log")
            tool("acknowledge-clean", guard, status["revision"], "--external-cleanup-complete")
            report.update(success=True, snapshot_sha256=hashlib.sha256(state.read_bytes()).hexdigest(),
                          cases=["normal_lifecycle", "sigkill_restart_gate", "external_ack_exact_replay",
                                 "unconfirmed_failed_create", "indirect_offline_cleanup_rejection"])
        finally:
            if daemon is not None:
                stop(daemon)
            check(not sock.exists(), "management socket leaked")
            (destination / "report.json").write_text(json.dumps(report, indent=2))
    print("PASS shared COUNT formal process recovery: normal cleanup, SIGKILL gate, exact replay, "
          "failed-create protection and indirect offline rejection; hardware=not_run", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    arguments = parser.parse_args()
    build = arguments.build_dir.resolve()
    run_case(build, build / "m3-shared-recovery")


if __name__ == "__main__":
    main()
