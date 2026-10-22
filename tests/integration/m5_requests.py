#!/usr/bin/env python3
"""正式 daemon 持久请求验收，独立使用原 CLI，不依赖 JSON 或 HTTP 网关"""

import argparse
from concurrent.futures import ThreadPoolExecutor
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
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from verify_m2 import source_digest


def wait(check, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(0.02)
    raise RuntimeError("持久请求验收等待超时")


def parsed(text):
    match = re.search(r"request client=(\d+) sequence=(\d+) phase=(pending|done) status=(-?\d+) "
                      r"applied=(yes|no) durable=(yes|no) revision=(\d+)", text)
    if match is None:
        raise AssertionError("缺少结构完整的请求回执: " + text)
    client, sequence, phase, status, applied, durable, revision = match.groups()
    return {"client": int(client), "sequence": int(sequence), "phase": phase, "status": int(status),
            "applied": applied == "yes", "durable": durable == "yes", "revision": int(revision),
            "rules": [(int(ident), int(generation)) for ident, generation in re.findall(r"^id=(\d+) generation=(\d+)$",
                                                                                    text, re.MULTILINE)]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--report-dir", type=Path, required=True)
    parser.add_argument("--tap", action="store_true")
    args = parser.parse_args()
    build, output = args.build_dir.resolve(), args.report_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binaries = [build / path for path in ("dppd", "dppctl", "dppd-flow-state", "tests/integration/test_m5_daemon_fault")]
    report = {"success": False, "scope": "M5_durable_request_formal_daemon", "source_sha256": source_digest(ROOT),
              "binary_sha256": {str(path.relative_to(build)): hashlib.sha256(path.read_bytes()).hexdigest() for path in binaries},
              "steps": [], "tap": args.tap, "packets": {"received": 0, "forwarded": 0, "dropped": 0},
              "hardware_acceptance": "not_run", "started_utc": datetime.now(timezone.utc).isoformat()}
    children, logs = [], []
    command_lock = threading.Lock()
    try:
        if args.tap:
            if os.geteuid() != 0 or os.readlink("/proc/self/ns/net") == os.readlink("/proc/1/ns/net"):
                raise RuntimeError("TAP 验收须在 root 独立网络命名空间运行")
            for group in ("default", "all"):
                Path(f"/proc/sys/net/ipv6/conf/{group}/disable_ipv6").write_text("1\n")
        cpus = sorted(os.sched_getaffinity(0))
        if len(cpus) < 2:
            raise RuntimeError("至少需要两个可用 CPU")
        with tempfile.TemporaryDirectory(prefix="dppd-requests-") as temporary:
            directory = Path(temporary)
            state, control, marker = directory / "state", directory / "control", directory / "fail-sync"
            environment = dict(os.environ, DPPD_TEST_SYNC_FAILURE=str(marker))
            subprocess.run([str(binaries[2]), "init-software", str(state), "0", "16", "2", "16"],
                           check=True, capture_output=True, timeout=20)
            sequence = 0

            def start(fault=False):
                nonlocal sequence
                sequence += 1
                vdevs = ["--vdev=net_tap0,iface=dppdreqa", "--vdev=net_tap1,iface=dppdreqb"] if args.tap else [
                    "--vdev=net_ring0", "--vdev=net_ring1"]
                command = [str(binaries[3] if fault else binaries[0]), f"--lcores=0@{cpus[0]},1@{cpus[1]}", "--main-lcore=0",
                           "--no-huge", "--no-pci", "--no-shconf", "--no-telemetry", "-m", "128",
                           f"--file-prefix=m5-request-{os.getpid()}-{sequence}", *vdevs, "--", "--ports", "0,1",
                           "--queues", "1", "--mbufs", "2048", "--cache", "0", "--control-socket", str(control),
                           "--flow-state", str(state)]
                log = (output / f"daemon-{sequence}.log").open("w")
                logs.append(log)
                child = subprocess.Popen(command, cwd=ROOT, env=environment, stdout=log, stderr=subprocess.STDOUT)
                children.append(child)
                return child

            def cli(*arguments, ok=True, target=None):
                result = subprocess.run([str(binaries[1]), "--socket", str(target or control), *map(str, arguments)],
                                        env=environment, capture_output=True, text=True, timeout=15)
                with command_lock, (output / "commands.log").open("a") as log:
                    log.write(" ".join(map(str, arguments)) + "\n" + result.stdout + result.stderr)
                if ok:
                    assert result.returncode == 0, result.stdout + result.stderr
                return result

            def ready():
                if process.poll() is not None:
                    raise RuntimeError("daemon 提前退出")
                return control.exists() and cli("ready", ok=False).returncode == 0

            def complete(client, number, expected=0):
                result = cli("request-get", client, number, ok=False)
                receipt = parsed(result.stdout)
                if receipt["phase"] != "done":
                    return None
                assert receipt["durable"] and receipt["status"] == expected, result.stdout + result.stderr
                assert result.returncode == (0 if expected == 0 else 1), result.stdout + result.stderr
                return receipt

            def submit(client, number, *arguments, expected=0):
                cli("request-submit", client, number, *arguments, ok=False)
                return wait(lambda: complete(client, number, expected))

            def stop(kill=False):
                process.send_signal(signal.SIGKILL if kill else signal.SIGTERM)
                assert process.wait(timeout=20) == (-signal.SIGKILL if kill else 0)
                if kill:
                    control.unlink()

            def packets(dropped):
                if not args.tap:
                    return
                with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)) as sender, \
                        socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)) as receiver:
                    sender.bind(("dppdreqa", 0))
                    receiver.bind(("dppdreqb", 0))
                    receiver.settimeout(0.2)
                    before = cli("stats").stdout
                    rx = int(re.search(r"rx_packets=(\d+)", before).group(1))
                    rule_drop = int(re.search(r"rule_drops=(\d+)", before).group(1))
                    tx = int(re.search(r"tx_packets=(\d+)", before).group(1))
                    frame = bytearray(64)
                    frame[:14] = bytes.fromhex("0200000000020200000000010800")
                    frame[14:34] = bytes.fromhex("450000320000000040110000c6120001c6130001")
                    frame[34:42] = struct.pack("!HHHH", 1234, 9000, 30, 0)
                    frame[42:49] = b"M5-reqs"
                    frame = bytes(frame)
                    for index in range(8):
                        sender.send(frame)
                        if dropped:
                            wait(lambda: int(re.search(r"rx_packets=(\d+)", cli("stats").stdout).group(1)) == rx + index + 1)
                        else:
                            assert receiver.recv(2048) == frame
                    if dropped:
                        try:
                            receiver.recv(2048)
                            raise AssertionError("DROP 策略意外转发")
                        except socket.timeout:
                            pass
                    after = cli("stats").stdout
                    assert int(re.search(r"rx_packets=(\d+)", after).group(1)) == rx + 8
                    assert int(re.search(r"rule_drops=(\d+)", after).group(1)) == rule_drop + (8 if dropped else 0)
                    assert int(re.search(r"tx_packets=(\d+)", after).group(1)) == tx + (0 if dropped else 8)
                    report["packets"]["received"] += 8
                    report["packets"]["dropped" if dropped else "forwarded"] += 8

            process = start()
            wait(ready)
            original = ("flow-create", 1, 0, "apply-drop", 11, 0, 0)
            with ThreadPoolExecutor(max_workers=8) as pool:
                submitted = list(pool.map(lambda _: cli("request-submit", 77, 1, *original), range(8)))
            first = wait(lambda: complete(77, 1))
            assert all(parsed(result.stdout)["client"] == 77 for result in submitted)
            assert first["applied"] and first["rules"][0][0] == 11
            assert submit(77, 1, *original) == first
            conflict = cli("request-submit", 77, 1, "flow-create", 1, 0, "apply-drop", 12, 0, 0, ok=False)
            assert conflict.returncode != 0 and "(-17)" in conflict.stderr
            listing = cli("list").stdout
            assert "total=1" in listing and f"generation={first['rules'][0][1]}" in listing
            packets(True)
            report["steps"].append("concurrent_identical_keys_one_rule_conflict_and_exact_revision")

            # 丢弃正式协议回复后断开客户端，后续只靠原客户端键查询，不使用进程内任务凭据
            proxy_path, proxy_errors = directory / "proxy", []
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as listener:
                listener.bind(str(proxy_path))
                listener.listen(1)
                listener.settimeout(10)

                def proxy():
                    try:
                        frontend, _ = listener.accept()
                        with frontend, socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as backend:
                            frontend.settimeout(10)
                            backend.settimeout(10)
                            payload = frontend.recv(131072)
                            backend.connect(str(control))
                            backend.sendall(payload)
                            response = backend.recv(131072)
                            assert response
                    except Exception as error:
                        proxy_errors.append(error)

                thread = threading.Thread(target=proxy)
                thread.start()
                lost = cli("request-submit", 77, 2, "flow-remove", 0, 11, first["rules"][0][1], target=proxy_path, ok=False)
                thread.join(timeout=15)
                assert not thread.is_alive() and not proxy_errors, proxy_errors
                assert lost.returncode != 0
            removed = wait(lambda: complete(77, 2))
            assert removed["applied"] and "total=0" in cli("list").stdout
            packets(False)
            report["steps"].append("lost_response_disconnected_client_query_without_resubmit")

            revision = int(re.search(r"revision=(\d+)", cli("flow-status").stdout).group(1))
            changes = directory / "changes"
            changes.write_text("count-add 800 0 ingress\ntemplate-add 2 8 0 apply-drop 1 0 0 shared:800:0\n")
            configured = submit(88, 1, "flow-config-apply", revision, changes)
            count_generation = configured["rules"][0][1]
            created = submit(77, 3, "flow-create", 2, 0, "apply-drop", 22, 0, 0, f"shared:800:{count_generation}")
            packets(True)
            before = cli("shared-count-query", 800, count_generation).stdout
            assert f"hits={8 if args.tap else 0}" in before
            stop(kill=True)
            process = start()
            wait(ready)
            assert complete(88, 1) == configured
            assert complete(77, 3) == created
            assert submit(88, 1, "flow-config-apply", revision, changes) == configured
            assert "hits=0 bytes=0" in cli("shared-count-query", 800, count_generation).stdout
            packets(True)
            submit(77, 4, "flow-remove", 0, 22, created["rules"][0][1])
            packets(False)
            report["steps"].append("sigkill_policy_receipt_shared_identity_replay_without_reexecuting_configuration")

            # 失败请求同样占据终态回执，推进到超过固定窗口后仍不能复用旧键
            for number in range(10, 141):
                submit(77, number, "flow-remove", 0, 999999, 1, expected=-116)
            old = cli("request-get", 77, 1, ok=False)
            assert old.returncode != 0 and "(-116)" in old.stderr
            old = cli("request-submit", 77, 1, *original, ok=False)
            assert old.returncode != 0 and "(-116)" in old.stderr
            stop()
            process = start(fault=True)
            wait(ready)
            assert "(-116)" in cli("request-get", 77, 1, ok=False).stderr
            report["steps"].append("receipt_rotation_retains_client_high_water_across_restart")

            marker.touch()
            failed = cli("request-submit", 77, 200, "flow-create", 1, 0, "apply-drop", 33, 0, 0, ok=False)
            failed_receipt = parsed(failed.stdout)
            assert failed.returncode != 0 and not failed_receipt["applied"] and not failed_receipt["durable"]
            wait(lambda: "workers=0/" in cli("health").stdout)
            assert cli("ready", ok=False).returncode != 0
            assert "dirty=yes" in cli("persistence-status").stdout
            marker.unlink()
            cli("persistence-flush")
            wait(ready)
            saved_failure = wait(lambda: complete(77, 200, expected=failed_receipt["status"]))
            assert not saved_failure["applied"]
            assert cli("get", 33, ok=False).returncode != 0
            created = submit(77, 201, "flow-create", 1, 0, "apply-drop", 33, 0, 0)
            packets(True)
            stop()
            process = start()
            wait(ready)
            assert complete(77, 200, expected=failed_receipt["status"]) == saved_failure
            assert complete(77, 201) == created
            submit(77, 202, "flow-remove", 0, 33, created["rules"][0][1])
            packets(False)
            stop()
            report["steps"].append("fsync_acceptance_failure_stops_workers_flush_persists_failure_and_new_key_recovers")
            assert source_digest(ROOT) == report["source_sha256"], "验收期间源码改变"
            report["success"] = True
    except (OSError, ValueError, KeyError, AssertionError, RuntimeError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
        print(str(error), file=sys.stderr)
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.kill()
                child.wait(timeout=10)
        for log in logs:
            log.close()
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        path = output / "report.json"
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(path)
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
