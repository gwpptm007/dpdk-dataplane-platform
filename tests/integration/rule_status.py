#!/usr/bin/env python3
"""用正式进程验证规则安装状态、只读查询、整批耗时、保存故障和重启重放"""

import argparse
import errno
import os
from pathlib import Path
import subprocess
import tempfile

from batch_update import check, fields
from health_readiness import clean_environment, daemon_command
from recovery_isolation import stop, wait_for


def installation(status):
    """只提取当前规则的安装字段，整个账本的保存版本会随其他规则修改而变化"""
    keys = ("rule", "generation", "port", "backend", "fallback", "count",
            "install-scope", "commit-rules", "install-ns")
    return {key: status[key] for key in keys}


def run_case(build, count):
    """每个场景独占临时目录和 ring 端口，不修改物理网卡或主机网络配置"""
    with tempfile.TemporaryDirectory(prefix="dppd-rule-status-") as temporary:
        directory = Path(temporary)
        storage = directory / "storage"
        storage.mkdir()
        sock, state = directory / "ctl.sock", storage / "state.bin"
        environment = clean_environment()
        command = daemon_command(build, directory, 1)
        command.extend(["--rule-capacity", str(count + 2)])
        ids = list(range(17000, 17000 + count))
        untouched, added = 18000, 19000

        def ctl(*tokens, error=None):
            """检查真实 CLI 的返回码和业务错误，成功时返回供调用方解析的输出"""
            result = subprocess.run([str(build / "dppctl"), "--socket", str(sock),
                                     *map(str, tokens)], env=environment,
                                    capture_output=True, text=True, timeout=5)
            if error is None:
                check(result.returncode == 0, result.stdout + result.stderr)
            else:
                check(result.returncode == 1 and f"({-error})" in result.stderr,
                      result.stdout + result.stderr)
            return result.stdout

        def status(rule_id, generation="any"):
            """当前环境应能读到单调时钟，但不要求虚拟机的测量结果达到某个性能门槛"""
            value = fields(ctl("rule-status", rule_id, generation))
            check(value["rule"] == str(rule_id) and value["backend"] == "software", str(value))
            check(value["install-ns"].isdigit(), "installation timing unavailable: " + str(value))
            check(value["persistence"] == "enabled", str(value))
            return value

        def start(log_name):
            """每次启动留独立日志，结束时检查缓冲池和 socket 的释放"""
            with (directory / log_name).open("w") as output:
                return subprocess.Popen(command, env=environment, stdout=output,
                                        stderr=subprocess.STDOUT)

        daemon = start("run.log")
        try:
            wait_for(daemon, sock.exists, "management socket")
            wait_for(daemon, lambda: fields(ctl("health"))["ready"] == "yes", "readiness")
            ctl("rule-status", 99999, error=errno.ENOENT)
            for rule_id in [*ids, untouched]:
                ctl("apply-filter", rule_id, 0, 0, "tcp", "any", "any", "any", 10000,
                    "drop", "count", "software")
            original = {rule_id: status(rule_id) for rule_id in [*ids, untouched]}
            saved = state.read_bytes()

            # 相同版本的查询输出应稳定，反复查询也不能重装、保存或修改 COUNT
            for _ in range(5):
                for rule_id in [*ids, untouched]:
                    check(status(rule_id) == original[rule_id], "read changed installation record")
            check(state.read_bytes() == saved, "read-only status changed snapshot")
            for rule_id in [*ids, untouched]:
                check("hits=0 bytes=0" in ctl("count", rule_id, original[rule_id]["generation"]),
                      "status changed COUNT")
            ctl("apply-filter", untouched, 0, original[untouched]["generation"], "tcp",
                "any", "any", "any", 10000, "drop", "count", "software")
            check(status(untouched) == original[untouched], "idempotent apply changed timing")

            # ring 不支持 rte_flow，prefer 必须落到软件，不能根据策略误报成硬件
            pairs = [value for rule_id in ids
                     for value in (rule_id, original[rule_id]["generation"])]
            rows = ctl("update-drop-batch", 1, 20, "prefer", *pairs).splitlines()[1:]
            versions = {int(fields(row)["rule"]): fields(row)["generation"] for row in rows}
            batch = {rule_id: status(rule_id, versions[rule_id]) for rule_id in ids}
            check(len({value["install-ns"] for value in batch.values()}) == 1,
                  "one publication reported different batch durations")
            for rule_id, value in batch.items():
                check(value["port"] == "1" and value["fallback"] == "prefer-hardware" and
                      value["count"] == "no" and value["install-scope"] == "batch" and
                      value["commit-rules"] == str(count), str(value))
                ctl("rule-status", rule_id, original[rule_id]["generation"], error=errno.ESTALE)
                ctl("count", rule_id, versions[rule_id], error=errno.ENODATA)
            check(installation(status(untouched)) == installation(original[untouched]),
                  "unrelated update changed original installation record")

            # 保存目录暂时变成普通文件，让规则已生效但快照保存失败的状态可重复构造
            saved = state.read_bytes()
            backup = directory / "saved-storage"
            storage.rename(backup)
            storage.write_text("storage unavailable\n")
            ctl("apply-drop", added, 0, 0, 25, "software", error=errno.EUCLEAN)
            dirty = status(added)
            check(dirty["dirty"] == "yes" and dirty["last-error"] != "0", str(dirty))
            for _ in range(3):
                check(status(added) == dirty, "status silently repaired persistence")
            check((backup / "state.bin").read_bytes() == saved,
                  "dirty status overwrote last good snapshot")
            check(storage.read_text() == "storage unavailable\n", "query changed storage")
            storage.unlink()
            backup.rename(storage)
            ctl("persistence-flush")
            check(status(added)["dirty"] == "no", "flush did not repair snapshot")

            # 单条更新后重新记录本次提交，未更新的批次成员仍保留原来的整批耗时
            first = ids[0]
            ctl("apply-drop", first, 0, versions[first], 35, "software")
            single = status(first)
            check(single["port"] == "0" and single["install-scope"] == "rule" and
                  single["commit-rules"] == "1", str(single))
            ctl("rule-status", first, versions[first], error=errno.ESTALE)
            for rule_id in ids[1:]:
                check(installation(status(rule_id)) == installation(batch[rule_id]),
                      "single update overwrote another rule's batch timing")
            before_restart = {rule_id: status(rule_id) for rule_id in [*ids, untouched, added]}
            saved = state.read_bytes()
        finally:
            stop(daemon)
        check(daemon.returncode == 0 and not sock.exists(), (directory / "run.log").read_text())
        check("available=1024 capacity=1024 in-use=0" in (directory / "run.log").read_text(),
              "first process leaked mbufs")

        daemon = start("replayed.log")
        try:
            wait_for(daemon, sock.exists, "replayed management socket")
            wait_for(daemon, lambda: fields(ctl("health"))["ready"] == "yes", "replayed readiness")
            check(state.read_bytes() == saved, "replay changed rule snapshot")
            for rule_id, previous in before_restart.items():
                value = status(rule_id, previous["generation"])
                for key in ("rule", "generation", "port", "backend", "fallback", "count"):
                    check(value[key] == previous[key], "replay changed rule identity: " + str(value))
                # 重放逐条提交，整批耗时不持久化，不能把上个进程的旧记录当成本次安装记录
                check(value["install-scope"] == "rule" and value["commit-rules"] == "1",
                      "replay reused historical batch timing")
                check(value["dirty"] == "no" and value["persisted-generation"] ==
                      value["repository-generation"], str(value))
                if value["count"] == "yes":
                    check("hits=0 bytes=0" in ctl("count", rule_id, previous["generation"]),
                          "replayed counter not zero")
            for rule_id, previous in before_restart.items():
                ctl("delete", rule_id, previous["generation"])
                ctl("rule-status", rule_id, error=errno.ENOENT)
        finally:
            stop(daemon)
        check(daemon.returncode == 0 and not sock.exists(), "replayed process cleanup failed")
        check("available=1024 capacity=1024 in-use=0" in
              (directory / "replayed.log").read_text(), "replayed process leaked mbufs")
        print(f"PASS rule status rules={count}: read-only, actual backend, batch timing, "
              "stale version, dirty/flush, replay, deletion, mbufs returned", flush=True)


def run_rte_flow_case(build):
    """用临时 TAP 的真实 rte_flow 创建验证后端记录，不把 TAP 当作物理网卡卸载证据"""
    with tempfile.TemporaryDirectory(prefix="dppd-rule-status-tap-") as temporary:
        directory = Path(temporary)
        (directory / "storage").mkdir()
        interfaces = (f"ds{os.getpid()}a", f"ds{os.getpid()}b")
        command = daemon_command(build, directory, 1, interfaces)
        sock, state = directory / "ctl.sock", directory / "storage/state.bin"
        environment = clean_environment()

        def ctl(*tokens, error=None):
            """每条命令都经过正式 socket，COUNT 缺失与规则状态可查询是两种不同结果"""
            result = subprocess.run([str(build / "dppctl"), "--socket", str(sock),
                                     *map(str, tokens)], env=environment,
                                    capture_output=True, text=True, timeout=5)
            check(result.returncode == (0 if error is None else 1), result.stdout + result.stderr)
            if error is not None:
                check(f"({-error})" in result.stderr, result.stderr)
            return fields(result.stdout)

        def start(log_name):
            """启动和退出均由正式程序管理 TAP，测试不会操作其他接口"""
            with (directory / log_name).open("w") as output:
                return subprocess.Popen(command, env=environment, stdout=output,
                                        stderr=subprocess.STDOUT)

        daemon = start("run.log")
        try:
            wait_for(daemon, sock.exists, "TAP management socket")
            wait_for(daemon, lambda: ctl("health")["ready"] == "yes", "TAP readiness")
            ctl("apply-drop", 20000, 0, 0, 10, "require")
            hardware = ctl("rule-status", 20000, 1)
            check(hardware["backend"] == "rte_flow" and hardware["fallback"] ==
                  "require-hardware" and hardware["count"] == "no", str(hardware))
            check(hardware["install-ns"].isdigit() and hardware["commit-rules"] == "1",
                  str(hardware))
            saved = state.read_bytes()
            for _ in range(5):
                check(ctl("rule-status", 20000) == hardware, "TAP query changed record")
            check(state.read_bytes() == saved, "TAP query changed snapshot")
            ctl("count", 20000, 1, error=errno.ENODATA)

            # 同一实例内同时安装平台软件规则，实际后端应逐条定位，不能按进程统一猜测
            ctl("apply-filter", 20001, 1, 0, "tcp", "any", "any", "any", 10000,
                "drop", "count", "software")
            software = ctl("rule-status", 20001, 2)
            check(software["backend"] == "software" and software["count"] == "yes",
                  str(software))
            check(installation(ctl("rule-status", 20000)) == installation(hardware),
                  "software installation changed rte_flow timing")
            ctl("apply-drop", 20000, 1, 1, 20, "require")
            hardware = ctl("rule-status", 20000, 3)
            check(hardware["backend"] == "rte_flow" and hardware["port"] == "1" and
                  hardware["install-scope"] == "rule", str(hardware))
            ctl("rule-status", 20000, 1, error=errno.ESTALE)
            saved = state.read_bytes()
        finally:
            stop(daemon)
        check(daemon.returncode == 0 and not sock.exists(), (directory / "run.log").read_text())

        daemon = start("replayed.log")
        try:
            wait_for(daemon, sock.exists, "replayed TAP socket")
            wait_for(daemon, lambda: ctl("health")["ready"] == "yes", "replayed TAP readiness")
            for rule_id, previous in ((20000, hardware), (20001, software)):
                current = ctl("rule-status", rule_id, previous["generation"])
                for key in ("rule", "generation", "port", "backend", "fallback", "count"):
                    check(current[key] == previous[key], "TAP replay changed identity: " + str(current))
                check(current["install-ns"].isdigit(), "TAP replay timing unavailable")
            check(state.read_bytes() == saved, "TAP replay changed snapshot")
            ctl("delete", 20000, 3)
            ctl("rule-status", 20000, error=errno.ENOENT)
            ctl("delete", 20001, 2)
        finally:
            stop(daemon)
        check(daemon.returncode == 0 and not sock.exists(), "TAP replay cleanup failed")
        for name in ("run.log", "replayed.log"):
            check("available=8191 capacity=8191 in-use=0" in (directory / name).read_text(),
                  "TAP process leaked mbufs")
        for interface in interfaces:
            check(not Path(f"/sys/class/net/{interface}").exists(), "temporary TAP remains")
        print("PASS TAP rule status: real rte_flow create/update/delete, mixed backends, "
              "read-only, stale version, replay, mbufs returned, interfaces removed", flush=True)


def main():
    """使用当前用户可用的 CPU，普通用户即可运行两个不同批次大小的场景"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--tap", action="store_true",
                        help="verify rte_flow status on temporary TAP ports as root")
    args = parser.parse_args()
    check(len(os.sched_getaffinity(0)) >= 2, "two CPUs required")
    if args.tap:
        check(os.geteuid() == 0, "--tap requires root")
        run_rte_flow_case(args.build_dir.resolve())
    else:
        for count in (2, 4):
            run_case(args.build_dir.resolve(), count)


if __name__ == "__main__":
    main()
