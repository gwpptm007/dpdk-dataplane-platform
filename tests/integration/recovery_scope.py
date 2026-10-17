#!/usr/bin/env python3
"""在测试新建的私有网络命名空间内核对正式入口的权限、授权和启动器死亡边界"""

import argparse
import os
from pathlib import Path
import re
import select
import subprocess
import tempfile

from batch_update import check
from health_readiness import clean_environment
from recovery_isolation import wait_for


def run_cases(build):
    environment = clean_environment()
    environment.pop("DPPD_SCOPE_FD", None)
    fixture = build / "tests/integration/test_recovery_scope_fixture"
    before = set(Path("/sys/class/net").iterdir())
    for role in ("monitor", "business", "recovery"):
        for mode in ("normal", "denied", "wrong-namespace", "wrong-role"):
            with tempfile.TemporaryDirectory(prefix="dppd-scope-") as temporary:
                result = subprocess.run([str(fixture), role, str(build), temporary, mode], env=environment,
                                        capture_output=True, text=True, timeout=25)
                output = result.stdout + result.stderr
                check((result.returncode == 0) == (mode == "normal"), output)
                check("scope fixture exec failed" not in output, output)
                if mode != "normal":
                    check("scope refused:" in output, output)
                    check("EAL:" not in output and "monitor ready" not in output, output)
                elif role == "monitor":
                    check("monitor ready" in output and "requests=3" in output, output)
                    shown = subprocess.run([str(build / "dppd-monitor"), "show", temporary + "/session"],
                                           env=environment, capture_output=True, text=True, timeout=5)
                    check(shown.returncode == 0 and "state=stopped" in shown.stdout, shown.stdout + shown.stderr)
                elif role == "business":
                    check("EAL:" in output and Path(temporary, "snapshot").is_file(), output)
                    check(not Path(temporary, "control").exists(), "business leaked its control socket")
                else:
                    check("plan version=1" in output and "executable=no" in output, output)
            print(f"PASS managed scope {role} {mode}", flush=True)

    # 父进程被杀后用 pidfd 验证监护确实退出，不能只依赖日志、PID 数字或旧状态文件
    with tempfile.TemporaryDirectory(prefix="dppd-scope-death-") as temporary:
        directory = Path(temporary)
        log = directory / "supervisor.log"
        child_fd = None
        with log.open("w") as output:
            supervisor = subprocess.Popen([str(fixture), "monitor", str(build), temporary, "hold"],
                                          env=environment, stdout=output, stderr=subprocess.STDOUT)
        try:
            wait_for(supervisor, lambda: "monitor ready" in log.read_text(), "managed monitor readiness")
            child = int(re.search(r"scope-fixture child=(\d+)", log.read_text()).group(1))
            child_fd = os.pidfd_open(child)
            exited = select.poll()
            exited.register(child_fd, select.POLLIN)
            supervisor.kill()
            supervisor.wait(timeout=5)
            check(exited.poll(5000), "managed monitor survived supervisor death")
            shown = subprocess.run([str(build / "dppd-monitor"), "show", str(directory / "session")],
                                   env=environment, capture_output=True, text=True, timeout=5)
            check(shown.returncode == 0 and "state=interrupted" in shown.stdout and
                  "reason=writer-missing" in shown.stdout, shown.stdout + shown.stderr)
        finally:
            if supervisor.poll() is None:
                supervisor.kill()
                supervisor.wait(timeout=5)
            if child_fd is not None:
                os.close(child_fd)
    check(set(Path("/sys/class/net").iterdir()) == before, "scope changed host interfaces")
    print("PASS managed scope supervisor death: child pidfd exited, session interrupted, host interfaces unchanged",
          flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    arguments = parser.parse_args()
    check(os.geteuid() == 0, "managed scope integration requires root for fresh network namespaces")
    run_cases(arguments.build_dir.resolve())


if __name__ == "__main__":
    main()
