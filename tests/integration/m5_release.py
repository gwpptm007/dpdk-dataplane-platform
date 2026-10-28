#!/usr/bin/env python3
"""用正式 daemon 验证发布包、保留策略升级、失败回滚和独立实例滚动升级"""

import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import m5_release as release
from verify_m2 import source_digest


def variant(original, original_hash, output, tag, failure=None):
    """版本标签只用于区分交接两端，失败包仅由测试替换启动器，不修改正式程序"""
    _, manifest, files = release.verify_bundle(original, original_hash)
    files.pop("manifest.json")
    files["release-test-tag.txt"] = (tag.encode(), 0o644)
    if failure:
        files["bin/" + failure] = (b"#!/bin/sh\nexit 23\n", 0o755)
    manifest["files"] = {name: {"sha256": release.digest(data), "size": len(data), "mode": mode}
                         for name, (data, mode) in files.items()}
    identity = release.digest(release.encoded(manifest))
    files["manifest.json"] = (release.encoded(manifest), 0o644)
    data = release.archive(files)
    output.write_bytes(data)
    return identity, release.digest(data)


def wait_for(check, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(0.03)
    raise RuntimeError("等待实际状态超时")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--report-dir", type=Path, required=True)
    parser.add_argument("--tap", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build, output = args.build_dir.resolve(), args.report_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"success": False, "scope": "M5_software_release_lifecycle", "steps": [],
              "source_sha256": source_digest(root), "hardware_acceptance": "not_run", "tap": args.tap,
              "binary_sha256": {name: release.digest((build / name).read_bytes()) for name in release.PROGRAMS},
              "binary_reproducibility": "separate_two_build_check_required", "lossless_claim": False,
              "release_pair": "same_binaries_distinct_manifests", "incompatible_reader": "injected_failure",
              "failed_start": "injected_failure"}
    instances = []
    try:
        if args.tap:
            if os.geteuid() != 0 or os.readlink("/proc/self/ns/net") == os.readlink("/proc/1/ns/net"):
                raise ValueError("TAP 验收必须由 root 在独立网络命名空间运行")
            for group in ("default", "all"):
                Path(f"/proc/sys/net/ipv6/conf/{group}/disable_ipv6").write_text("1\n")
        cpus = sorted(os.sched_getaffinity(0))
        if len(cpus) < 2:
            raise ValueError("需要至少两个可用 CPU")

        def run(command, expected=0, timeout=60):
            result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=timeout)
            with (output / "commands.log").open("a") as log:
                log.write(" ".join(map(str, command)) + "\n" + result.stdout + result.stderr + "\n")
            if result.returncode != expected:
                raise RuntimeError("命令退出码不符合预期：" + result.stdout + result.stderr)
            return result

        def manage(instance, *arguments, expected=0):
            result = run([sys.executable, root / "scripts/m5_deploy.py", "--root", instance, "--timeout", "20", *arguments],
                         expected=expected, timeout=120)
            return json.loads(result.stdout)

        def cli(instance, *arguments, expected=0):
            return run([instance / "current/bin/dppctl", "--socket", instance / "run/control.sock", *arguments], expected)

        def packets(instance, number, dropped):
            if not args.tap:
                return
            interfaces = ("dppm5ra", "dppm5rb") if number == 0 else ("dppm5rc", "dppm5rd")
            sender = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
            receiver = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
            try:
                sender.bind((interfaces[0], 0))
                receiver.bind((interfaces[1], 0))
                receiver.settimeout(0.2)
                before = int(re.search(r"rx_packets=(\d+)", cli(instance, "stats").stdout)[1])
                frame = bytes.fromhex("0200000000020200000000010800" + "450000320000000040110000c6120001c6130001")
                frame += struct.pack("!HHHH", 1234, 9000, 30, 0) + b"M5-release" + bytes(12)
                for index in range(8):
                    sender.send(frame)
                    if not dropped:
                        assert receiver.recv(2048) == frame
                    wait_for(lambda: int(re.search(r"rx_packets=(\d+)", cli(instance, "stats").stdout)[1]) >= before + index + 1)
                if dropped:
                    try:
                        receiver.recv(2048)
                        raise AssertionError("升级或恢复后丢失声明的 DROP 策略")
                    except socket.timeout:
                        pass
                report["received"] = report.get("received", 0) + 8
                key = "dropped" if dropped else "forwarded"
                report[key] = report.get(key, 0) + 8
            finally:
                sender.close()
                receiver.close()

        # 保留独立工作目录作为失败证据，任何异常都先停止已登记进程，不提前删除其策略和身份文件
        with contextlib.nullcontext(tempfile.mkdtemp(prefix="dppd-m5-release-")) as scratch:
            temporary = Path(scratch)
            report["work_directory"] = str(temporary)
            first, duplicate = temporary / "first.tar", temporary / "duplicate.tar"
            package = release.pack(root, build, first)
            repeated = release.pack(root, build, duplicate)
            assert package == repeated and first.read_bytes() == duplicate.read_bytes()
            old, old_hash = package["release_id"], package["package_sha256"]
            second = temporary / "second.tar"
            new, new_hash = variant(first, old_hash, second, "second")
            failed = temporary / "failed.tar"
            bad, bad_hash = variant(first, old_hash, failed, "failed-start", "dppd")
            incompatible = temporary / "incompatible.tar"
            incompatible_id, incompatible_hash = variant(first, old_hash, incompatible, "incompatible-reader", "dppd-flow-state")
            report["package_sha256"] = old_hash
            report["release_source_sha256"] = package["source_sha256"]
            report["steps"].append("two_canonical_packages_identical_and_complete_manifest_verified")
            initial = temporary / "initial.state"
            run([build / "dppd-flow-state", "init-software", initial, "0", "16", "2", "16"])
            for index in range(2):
                instance = temporary / ("instance" + str(index))
                interfaces = ("dppm5ra", "dppm5rb") if index == 0 else ("dppm5rc", "dppm5rd")
                vdevs = [f"--vdev=net_tap0,iface={interfaces[0]}", f"--vdev=net_tap1,iface={interfaces[1]}"] if args.tap else [
                    "--vdev=net_ring0", "--vdev=net_ring1"]
                arguments = [f"--lcores=0@{cpus[0]},1@{cpus[1]}", "--main-lcore=0", "--no-huge", "--no-pci", "--no-shconf",
                             "--no-telemetry", "-m", "128", f"--file-prefix=m5release-{os.getpid()}-{index}", *vdevs,
                             "--", "--ports", "0,1", "--queues", "1", "--mbufs", "2048", "--cache", "0"]
                arguments_path = temporary / ("arguments" + str(index) + ".json")
                arguments_path.write_text(json.dumps(arguments), encoding="utf-8")
                manage(instance, "init", "--state", initial, "--arguments", arguments_path)
                instances.append(instance)
                for path, sha in ((first, old_hash), (second, new_hash), (failed, bad_hash), (incompatible, incompatible_hash)):
                    manage(instance, "install", "--package", path, "--sha256", sha)
                manage(instance, "start", "--release", old)
                if index == 0:
                    packets(instance, index, False)
                accepted = cli(instance, "flow-create", "1", "0", "apply-drop", "10", "0", "0").stdout
                token = re.search(r"accepted session=(\d+) owner=(\d+) serial=(\d+) slot=(\d+)", accepted).groups()

                def collect():
                    result = subprocess.run([str(instance / "current/bin/dppctl"), "--socket", str(instance / "run/control.sock"),
                                             "flow-collect", *token], capture_output=True, text=True, timeout=5)
                    if "(-11)" in result.stderr:
                        return False
                    assert result.returncode == 0 and "durable=yes" in result.stdout, result.stdout + result.stderr
                    return True

                wait_for(collect)
            first_instance = instances[0]
            packets(first_instance, 0, True)
            policy = (first_instance / "var/flow.state").read_bytes()
            rule = cli(first_instance, "get", "10").stdout
            report["steps"].append("two_formal_daemons_and_durable_declared_policy")
            manage(first_instance, "upgrade", "--release", new)
            assert cli(first_instance, "get", "10").stdout == rule
            assert (first_instance / "var/flow.state").read_bytes() == policy
            packets(first_instance, 0, True)
            manage(first_instance, "rollback")
            assert cli(first_instance, "get", "10").stdout == rule
            packets(first_instance, 0, True)
            report["steps"].append("healthy_upgrade_and_explicit_rollback_keep_exact_policy")
            manage(first_instance, "upgrade", "--release", bad, expected=1)
            assert manage(first_instance, "status")["current"]["release"] == old
            assert (first_instance / "var/flow.state").read_bytes() == policy
            packets(first_instance, 0, True)
            report["steps"].append("failed_candidate_automatically_restores_old_binary_with_current_policy")
            before = manage(first_instance, "status")["process"]["identity"]
            manage(first_instance, "upgrade", "--release", incompatible_id, expected=1)
            assert manage(first_instance, "status")["process"]["identity"] == before
            report["steps"].append("incompatible_reader_rejected_before_stopping_current_process")
            # 只终止升级控制进程，受管 daemon 与监督者继续运行，恢复命令完成持久事务
            program = """import os,sys
sys.path.insert(0, sys.argv[1])
import m5_deploy
original=m5_deploy.Deployment.transaction
def interrupted(self, record, phase, **extra):
    original(self, record, phase, **extra)
    if phase == 'candidate_ready': os._exit(77)
m5_deploy.Deployment.transaction=interrupted
sys.argv=['m5_deploy','--root',sys.argv[2],'upgrade','--release',sys.argv[3]]
raise SystemExit(m5_deploy.main())
"""
            run([sys.executable, "-c", program, root / "scripts", first_instance, new], expected=77)
            recovered = manage(first_instance, "recover")
            assert recovered["ready"] and recovered["current"]["release"] == new
            packets(first_instance, 0, True)
            manage(first_instance, "rollback")
            report["steps"].append("controller_interruption_recovers_ready_candidate_without_policy_restore")
            run([sys.executable, root / "scripts/m5_deploy.py", "rolling", "--release", new,
                 "--minimum-ready", "1", "--roots", *instances], timeout=180)
            for index, instance in enumerate(instances):
                status = manage(instance, "status")
                assert status["ready"] and status["current"]["release"] == new
                packets(instance, index, True)
            report["steps"].append("two_independent_instances_roll_with_other_ready_gate")
            current = manage(first_instance, "status")["process"]["identity"]
            descriptor = os.pidfd_open(current["pid"], 0)
            signal.pidfd_send_signal(descriptor, signal.SIGKILL)
            os.close(descriptor)
            wait_for(lambda: not manage(first_instance, "status")["running"])
            assert manage(first_instance, "recover")["ready"]
            assert cli(first_instance, "get", "10").stdout == rule
            assert (first_instance / "var/flow.state").read_bytes() == policy
            packets(first_instance, 0, True)
            report["steps"].append("daemon_crash_replays_current_durable_policy")
            for index, instance in enumerate(instances):
                manage(instance, "stop")
                (output / f"daemon-{index}.log").write_bytes((instance / "run/daemon.log").read_bytes())
            instances.clear()
            assert source_digest(root) == report["source_sha256"]
            report["success"] = True
    except (OSError, ValueError, RuntimeError, AssertionError, KeyError, AttributeError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
        print(str(error), file=sys.stderr)
    finally:
        # 失败时仅停止本轮已登记的实例，不枚举或终止其他业务进程
        for instance in instances:
            if instance.exists():
                try:
                    from m5_deploy import Deployment
                    with Deployment(instance).lock() as managed:
                        managed.stop_process(require_clean=False)
                except (OSError, ValueError, RuntimeError, subprocess.SubprocessError):
                    pass
        (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(str(output / "report.json"))
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
