#!/usr/bin/env python3
"""正式 daemon 与 CLI 的配置恢复、持久回执、隔离和异常启动验收"""

import argparse
from datetime import datetime, timezone
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
from verify_m2 import source_digest


def wait_for(check, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(0.02)
    raise RuntimeError("等待 daemon 状态超时")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--report-dir", type=Path, required=True)
    parser.add_argument("--tap", action="store_true", help="在独立网络命名空间内验证真实 TAP 报文")
    args = parser.parse_args()
    root, build, output = Path(__file__).resolve().parents[2], args.build_dir.resolve(), args.report_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binaries = [build / "dppd", build / "dppctl", build / "dppd-flow-state",
                build / "tests/integration/test_m5_daemon_fault"]
    hashes = {str(item.relative_to(build)): hashlib.sha256(item.read_bytes()).hexdigest() for item in binaries}
    report = {"success": False, "scope": "M5_formal_daemon_local_protocol", "source_sha256": source_digest(root),
              "binary_sha256": hashes, "hardware_acceptance": "not_run", "steps": [], "tap": args.tap,
              "started_utc": datetime.now(timezone.utc).isoformat()}
    process = None
    handles = []
    try:
        if args.tap:
            if os.geteuid() != 0 or os.readlink("/proc/self/ns/net") == os.readlink("/proc/1/ns/net"):
                raise RuntimeError("TAP 验收必须由 root 在独立网络命名空间运行")
            for group in ("default", "all"):
                Path(f"/proc/sys/net/ipv6/conf/{group}/disable_ipv6").write_text("1\n")
        cpus = sorted(os.sched_getaffinity(0))
        if len(cpus) < 2:
            raise RuntimeError("需要至少两个可用 CPU")
        with tempfile.TemporaryDirectory(prefix="dppd-m5-daemon-") as temporary:
            directory = Path(temporary)
            state, control, marker = directory / "state", directory / "control", directory / "fail-sync"
            result = subprocess.run([str(binaries[2]), "init-software", str(state), "0", "16", "2", "16"],
                                    capture_output=True, text=True, timeout=15)
            assert result.returncode == 0, result.stderr
            environment = dict(os.environ, DPPD_TEST_SYNC_FAILURE=str(marker))
            sequence = 0

            def start(fault=False, extra=(), path=None, socket_path=None):
                nonlocal sequence
                sequence += 1
                prefix = "m5-daemon-" + str(os.getpid()) + "-" + str(sequence)
                vdevs = ["--vdev=net_tap0,iface=dppdm5a", "--vdev=net_tap1,iface=dppdm5b"] if args.tap else [
                    "--vdev=net_ring0", "--vdev=net_ring1"]
                command = [str(binaries[3] if fault else binaries[0]),
                           f"--lcores=0@{cpus[0]},1@{cpus[1]}", "--main-lcore=0", "--no-huge", "--no-pci",
                           "--no-shconf", "--no-telemetry", "-m", "128", "--file-prefix=" + prefix, *vdevs, "--",
                           "--ports", "0,1", "--queues", "1", "--mbufs", "2048", "--cache", "0",
                           "--control-socket", str(socket_path or control), "--flow-state", str(path or state), *extra]
                log = (output / f"daemon-{sequence}.log").open("w")
                handles.append(log)
                child = subprocess.Popen(command, cwd=root, env=environment, stdout=log, stderr=subprocess.STDOUT)
                return child

            def cli(*arguments, ok=True):
                result = subprocess.run([str(binaries[1]), "--socket", str(control), *map(str, arguments)],
                                        capture_output=True, text=True, timeout=10, env=environment)
                with (output / "commands.log").open("a") as log:
                    log.write(" ".join(map(str, arguments)) + "\n" + result.stdout + result.stderr)
                if ok and result.returncode != 0:
                    raise RuntimeError("CLI 失败 " + " ".join(map(str, arguments)) + ": " + result.stderr)
                return result

            def ready():
                if process.poll() is not None:
                    raise RuntimeError("daemon 提前退出，请检查日志")
                return control.exists() and cli("ready", ok=False).returncode == 0

            def accepted(*arguments):
                result = cli(*arguments)
                match = re.search(r"accepted session=(\d+) owner=(\d+) serial=(\d+) slot=(\d+) durable=no", result.stdout)
                assert match, result.stdout
                return match.groups()

            def collect(token):
                result = cli("flow-collect", *token, ok=False)
                if "(-11)" in result.stderr:
                    return None
                assert result.returncode == 0 and "durable=yes" in result.stdout, result.stdout + result.stderr
                return result.stdout

            def stop(kill=False, failed=False):
                process.send_signal(signal.SIGKILL if kill else signal.SIGTERM)
                code = process.wait(timeout=20)
                assert code == (-signal.SIGKILL if kill else 1 if failed else 0), code
                if kill:
                    # 异常退出留下的 socket 由本测试确认旧 PID 结束后清理，不让 daemon 偷删其他实例入口
                    control.unlink()

            def packets(dropped):
                if not args.tap:
                    return
                receiver = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
                sender = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
                try:
                    receiver.bind(("dppdm5b", 0))
                    sender.bind(("dppdm5a", 0))
                    receiver.settimeout(0.2)
                    before = cli("stats").stdout
                    match = re.search(r"rx_packets=(\d+)", before)
                    assert match, before
                    rx = int(match[1])
                    frame = bytearray(64)
                    frame[:14] = bytes.fromhex("0200000000020200000000010800")
                    frame[14:34] = bytes.fromhex("450000320000000040110000c6120001c6130001")
                    frame[34:42] = struct.pack("!HHHH", 1234, 9000, 30, 0)
                    frame[42:51] = b"M5-daemon"
                    frame = bytes(frame)
                    for index in range(8):
                        sender.send(frame)
                        if not dropped:
                            assert receiver.recv(2048) == frame
                        else:
                            # 每包确认已进入 worker，避免把 TAP 内核发送队列丢包误算成策略丢弃
                            wait_for(lambda: int(re.search(r"rx_packets=(\d+)", cli("stats").stdout)[1]) >= rx + index + 1)
                    wait_for(lambda: int(re.search(r"rx_packets=(\d+)", cli("stats").stdout)[1]) >= rx + 8)
                    if dropped:
                        try:
                            receiver.recv(2048)
                            raise AssertionError("DROP 规则仍然转发")
                        except socket.timeout:
                            pass
                    report["packets"] = report.get("packets", 0) + 8
                    field = "dropped" if dropped else "forwarded"
                    report[field] = report.get(field, 0) + 8
                finally:
                    receiver.close()
                    sender.close()

            process = start()
            wait_for(ready)
            packets(False)
            token = accepted("flow-create", "1", "0", "apply-drop", "10", "0", "0")
            completed = wait_for(lambda: collect(token))
            generation = re.search(r"generation=(\d+)", completed)[1]
            assert "dirty=no" in cli("flow-status").stdout
            assert "id=10" in cli("list").stdout
            assert cli("apply-drop", "20", "0", "0", ok=False).returncode != 0
            packets(True)
            snapshot = hashlib.sha256(state.read_bytes()).hexdigest()
            contender = start(socket_path=directory / "other-control")
            assert contender.wait(timeout=15) != 0 and not (directory / "other-control").exists()
            report["steps"].append("durable_create_and_exclusive_writer")
            # 固定长度边界在真实 SEQPACKET 入口验证，残缺消息不能改变策略
            for data in (b"x", bytes(65536)):
                client = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                client.settimeout(5)
                client.connect(str(control))
                client.send(data)
                response = client.recv(65536)
                client.close()
                assert len(response) >= 24 and struct.unpack_from("=i", response, 16)[0] == -90
            stop(kill=True)
            process = start()
            wait_for(ready)
            assert f"generation={generation}" in cli("get", "10").stdout
            assert hashlib.sha256(state.read_bytes()).hexdigest() == snapshot
            assert "(-116)" in cli("flow-collect", *token, ok=False).stderr
            packets(True)
            token = accepted("flow-update", "1", "1", "apply-drop", "10", "0", generation)
            completed = wait_for(lambda: collect(token))
            generation = re.search(r"generation=(\d+)", completed)[1]
            token = accepted("flow-remove", "0", "10", generation)
            wait_for(lambda: collect(token))
            packets(False)
            stop()
            report["steps"].append("sigkill_replay_exact_generation_update_delete_and_stale_session")
            process = start(fault=True)
            wait_for(ready)
            marker.touch()
            token = accepted("flow-create", "1", "0", "apply-drop", "30", "0", "0")
            wait_for(lambda: "dirty=yes" in cli("flow-status").stdout)
            wait_for(lambda: "workers=0/1" in cli("health").stdout)
            assert "(-117)" in cli("flow-collect", *token, ok=False).stderr
            assert cli("ready", ok=False).returncode != 0
            marker.unlink()
            cli("persistence-flush")
            wait_for(lambda: collect(token))
            wait_for(ready)
            packets(True)
            stop()
            process = start(fault=True)
            wait_for(ready)
            checkpoint = hashlib.sha256(state.read_bytes()).hexdigest()
            marker.touch()
            accepted("flow-create", "1", "0", "apply-drop", "40", "0", "0")
            wait_for(lambda: "dirty=yes" in cli("flow-status").stdout)
            stop(failed=True)
            marker.unlink()
            assert hashlib.sha256(state.read_bytes()).hexdigest() == checkpoint
            process = start()
            wait_for(ready)
            assert cli("get", "40", ok=False).returncode != 0
            packets(True)
            # 放弃领取的受理任务仍由 daemon 在退出时取消或领取，不能阻止正常关闭
            accepted("flow-remove", "0", "30", re.search(r"generation=(\d+)", cli("get", "30").stdout)[1])
            stop()
            report["steps"].append("write_failure_stops_workers_flush_recovers_durable_ack")
            # 已存在文件不能被隐式创建或覆盖，损坏启动不得开放管理入口
            broken = directory / "broken"
            broken.write_bytes(b"broken")
            process = start(path=broken)
            assert process.wait(timeout=15) != 0 and not control.exists()
            assert broken.read_bytes() == b"broken"
            process = start(extra=("--state-path", str(directory / "legacy")))
            assert process.wait(timeout=15) != 0 and not control.exists()
            process = start(socket_path=Path(str(state) + ".lock"))
            assert process.wait(timeout=15) != 0
            assert hashlib.sha256(state.read_bytes()).hexdigest() != ""
            report["steps"].append("corruption_legacy_mode_and_path_alias_rejected")
        assert source_digest(root) == report["source_sha256"]
        assert hashes == {str(item.relative_to(build)): hashlib.sha256(item.read_bytes()).hexdigest() for item in binaries}
        report["success"] = True
    except (AssertionError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        report["error"] = repr(error)
        print(repr(error), file=sys.stderr)
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=10)
        for handle in handles:
            handle.close()
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
