#!/usr/bin/env python3
"""在真实 TAP 创建前后强制结束进程，并验证无 handle 的创建失败不能自动确认清理"""

import argparse
import errno
import os
from pathlib import Path
import signal
import subprocess
import tempfile

from batch_update import check, fields
from health_readiness import clean_environment, daemon_command
from recovery_isolation import wait_for


def run_case(build, mode):
    """仅测试进程加载暂停夹具，CLI、离线工具和内核查询都使用正常环境"""
    environment = clean_environment()
    owned = {}
    daemon = client = None

    def run(*arguments, error=None):
        """错误场景同时核对失败退出和原始 errno"""
        result = subprocess.run(list(map(str, arguments)), env=environment,
                                capture_output=True, text=True, timeout=10)
        check(result.returncode == 0 if error is None else result.returncode != 0, result.stdout + result.stderr)
        if error:
            check(f"({-error})" in result.stderr, result.stderr)
        return result.stdout

    def remove_interfaces():
        """按创建时保存的索引核对后删除自有 TAP，不接管同名替换接口"""
        for name, index in tuple(owned.items()):
            check(Path(f"/sys/class/net/{name}/ifindex").read_text() == index, "test interface replaced")
            run("ip", "link", "delete", "dev", name)
            del owned[name]

    with tempfile.TemporaryDirectory(prefix="dppd-attempt-") as temporary:
        directory = Path(temporary)
        (directory / "storage").mkdir()
        guard, sock, log = directory / "recovery", directory / "ctl.sock", directory / "daemon.log"
        interfaces = (f"da{os.getpid()}a", f"da{os.getpid()}b")
        command = daemon_command(build, directory, 1, interfaces) + ["--recovery-path", str(guard)]
        try:
            for name in interfaces:
                check(not Path(f"/sys/class/net/{name}").exists(), "existing interface")
                run("ip", "tuntap", "add", "dev", name, "mode", "tap", "multi_queue")
                owned[name] = Path(f"/sys/class/net/{name}/ifindex").read_text()
            daemon_env = dict(environment, LD_PRELOAD=str(build / "tests/integration/libdppd_recovery_attempt_faults.so"),
                              DPPD_TEST_RECOVERY_WINDOW=mode)
            with log.open("w") as output:
                daemon = subprocess.Popen(command, env=daemon_env, stdout=output, stderr=subprocess.STDOUT)
            wait_for(daemon, sock.exists, "management socket")
            wait_for(daemon, lambda: fields(run(build / "dppctl", "--socket", sock, "health"))["ready"] == "yes",
                     "worker readiness")
            saved = (directory / "storage/state.bin").read_bytes()
            client = subprocess.Popen([str(build / "dppctl"), "--socket", str(sock), "apply-drop",
                                       "970", "0", "0", "9", "require"], env=environment,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if mode == "fail-empty":
                output, error = client.communicate(timeout=10)
                check(client.returncode != 0 and f"({-errno.EIO})" in error, output + error)
                daemon.send_signal(signal.SIGINT)
                daemon.wait(timeout=10)
                check(daemon.returncode != 0 and "could not record completed cleanup" in log.read_text(), log.read_text())
            else:
                wait_for(daemon, lambda: "State:\tT" in Path(f"/proc/{daemon.pid}/status").read_text(),
                         "create window pause")
                daemon.kill()
                daemon.wait(timeout=10)
                check(daemon.returncode == -signal.SIGKILL, log.read_text())
                client.communicate(timeout=10)
                if sock.exists():
                    sock.unlink()
            show = run(build / "dppd-recovery", "show", guard)
            revision = fields(show.splitlines()[0])["revision"]
            check("state=external-reconciliation-required" in show and "attempts=1" in show, show)
            phase = "create-failed" if mode == "fail-empty" else "intent"
            check(f"phase={phase}" in show and "candidate " not in show, show)
            check((directory / "storage/state.bin").read_bytes() == saved, "unfinished request published snapshot")
            inspected = run(build / "dppd-recovery", "inspect", guard, revision)
            expected = 1 if mode == "after" else 0
            check(f"complete=yes error=0 filters={expected}" in inspected, inspected)
            check("status=no-candidate owner=unknown" in inspected, inspected)
            # 即使当前本地列表为空，记录仍待确认，测试先明确删除全部自有接口再确认
            remove_interfaces()
            run(build / "dppd-recovery", "acknowledge-clean", guard, revision, "--external-cleanup-complete")
        finally:
            for process in (client, daemon):
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait(timeout=10)
            remove_interfaces()
    print(f"PASS recovery attempt {mode}: durable intent/result, kernel residual count, "
          "unproven ownership, unchanged snapshot, no automatic acknowledgement", flush=True)


def main():
    """三个场景只使用自建 TAP，物理网卡不参与测试"""
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    arguments = parser.parse_args()
    check(os.geteuid() == 0, "TAP recovery attempt tests need root")
    for mode in ("before", "after", "fail-empty"):
        run_case(arguments.build_dir.resolve(), mode)


if __name__ == "__main__":
    main()
