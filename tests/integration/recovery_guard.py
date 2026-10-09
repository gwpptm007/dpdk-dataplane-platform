#!/usr/bin/env python3
"""验证真实进程的安装保护、强制结束、阻止重放和离线清理确认，仅使用测试自有虚拟端口"""

import argparse
import errno
import os
from pathlib import Path
import signal
import subprocess
import tempfile

from batch_update import check, fields
from health_readiness import clean_environment, daemon_command
from recovery_isolation import stop, wait_for


def run_case(build, tap=False):
    """普通用户用显式 flow 夹具，TAP 使用真实驱动，两者都不清空或重新绑定物理网卡"""
    with tempfile.TemporaryDirectory(prefix="dppd-guard-") as temporary:
        directory = Path(temporary)
        storage = directory / "storage"
        storage.mkdir()
        guard, state, sock = directory / "recovery.bin", storage / "state.bin", directory / "ctl.sock"
        interfaces = (f"dg{os.getpid()}a", f"dg{os.getpid()}b") if tap else ()
        environment = clean_environment()
        daemon_env = dict(environment)
        if not tap:
            # 此夹具前两次创建及正常退出均无故障，规则只存在于测试进程的堆内存
            daemon_env.update(LD_PRELOAD=str(build / "tests/integration/libdppd_flow_faults.so"),
                              DPPD_TEST_FLOW_FAULT_MODE="create-rollback")
        command = daemon_command(build, directory, 1, interfaces) + ["--recovery-path", str(guard)]
        daemon = None

        def run(arguments, error=None):
            """CLI 不加载 flow 夹具，所有失败都检查明确退出码和必要的 errno"""
            result = subprocess.run(list(map(str, arguments)), env=environment,
                                    capture_output=True, text=True, timeout=10)
            if error is None:
                check(result.returncode == 0, result.stdout + result.stderr)
            else:
                check(result.returncode != 0, result.stdout + result.stderr)
                if error:
                    check(f"({-error})" in result.stderr, result.stderr)
            return result.stdout

        def ctl(*arguments, error=None):
            """管理查询和安装都使用正式客户端"""
            return run([build / "dppctl", "--socket", sock, *arguments], error)

        def tool(*arguments, error=None):
            """离线工具从磁盘读取或确认记录，不把 daemon 内存作为跨进程证据"""
            return run([build / "dppd-recovery", *arguments], error)

        def show():
            """首行描述是否还需外部核对，后续行保留各端口首次尝试的身份"""
            text = tool("show", guard)
            return fields(text.splitlines()[0]), text

        def start(name):
            """正常启动必须进入真实工作线程就绪，不能仅凭管理 socket 存在判断成功"""
            log = directory / name
            with log.open("w") as output:
                process = subprocess.Popen(command, env=daemon_env, stdout=output, stderr=subprocess.STDOUT)
            try:
                wait_for(process, sock.exists, "management socket")
                wait_for(process, lambda: fields(ctl("health"))["ready"] == "yes", "worker readiness")
            except Exception:
                stop(process)
                raise AssertionError(log.read_text())
            return process, log

        def killed(process):
            """等待本测试进程确实结束后，才删除它未能清理的管理 socket"""
            process.kill()
            process.wait(timeout=10)
            check(process.returncode == -signal.SIGKILL, str(process.returncode))
            check(all(not Path(f"/sys/class/net/{name}").exists() for name in interfaces),
                  "test TAP still exists after process exit; external cleanup not confirmed")
            if sock.exists():
                sock.unlink()

        def rejected(arguments, expected):
            """失败启动必须发生在项目的队列配置和快照重放前，不改写恢复记录或期望快照"""
            before_guard, before_state = guard.read_bytes(), state.read_bytes()
            result = subprocess.run(arguments, env=daemon_env, capture_output=True, text=True, timeout=10)
            text = result.stdout + result.stderr
            check(result.returncode != 0 and expected in text, text)
            check("rule snapshot ready:" not in text and "[dppd] port=" not in text, text)
            check(guard.read_bytes() == before_guard and state.read_bytes() == before_state,
                  "rejected startup changed durable data")

        try:
            daemon, log = start("software.log")
            tool("show", guard, error=errno.EBUSY)
            tool("acknowledge-clean", guard, 0, "--external-cleanup-complete", error=errno.EBUSY)
            ctl("apply-drop", 800, 0, 0, 20, "software")
            saved = state.read_bytes()
            killed(daemon)
            status, _ = show()
            check(status["state"] == "clean" and status["revision"] == "0", str(status))

            daemon, log = start("hardware.log")
            check(state.read_bytes() == saved and fields(ctl("get", 800))["generation"] == "1",
                  "software replay changed snapshot")
            second = [argument.replace(f"--file-prefix={directory.name}",
                      f"--file-prefix={directory.name}-second") for argument in command]
            rejected(second, f"recovery guard open failed: {-errno.EBUSY}")
            ctl("apply-drop", 901, 0, 0, 10, "require")
            ctl("apply-drop", 902, 1, 0, 10, "require")
            saved = state.read_bytes()
            stop(daemon)
            check(daemon.returncode == 0, log.read_text())
            status, _ = show()
            check(status["state"] == "clean" and int(status["revision"]) == 3, str(status))

            daemon, log = start("crashed.log")
            check(fields(ctl("list").splitlines()[0])["total"] == "3", "three rules not replayed")
            killed(daemon)
            status, text = show()
            revision = int(status["revision"])
            check(status["state"] == "external-reconciliation-required" and status["ports"] == "2", text)
            check("port=0 device=net_" in text and "port=1 device=net_" in text, text)
            check("first-rule=901" in text and "first-rule=902" in text, text)
            rejected(command, "prior hardware activity requires external reconciliation")
            rejected(command, "prior hardware activity requires external reconciliation")
            rejected(command + ["--state-path", str(storage / "other.bin")],
                     f"recovery guard open failed: {-errno.EXDEV}")
            unchanged = guard.read_bytes()
            tool("acknowledge-clean", guard, revision, error=0)
            tool("acknowledge-clean", guard, revision - 1, "--external-cleanup-complete", error=errno.ESTALE)
            tool("acknowledge-clean", guard, "-1", "--external-cleanup-complete", error=0)
            check(guard.read_bytes() == unchanged, "invalid acknowledgement changed recovery record")
            damaged = bytearray(unchanged)
            damaged[80] ^= 1
            guard.write_bytes(damaged)
            rejected(command, f"recovery guard open failed: {-errno.EBADMSG}")
            guard.write_bytes(unchanged)
            # 已确认子进程结束、测试堆对象消失或自有 TAP 接口消失，才确认本次测试的外部清理
            tool("acknowledge-clean", guard, revision, "--external-cleanup-complete")
            check(show()[0]["state"] == "clean" and state.read_bytes() == saved, "ack changed snapshot")
            tool("acknowledge-clean", guard, revision, "--external-cleanup-complete", error=errno.ESTALE)

            daemon, log = start("recovered.log")
            check(fields(ctl("list").splitlines()[0])["total"] == "3", "desired state not recovered")
            stop(daemon)
            check(daemon.returncode == 0 and show()[0]["state"] == "clean", log.read_text())

            daemon, log = start("path-failure.log")
            backup = directory / "renamed-recovery.bin"
            guard.rename(backup)
            ctl("apply-drop", 903, 0, 0, 15, "require", error=errno.ENOENT)
            check(state.read_bytes() == saved, "unrecorded create changed desired rules")
            backup.rename(guard)
            ctl("apply-drop", 903, 0, 0, 15, "require", error=errno.EUCLEAN)
            stop(daemon)
            check(daemon.returncode != 0 and "could not record completed cleanup" in log.read_text(), log.read_text())
            status, _ = show()
            check(status["state"] == "external-reconciliation-required", str(status))
            check(all(not Path(f"/sys/class/net/{name}").exists() for name in interfaces), "TAP leak")
            tool("acknowledge-clean", guard, status["revision"], "--external-cleanup-complete")
            check(not sock.exists(), "management socket leak")
            check("available=" + str(8191 if tap else 1024) in log.read_text(), log.read_text())
            if not tap:
                check("live-flows=0" in log.read_text(), log.read_text())
        finally:
            if daemon is not None:
                stop(daemon)
        print(f"PASS {'TAP' if tap else 'ring fixture'} recovery guard: software crash replay, "
              "exclusive lock, durable pre-create markers, clean exit, SIGKILL gate, device identity, "
              "corruption/binding/stale acknowledgement rejection, external cleanup acknowledgement, "
              "snapshot replay, path failure, resource cleanup", flush=True)


def main():
    """TAP 模式单独由具备创建虚拟接口权限的用户运行，普通模式不需要 root"""
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--tap", action="store_true")
    arguments = parser.parse_args()
    check(not arguments.tap or os.geteuid() == 0, "TAP test needs root")
    run_case(arguments.build_dir.resolve(), arguments.tap)


if __name__ == "__main__":
    main()
