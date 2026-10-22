#!/usr/bin/env python3
"""通过正式进程核对 JSON 日志、managed telemetry、NUMA 布局及卡死超时退出"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from verify_m2 import source_digest


def wait_for(predicate, seconds=10):
    """只等待可观察的完成条件，超时保留进程日志供定位"""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.02)
    raise RuntimeError("等待生产观测状态超时")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--report-dir", type=Path, required=True)
    args = parser.parse_args()
    root, build, output = Path(__file__).resolve().parents[2], args.build_dir.resolve(), args.report_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binaries = {name: build / name for name in ("dppd", "dppctl", "dppd-flow-state",
                "tests/integration/test_m5_worker_stall")}
    report = {"success": False, "scope": "M5_managed_observability_numa_heartbeat",
              "source_sha256": source_digest(root), "steps": [],
              "hardware_acceptance": "not_run", "numa_performance_acceptance": "not_run",
              "binary_sha256": {name: hashlib.sha256(path.read_bytes()).hexdigest()
                                for name, path in binaries.items()}}
    children, clients = [], []
    try:
        cpus = sorted(os.sched_getaffinity(0))
        assert len(cpus) >= 2
        with tempfile.TemporaryDirectory(prefix="dppd-observe-") as temporary:
            directory = Path(temporary)
            state, control = directory / "state", directory / "control"
            initialized = subprocess.run([str(binaries["dppd-flow-state"]), "init-software", str(state),
                                          "0", "16", "2", "16"], capture_output=True, text=True, timeout=15)
            assert initialized.returncode == 0, initialized.stderr

            def start(stalled=False, timeout=5000):
                """每个实例使用自己的 EAL 前缀，不触碰主机已有服务和真实管理网卡"""
                prefix = f"m5-observe-{os.getpid()}-{len(children)}"
                binary = binaries["tests/integration/test_m5_worker_stall" if stalled else "dppd"]
                command = [str(binary), f"--lcores=0@{cpus[0]},1@{cpus[1]}", "--main-lcore=0",
                           "--no-huge", "--no-pci", "-m", "128", "--file-prefix=" + prefix,
                           "--vdev=net_ring0", "--vdev=net_ring1", "--", "--ports", "0,1", "--queues", "1",
                           "--mbufs", "2048", "--cache", "0", "--stats-ms", "100",
                           "--worker-timeout-ms", str(timeout), "--control-socket", str(control),
                           "--flow-state", str(state)]
                log = output / f"daemon-{len(children)}.log"
                environment = dict(os.environ)
                if stalled:
                    environment["DPPD_TEST_WORKER_STALL"] = "1"
                with log.open("w") as stream:
                    process = subprocess.Popen(command, cwd=root, env=environment,
                                               stdout=stream, stderr=subprocess.STDOUT)
                children.append(process)
                return process, log

            def cli(*arguments, check=True):
                result = subprocess.run([str(binaries["dppctl"]), "--socket", str(control), *map(str, arguments)],
                                        capture_output=True, text=True, timeout=5)
                if check:
                    assert result.returncode == 0, result.stdout + result.stderr
                return result

            daemon, log = start()
            wait_for(lambda: control.exists() and cli("ready", check=False).returncode == 0)
            assert daemon.poll() is None

            def connect():
                """定位当前实例自己的 socket，握手 PID 必须与启动的正式进程相同"""
                match = re.search(r"Multi-process socket (.+)/mp_socket", log.read_text())
                if not match:
                    return None
                path = Path(match[1]) / "dpdk_telemetry.v2"
                if not path.exists():
                    return None
                client = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                client.settimeout(3)
                client.connect(str(path))
                greeting = json.loads(client.recv(65536))
                assert greeting["pid"] == daemon.pid
                clients.append(client)
                return client

            client = wait_for(connect)

            def query(command, *parameters):
                client.sendall(",".join([command, *map(str, parameters)]).encode("ascii"))
                response = json.loads(client.recv(65536))
                assert command in response
                return response[command]

            commands = query("/")
            assert all("/dppd/managed/" + name in commands for name in ("status", "stats", "rule", "worker"))
            status = wait_for(lambda: (value := query("/dppd/managed/status")) and value["ready"] and value)
            assert status["workers_expected"] == status["workers_running"] == 1 and status["ports_started"] == 2
            worker = query("/dppd/managed/worker", 0)
            assert worker["lcore"] == 1 and worker["state"] == 2 and worker["timeout_ms"] == 5000
            assert not worker["stalled"] and worker["heartbeat_age_ms"] < 5000
            first = worker["heartbeat_ms"]
            wait_for(lambda: query("/dppd/managed/worker", 0)["heartbeat_ms"] > first)
            assert query("/dppd/managed/worker", 1) is None
            assert query("/dppd/managed/stats", 0, 0)["rx_packets"] == 0
            assert query("/dppd/managed/stats", 1, 0)["rx_packets"] == 0
            assert query("/dppd/managed/stats", 0, 1) is None
            assert query("/dppd/managed/stats", "0,0garbage") is None
            revision = re.search(r"revision=(\d+)", cli("flow-config-get").stdout)[1]
            configuration = directory / "definitions"
            configuration.write_text(
                "template-add 2 32 0 apply-filter 1 0 0 ipv4 any any any any drop count\n")
            assert "applied=yes durable=yes" in cli("flow-config-apply", revision, configuration).stdout
            accepted = cli("flow-create", "2", "0", "apply-filter", "10", "0", "0", "ipv4",
                           "any", "any", "any", "any", "drop", "count").stdout
            token = re.search(r"accepted session=(\d+) owner=(\d+) serial=(\d+) slot=(\d+)", accepted).groups()

            def collect():
                result = cli("flow-collect", *token, check=False)
                if "(-11)" in result.stderr:
                    return None
                assert result.returncode == 0, result.stdout + result.stderr
                return result.stdout

            completed = wait_for(collect)
            generation = int(re.search(r"generation=(\d+)", completed)[1])
            rule = wait_for(lambda: query("/dppd/managed/rule", 10))
            assert rule["generation"] == generation and rule["port"] == rule["queue"] == 0
            assert rule["count_available"] and not rule["shared"] and rule["hits"] == rule["bytes"] == 0
            report["steps"].append("formal_daemon_health_port_queue_rule_telemetry_and_idle_heartbeat")
            daemon.terminate()
            assert daemon.wait(timeout=10) == 0
            events = [json.loads(line) for line in log.read_text().splitlines() if line.startswith('{"schema":')]
            names = {event["event"] for event in events}
            assert {"start", "ready", "worker_layout", "stats", "stop"} <= names
            assert all(event["schema"] == 1 and event["component"] == "dppd" for event in events)
            report["layout"] = [event for event in events if event["event"] == "worker_layout"]
            report["steps"].append("json_lifecycle_periodic_statistics_and_eal_restricted_numa_layout")
            client.close()
            clients.remove(client)
            started = time.monotonic()
            stalled, stall_log = start(stalled=True, timeout=300)
            assert stalled.wait(timeout=10) == 1
            elapsed = time.monotonic() - started
            assert elapsed < 10
            stall_events = [json.loads(line) for line in stall_log.read_text().splitlines()
                            if line.startswith('{"schema":')]
            assert any(event["event"] == "worker_timeout" and event["error"] == -110 for event in stall_events)
            assert not any(event["event"] == "stop" for event in stall_events)
            # 超时退出没有销毁卡死线程仍借用的对象，确认旧 PID 结束后才由部署侧清理 socket
            assert control.exists()
            control.unlink()
            recovered, _ = start()
            wait_for(lambda: control.exists() and cli("ready", check=False).returncode == 0)
            assert f"generation={generation}" in cli("get", "10").stdout
            recovered.terminate()
            assert recovered.wait(timeout=10) == 0
            report["stall_exit_seconds"] = elapsed
            # 停止请求会关闭日常心跳判断，join 本身仍必须限时，避免 SIGTERM 恰好掩盖卡死
            stopping, stopping_log = start(stalled=True, timeout=1000)
            wait_for(lambda: control.exists() and cli("ready", check=False).returncode == 0)
            stop_started = time.monotonic()
            stopping.terminate()
            assert stopping.wait(timeout=5) == 1
            report["stop_stall_exit_seconds"] = time.monotonic() - stop_started
            stopping_events = [json.loads(line) for line in stopping_log.read_text().splitlines()
                               if line.startswith('{"schema":')]
            assert any(event["event"] == "worker_timeout" and event["error"] == -110
                       for event in stopping_events)
            assert not any(event["event"] == "stop" for event in stopping_events)
            report["steps"].append("hung_worker_bounded_failure_exit_and_exact_checkpoint_restart")
            report["success"] = True
    finally:
        for client in clients:
            client.close()
        for child in children:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=10)
        report["source_after_sha256"] = source_digest(root)
        assert report["source_after_sha256"] == report["source_sha256"]
        (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"success": report["success"], "steps": len(report["steps"])}))


if __name__ == "__main__":
    main()
