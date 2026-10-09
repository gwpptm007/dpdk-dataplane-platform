#!/usr/bin/env python3
"""用自有持久 TAP 保留崩溃后的真实 TC 规则，核对只读检查、未知归属和环境错配"""

import argparse
import errno
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import tempfile
import zlib

from batch_update import check, fields
from health_readiness import clean_environment, daemon_command
from recovery_isolation import stop, wait_for


def run_case(build):
    """所有修改仅限本测试新建的两个 TAP，发现已有同名接口就立即停止"""
    environment = clean_environment()
    interfaces = (f"di{os.getpid()}a", f"di{os.getpid()}b")
    owned = {}
    daemon = None

    def run(*arguments, error=None):
        """保留内核或 CLI 的错误文本，错误场景必须明确失败"""
        result = subprocess.run(list(map(str, arguments)), env=environment,
                                capture_output=True, text=True, timeout=15)
        check(result.returncode == 0 if error is None else result.returncode != 0,
              result.stdout + result.stderr)
        if error:
            check(f"({-error})" in result.stderr, result.stdout + result.stderr)
        return result.stdout

    def create(name):
        """只有成功创建后才登记清理权限，不接管已有接口"""
        check(not Path(f"/sys/class/net/{name}").exists(), "interface already exists")
        run("ip", "tuntap", "add", "dev", name, "mode", "tap", "multi_queue")
        owned[name] = Path(f"/sys/class/net/{name}/ifindex").read_text().strip()

    def remove(name):
        """只删除本测试成功创建的虚拟接口，不接触管理口和物理数据口"""
        check(name in owned, "interface not owned by this test")
        check(Path(f"/sys/class/net/{name}/ifindex").read_text().strip() == owned[name],
              "test interface was replaced externally")
        run("ip", "link", "delete", "dev", name)
        del owned[name]

    def kernel_filters(name):
        """直接读取 tc JSON，忽略无 handle 的分类器标题，并保留可定位字段作为独立对照"""
        rows = []
        for parent in ("1:", "ffff:"):
            for row in json.loads(run("tc", "-j", "filter", "show", "dev", name, "parent", parent)):
                handle = row.get("options", {}).get("handle")
                if handle is None or int(str(handle), 0) == 0:
                    continue
                rows.append((parent, int(str(handle), 0), row.get("chain", 0), row["pref"], row["kind"]))
        return sorted(rows)

    with tempfile.TemporaryDirectory(prefix="dppd-inspect-") as temporary:
        directory = Path(temporary)
        (directory / "storage").mkdir()
        guard, state, sock = directory / "recovery.bin", directory / "storage/state.bin", directory / "ctl.sock"
        command = daemon_command(build, directory, 1, interfaces) + ["--recovery-path", str(guard)]
        log = directory / "daemon.log"
        try:
            for name in interfaces:
                create(name)
            with log.open("w") as output:
                daemon = subprocess.Popen(command, env=environment, stdout=output, stderr=subprocess.STDOUT)
            try:
                wait_for(daemon, sock.exists, "management socket")
                wait_for(daemon, lambda: fields(run(build / "dppctl", "--socket", sock, "health"))["ready"] == "yes",
                         "worker readiness")
                run(build / "dppctl", "--socket", sock, "apply-drop", 910, 0, 0, 10, "require")
                run(build / "dppctl", "--socket", sock, "apply-drop", 911, 1, 0, 11, "require")
            except Exception as error:
                raise AssertionError(log.read_text()) from error
            run(build / "dppd-recovery", "inspect", guard, 2, error=errno.EBUSY)
            # 额外规则属于测试工具而非 daemon，检查程序必须同样报告归属未知并保持不动
            run("tc", "filter", "add", "dev", interfaces[0], "parent", "1:", "protocol", "ip",
                "pref", "60000", "handle", "0xabc", "flower", "dst_ip", "198.18.0.1", "action", "drop")
            daemon.kill()
            daemon.wait(timeout=10)
            check(daemon.returncode == -signal.SIGKILL, "daemon not killed")
            sock.unlink()
            check(all(Path(f"/sys/class/net/{name}").exists() for name in interfaces), "persistent TAP vanished")
            show = run(build / "dppd-recovery", "show", guard)
            revision = int(fields(show.splitlines()[0])["revision"])
            check("format=2" in show and "ports=2" in show, show)
            original_guard, original_state = guard.read_bytes(), state.read_bytes()
            expected = [kernel_filters(name) for name in interfaces]
            check(len(expected[0]) >= 2 and len(expected[1]) >= 1, str(expected))
            check(any(row[1] == 0xabc for row in expected[0]), "foreign fixture filter missing")
            inspected = run(build / "dppd-recovery", "inspect", guard, revision)
            actual = [[], []]
            for line in inspected.splitlines():
                if not line.startswith("filter "):
                    continue
                row = fields(line)
                check(row["owner"] == "unknown", line)
                parent = {"00010000": "1:", "ffff0000": "ffff:"}[row["parent"]]
                actual[int(row["port"])].append((parent, int(row["handle"], 16), int(row["chain"]),
                                                int(row["priority"]), row["kind"]))
            check([sorted(rows) for rows in actual] == expected, inspected + str(expected))
            check(inspected.count("status=coordinates-match complete=yes") == 2, inspected)
            check("cleanup-confirmed=no" in inspected, inspected)
            check([kernel_filters(name) for name in interfaces] == expected, "inspection changed kernel filters")
            run(build / "dppd-recovery", "inspect", guard, revision - 1, error=errno.ESTALE)
            run(build / "dppd-recovery", "inspect", guard, "-1", error=0)
            mismatch = run("unshare", "--net", build / "dppd-recovery", "inspect", guard, revision, error=errno.EXDEV)
            check(mismatch.count("status=context-mismatch complete=no") == 2 and "filters=" not in mismatch, mismatch)
            check(guard.read_bytes() == original_guard and state.read_bytes() == original_state,
                  "read-only inspection changed persisted state")
            # 使用真实旧磁盘布局验证离线兼容，缺少身份只能返回未知，不能猜接口名
            legacy = bytearray(7712)
            legacy[:4128] = original_guard[:4128]
            struct.pack_into("<II", legacy, 8, 1, len(legacy))
            struct.pack_into("<I", legacy, 28, 0)
            for index in range(16):
                legacy[4128 + index * 224:4344 + index * 224] = original_guard[
                    4128 + index * 320:4344 + index * 320]
            struct.pack_into("<I", legacy, 28, zlib.crc32(legacy))
            legacy_path = directory / "legacy.bin"
            legacy_path.write_bytes(legacy)
            legacy_path.chmod(0o600)
            check("format=1" in run(build / "dppd-recovery", "show", legacy_path), "legacy read failed")
            unknown = run(build / "dppd-recovery", "inspect", legacy_path, revision, error=errno.ENODATA)
            check(unknown.count("status=identity-unavailable complete=no") == 2 and "filters=" not in unknown, unknown)
            check(legacy_path.read_bytes() == legacy, "read-only tool upgraded legacy file")
            # 删除后同名重建会得到不同索引，不能把新接口当作旧接口无残留
            remove(interfaces[0])
            create(interfaces[0])
            mismatch = run(build / "dppd-recovery", "inspect", guard, revision, error=errno.EXDEV)
            check("status=identity-mismatch complete=no" in mismatch, mismatch)
            for name in interfaces:
                remove(name)
            absent = run(build / "dppd-recovery", "inspect", guard, revision)
            check(absent.count("status=interface-absent complete=yes") == 2, absent)
            check(guard.read_bytes() == original_guard and state.read_bytes() == original_state,
                  "interface absence incorrectly cleared recovery guard")
            check("state=external-reconciliation-required" in run(build / "dppd-recovery", "show", guard),
                  "inspection acknowledged cleanup")
            # 仅在本测试两个接口确实删除之后，显式确认测试资源的外部清理
            run(build / "dppd-recovery", "acknowledge-clean", guard, revision, "--external-cleanup-complete")
            check("state=clean" in run(build / "dppd-recovery", "show", guard), "explicit acknowledgement failed")
        finally:
            if daemon is not None:
                stop(daemon)
            for name in tuple(owned):
                remove(name)
    print("PASS TAP recovery inspection: persistent crash residuals, kernel TC inventory equality, "
          "unknown ownership including foreign rule, read-only files/filters, live lock, stale revision, "
          "network namespace mismatch, legacy format, same-name replacement, interface absence, explicit cleanup", flush=True)


def main():
    """需要 root 创建测试私有 TAP，正式 inspection 命令本身不要求创建接口"""
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    arguments = parser.parse_args()
    check(os.geteuid() == 0, "persistent TAP test needs root")
    run_case(arguments.build_dir.resolve())


if __name__ == "__main__":
    main()
