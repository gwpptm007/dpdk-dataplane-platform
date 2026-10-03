#!/usr/bin/env python3
"""验证正式进程的存活、就绪、存储故障恢复、线程故障退出和可选 TAP 链路恢复"""

import argparse
import errno
import os
from pathlib import Path
import subprocess
import tempfile

from batch_update import check, fields
from recovery_isolation import stop, wait_for


def clean_environment():
    """确保新进程不继承故障库或其他测试场景的环境变量"""
    environment = {key: value for key, value in os.environ.items()
                   if key != "LD_PRELOAD" and not key.startswith("DPPD_TEST_")}
    environment["LC_ALL"] = "C"
    return environment


def daemon_command(build, directory, queues, interfaces=()):
    """把可用 CPU 映射成连续的逻辑核，仅创建测试实例自己的虚拟端口"""
    cpus = sorted(os.sched_getaffinity(0))
    check(len(cpus) > queues, f"{queues + 1} CPUs required")
    lcores = ",".join(f"{index}@{cpu}" for index, cpu in enumerate(cpus[:queues + 1]))
    # net_ring 不提供 RSS，双队列进程检查改用 net_null，保持正式 daemon 的能力要求
    # 实际双队列报文与逐包内容仍由 software_traffic.py 的独立 net_ring 测试验证
    ports = [f"--vdev=net_tap{index},iface={interface}"
             for index, interface in enumerate(interfaces)] if interfaces else [
                 f"--vdev=net_{'null' if queues > 1 else 'ring'}0",
                 f"--vdev=net_{'null' if queues > 1 else 'ring'}1"]
    # TAP 会为接收队列预先分配报文，两个端口需要大于单队列描述符总数的池容量
    mbufs = 8191 if interfaces else 1024
    return [str(build / "dppd"), f"--lcores={lcores}", "--main-lcore=0",
            "--no-huge", "--no-pci", "-m", "64", "--no-telemetry",
            f"--file-prefix={directory.name}", *ports, "--", "--ports", "0,1",
            "--queues", str(queues), "--mbufs", str(mbufs), "--cache", "0",
            "--control-socket", str(directory / "ctl.sock"),
            "--state-path", str(directory / "storage/state.bin")]


def run_case(build, queues, tap=False):
    """完整经历正常就绪、写入失败、恢复保存和重启，TAP 场景另外验证 down/up"""
    with tempfile.TemporaryDirectory(prefix="dppd-health-") as temporary:
        directory = Path(temporary)
        storage = directory / "storage"
        storage.mkdir()
        sock, state = directory / "ctl.sock", storage / "state.bin"
        interfaces = (f"dh{os.getpid()}a", f"dh{os.getpid()}b") if tap else ()
        pool_capacity = 8191 if tap else 1024
        environment = clean_environment()
        command = daemon_command(build, directory, queues, interfaces)

        def ctl(*tokens, expected=0):
            """核对真实 CLI 的退出码，再把结果交给调用方检查内容"""
            result = subprocess.run([str(build / "dppctl"), "--socket", str(sock),
                                     *map(str, tokens)], env=environment,
                                    capture_output=True, text=True, timeout=5)
            check(result.returncode == expected, result.stdout + result.stderr)
            return result

        def health():
            """未就绪时 health 仍应成功响应，ready 的失败退出码在各阶段单独核对"""
            status = fields(ctl("health").stdout)
            check(status["live"] == "yes", str(status))
            return status

        def start(log_name):
            """每个进程保存独立日志，便于定位退出和缓冲池回收问题"""
            with (directory / log_name).open("w") as output:
                return subprocess.Popen(command, env=environment, stdout=output,
                                        stderr=subprocess.STDOUT)

        daemon = start("run.log")
        try:
            wait_for(daemon, sock.exists, "management socket")
            wait_for(daemon, lambda: health()["ready"] == "yes", "actual worker readiness")
            initial = fields(ctl("ready").stdout)
            check(initial["workers"] == f"{queues}/{queues}" and initial["ports"] == "2/2",
                  str(initial))
            ctl("apply-filter", 16001, 0, 0, "tcp", "192.168.100.2/32",
                "192.168.100.1/32", "any", 10000, "drop", "count", "software")
            saved = state.read_bytes()
            for _ in range(3):
                check(health()["current-generation"] == "1", "probe changed generation")
                ctl("ready")
            check(state.read_bytes() == saved, "read-only probes changed snapshot")
            check("hits=0 bytes=0" in ctl("count", 16001, 1).stdout, "probe changed COUNT")

            # 用普通文件替换本测试自己的存储目录，稳定产生 ENOTDIR，不依赖用户权限
            backup = directory / "saved-storage"
            storage.rename(backup)
            storage.write_text("test storage is temporarily unavailable\n")
            applied = ctl("apply-drop", 16002, 1, 0, 0, "software", expected=1)
            check(f"({-errno.EUCLEAN})" in applied.stderr, applied.stderr)
            dirty = health()
            check(dirty["ready"] == "no" and dirty["reasons"] == "persistence-dirty", str(dirty))
            check(dirty["persisted-generation"] == "1" and dirty["current-generation"] == "2",
                  str(dirty))
            check(dirty["workers"] == f"{queues}/{queues}" and dirty["rules"] == "2", str(dirty))
            check(fields(ctl("ready", expected=1).stdout)["ready"] == "no", "ready exit mismatch")
            check((backup / "state.bin").read_bytes() == saved, "storage error damaged old snapshot")
            ctl("get", 16002)
            storage.unlink()
            backup.rename(storage)
            ctl("persistence-flush")
            check(health()["ready"] == "yes", "flush did not restore readiness")
            ctl("ready")
            saved = state.read_bytes()

            if tap:
                # 只操作本进程刚创建的 TAP，不修改管理网卡或物理数据口
                subprocess.run(["ip", "link", "set", "dev", interfaces[1], "down"], check=True)
                wait_for(daemon, lambda: health()["links-down"] == "1", "TAP link down")
                down = fields(ctl("ready", expected=1).stdout)
                check(down["ready"] == "no" and down["reasons"] == "link-down", str(down))
                check(down["workers"] == "1/1", str(down))
                check(state.read_bytes() == saved, "link down changed snapshot")
                subprocess.run(["ip", "link", "set", "dev", interfaces[1], "up"], check=True)
                wait_for(daemon, lambda: health()["ready"] == "yes", "TAP readiness recovery")
                ctl("ready")
                check(state.read_bytes() == saved, "link recovery changed snapshot")
        except Exception:
            print((directory / "run.log").read_text(), flush=True)
            raise
        finally:
            stop(daemon)
        check(daemon.returncode == 0 and not sock.exists(), "normal exit/socket cleanup failed")
        check(f"available={pool_capacity} capacity={pool_capacity} in-use=0" in
              (directory / "run.log").read_text(),
              "mbufs not returned")
        failed_probe = ctl("health", expected=1)
        check("transport failed" in failed_probe.stderr and "live=yes" not in failed_probe.stdout,
              "stopped daemon was reported live")

        # 用干净环境重启正式程序，再验证已保存的两条规则与就绪状态
        daemon = start("restarted.log")
        try:
            wait_for(daemon, sock.exists, "restarted management socket")
            wait_for(daemon, lambda: health()["ready"] == "yes", "restarted readiness")
            restored = fields(ctl("ready").stdout)
            check(restored["rules"] == "2" and restored["current-generation"] == "2", str(restored))
            check(state.read_bytes() == saved, "restart changed snapshot")
            check("hits=0 bytes=0" in ctl("count", 16001, 1).stdout, "restarted COUNT not zero")
        finally:
            stop(daemon)
        check(daemon.returncode == 0 and not sock.exists(), "restart cleanup failed")
        check(f"available={pool_capacity} capacity={pool_capacity} in-use=0" in
              (directory / "restarted.log").read_text(),
              "restart mbufs not returned")
        for interface in interfaces:
            check(not Path(f"/sys/class/net/{interface}").exists(), "temporary TAP remains")
        port_kind = "tap" if tap else "null" if queues > 1 else "ring"
        print(f"PASS health ports={port_kind} queues={queues}: "
              "read-only probes, dirty/flush, CLI exits, restart, cleanup" +
              (", link down/up readiness" if tap else ""), flush=True)


def run_worker_failure(build, queues):
    """使用测试专用链接包装，验证正式主循环发现线程注册失败并回收其他队列"""
    with tempfile.TemporaryDirectory(prefix="dppd-worker-failure-") as temporary:
        directory = Path(temporary)
        (directory / "storage").mkdir()
        command = daemon_command(build, directory, queues)
        command[0] = str(build / "tests/integration/test_worker_failure")
        environment = dict(clean_environment(), DPPD_TEST_WORKER_FAILURE="registration")
        log_path = directory / "failure.log"
        with log_path.open("w") as output:
            daemon = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                      env=environment)
        try:
            daemon.wait(timeout=10)
        finally:
            stop(daemon)
        log = log_path.read_text()
        check(daemon.returncode == 1, log)
        check("[worker-fixture] queue=0 registration failed" in log, log)
        check("worker monitoring failed; stopping workers" in log, log)
        check("available=1024 capacity=1024 in-use=0" in log, log)
        check(not (directory / "ctl.sock").exists(), "failed worker left control socket")
        check((directory / "storage/state.bin").is_file(), "startup snapshot missing")
        print(f"PASS worker registration failure queues={queues}: monitoring exit=1, "
              "workers joined, mbufs returned, socket removed", flush=True)


def main():
    """基础场景可用普通用户运行，TAP 检查需要 root 并且只创建临时虚拟接口"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--tap", action="store_true", help="also verify temporary TAP down/up as root")
    args = parser.parse_args()
    if args.tap:
        check(os.geteuid() == 0, "--tap requires root")
    build = args.build_dir.resolve()
    queue_counts = [1, 2] if len(os.sched_getaffinity(0)) >= 3 else [1]
    for queues in queue_counts:
        run_case(build, queues)
        run_worker_failure(build, queues)
    if args.tap:
        run_case(build, 1, tap=True)


if __name__ == "__main__":
    main()
