#!/usr/bin/env python3
"""通过正式管理入口验收双栈路由、静态邻居、持久回执和进程恢复"""

import argparse
from datetime import datetime, timezone
import fcntl
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import secrets
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from verify_m2 import source_digest


class UnixConnection(http.client.HTTPConnection):
    """HTTP 仍走正式网关，Unix 监听避免测试依赖某个空闲 TCP 端口"""

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.host)


def wait(check, seconds=20):
    """只等待可以明确观察的状态，超时保留日志而不假定操作已完成"""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(0.03)
    raise RuntimeError("受管理路由验收等待超时")


def checksum(data):
    """独立按网络字节序计算校验和，不调用项目中的报文改写函数"""
    if len(data) & 1:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return (~total) & 65535


class Scenario:
    def __init__(self, build, output, directory, report, tap):
        self.build, self.output, self.directory = build, output, directory
        self.report, self.tap = report, tap
        self.state, self.control = directory / "state", directory / "control"
        self.api_socket, self.marker = directory / "api", directory / "fail-sync"
        self.children, self.logs = [], []
        self.process, self.gateway = None, None
        self.sequence, self.packet_sequence = 0, 0
        self.deferred_target = None
        self.names = [f"dmr{os.getpid() % 100000}{n}" for n in range(3)]
        self.environment = dict(os.environ, DPPD_TEST_SYNC_FAILURE=str(self.marker))
        self.tokens = {role: secrets.token_hex(32) for role in ("reader", "operator", "admin")}
        self.credentials = directory / "tokens.json"
        self.credentials.write_text(json.dumps({"version": 1, "tokens": [
            {"sha256": hashlib.sha256(token.encode()).hexdigest(), "role": role, "client": str(index + 1)}
            for index, (role, token) in enumerate(self.tokens.items())]}), encoding="utf-8")
        self.credentials.chmod(0o600)
        cpus = sorted(os.sched_getaffinity(0))
        if len(cpus) < 2:
            raise RuntimeError("至少需要两个可用 CPU")
        self.cpus = cpus[:2]

    def initialize(self, path=None):
        subprocess.run([str(self.build / "dppd-flow-state"), "init-software", str(path or self.state),
                        "0", "16", "2", "16"], check=True, capture_output=True, timeout=20, env=self.environment)

    def launch(self, command, label):
        log = (self.output / (label + ".log")).open("w")
        self.logs.append(log)
        child = subprocess.Popen(command, cwd=ROOT, env=self.environment, stdout=log, stderr=subprocess.STDOUT)
        self.children.append(child)
        return child

    def start(self, fault=False, routing=True, state=None, control=None, ports=(0, 1), rename=False, extra=()):
        self.sequence += 1
        names = [name + "x" if rename else name for name in self.names]
        vdevs = [f"--vdev=net_tap{port},iface={names[port]}" if self.tap else f"--vdev=net_ring{port}"
                 for port in ports]
        executable = self.build / ("tests/integration/test_m5_daemon_fault" if fault else "dppd")
        command = [str(executable), f"--lcores=0@{self.cpus[0]},1@{self.cpus[1]}", "--main-lcore=0",
                   "--no-huge", "--no-pci", "--no-shconf", "--no-telemetry", "-m", "128",
                   f"--file-prefix=managed-route-{os.getpid()}-{self.sequence}", *vdevs, "--",
                   "--ports", ",".join(map(str, ports)), "--queues", "1", "--mbufs", "4096", "--cache", "0",
                   "--control-socket", str(control or self.control), "--flow-state", str(state or self.state)]
        if routing:
            command.append("--managed-routing")
        return self.launch([*command, *extra], f"daemon-{self.sequence}")

    def cli(self, *arguments, ok=True, control=None):
        result = subprocess.run([str(self.build / "dppctl"), "--socket", str(control or self.control),
                                 *map(str, arguments)], capture_output=True, text=True, timeout=15,
                                env=self.environment)
        with (self.output / "commands.log").open("a") as log:
            log.write(" ".join(map(str, arguments)) + "\n" + result.stdout + result.stderr)
        if ok:
            assert result.returncode == 0, result.stdout + result.stderr
        return result

    def ready(self):
        if self.process.poll() is not None:
            raise RuntimeError("daemon 提前退出，请检查对应启动日志")
        return self.control.exists() and self.cli("ready", ok=False).returncode == 0

    def start_gateway(self):
        self.gateway = self.launch([sys.executable, str(ROOT / "app/dppd-api/main.py"), "--socket",
                                    str(self.control), "--dppctl", str(self.build / "dppctl"), "--tokens",
                                    str(self.credentials), "--unix", str(self.api_socket)], "api")
        wait(lambda: self.api_socket.exists() and self.request("GET", "/v1/ready")[0] == 200)

    def request(self, method, path, body=None, role="reader", auth=True):
        connection = UnixConnection(str(self.api_socket), timeout=15)
        headers = {"Authorization": "Bearer " + self.tokens[role]} if auth else {}
        if body is not None:
            headers["Content-Type"] = "application/json"
        connection.request(method, path, json.dumps(body) if body is not None else None, headers)
        response = connection.getresponse()
        value, code = json.loads(response.read()), response.status
        connection.close()
        assert value["api_version"] == "v1", value
        return code, value

    def routing(self):
        code, value = self.request("GET", "/v1/routing")
        assert code == 200, value
        return value["data"]

    def complete(self, number, status=0):
        code, value = self.request("GET", f"/v1/requests/{number}", role="admin")
        if code == 202:
            return None
        assert code == 200, (code, value)
        result = value["data"]
        if result["state"] != "complete":
            return None
        assert result["durable"] and result["status"] == status and not result["blocked"], result
        return result

    def submit(self, body, status=0):
        code, value = self.request("POST", "/v1/requests", body, role="admin")
        assert code in (200, 202), (code, value)
        return wait(lambda: self.complete(body["sequence"], status))

    def stop(self, kill=False):
        self.process.send_signal(signal.SIGKILL if kill else signal.SIGTERM)
        assert self.process.wait(timeout=25) == (-signal.SIGKILL if kill else 0)
        if self.control.exists():
            assert stat.S_ISSOCK(self.control.lstat().st_mode)
            self.control.unlink()

    def packets(self, neighbors, forward=True, stopped=False):
        """逐包核对完整帧，丢弃还核对入口计数，避免把发包失败算成策略生效"""
        if not self.tap:
            return
        before_stats = self.cli("stats").stdout
        before = int(re.search(r"rx_packets=(\d+)", before_stats)[1])
        before_tx = int(re.search(r"tx_packets=(\d+)", before_stats)[1])
        sender = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
        receiver = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
        try:
            sender.bind((self.names[0], 0))
            receiver.bind((self.names[1], 0))
            """ioctl 查询当前网络空间的接口，避免沿用宿主机挂载的 sysfs 视图"""
            identity = fcntl.ioctl(receiver.fileno(), 0x8927, struct.pack("256s", self.names[1].encode()))
            source_mac = identity[18:24]
            receiver.settimeout(0.3 if not forward else 2)
            for index in range(8):
                self.packet_sequence += 1
                family = 4 if index % 2 == 0 else 6
                payload = b"MANAGED-ROUTE" + struct.pack("!I", self.packet_sequence) + bytes(range(32))
                udp = bytearray(struct.pack("!HHHH", 12000, 24000, len(payload) + 8, 0) + payload)
                if family == 4:
                    ip = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), self.packet_sequence,
                                               0, 64, 17, 0, socket.inet_aton("198.18.0.10"),
                                               socket.inet_aton("198.19.0.5")))
                    struct.pack_into("!H", ip, 10, checksum(ip))
                    ether_type = b"\x08\x00"
                else:
                    source = socket.inet_pton(socket.AF_INET6, "2001:db8:1::10")
                    destination = socket.inet_pton(socket.AF_INET6, "2001:db8:2::5")
                    pseudo = source + destination + struct.pack("!I3xB", len(udp), 17)
                    struct.pack_into("!H", udp, 6, checksum(pseudo + udp) or 65535)
                    ip = bytearray(struct.pack("!IHBB16s16s", 6 << 28, len(udp), 17, 64, source, destination))
                    ether_type = b"\x86\xdd"
                frame = bytes.fromhex("020000001001020000001002") + ether_type + ip + udp
                sender.send(frame)
                if forward:
                    expected_ip = bytearray(ip)
                    if family == 4:
                        expected_ip[8] -= 1
                        struct.pack_into("!H", expected_ip, 10, 0)
                        struct.pack_into("!H", expected_ip, 10, checksum(expected_ip))
                    else:
                        expected_ip[7] -= 1
                    expected = bytes.fromhex(neighbors[family].replace(":", "")) + source_mac + ether_type + expected_ip + udp
                    actual = receiver.recv(65536)
                    assert actual == expected, (family, actual.hex(), expected.hex())
                elif not stopped:
                    wait(lambda: int(re.search(r"rx_packets=(\d+)", self.cli("stats").stdout)[1]) >= before + index + 1)
            if not forward:
                try:
                    unexpected = receiver.recv(65536)
                    raise AssertionError("本应停止或丢弃的报文仍被转发: " + unexpected.hex())
                except socket.timeout:
                    pass
            if stopped:
                assert int(re.search(r"rx_packets=(\d+)", self.cli("stats").stdout)[1]) == before
                self.deferred_target = (before + 8, before_tx + 8)
            else:
                wait(lambda: int(re.search(r"rx_packets=(\d+)", self.cli("stats").stdout)[1]) >= before + 8)
            key = "forwarded" if forward else "worker_stopped" if stopped else "dropped"
            self.report["packets"][key] += 8
        finally:
            sender.close()
            receiver.close()

    def cleanup(self):
        for child in reversed(self.children):
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)
        for log in self.logs:
            log.close()


def configuration(sequence, revision, mac4="02:00:00:00:44:05", mac6="02:00:00:00:66:05"):
    return {"operation": "routing_replace", "sequence": str(sequence), "revision": str(revision),
            "routes": [{"prefix": "198.19.0.0/24", "port": 1, "next_hop": "direct"}],
            "neighbors": [{"address": "198.19.0.5", "port": 1, "mac": mac4}],
            "routes6": [{"prefix": "2001:db8:2::/64", "port": 1, "next_hop": "direct"}],
            "neighbors6": [{"address": "2001:db8:2::5", "port": 1, "mac": mac6}]}


def verify(scenario):
    s = scenario
    s.initialize()
    if not s.tap:
        """net_ring 的全零源 MAC 不能用来证明三层转发成功，明确验收其拒绝路径"""
        s.process = s.start()
        assert s.process.wait(timeout=20) != 0
        assert not s.control.exists()
        s.report["steps"].append("net_ring_invalid_source_mac_rejected_before_ready")
        return
    s.process = s.start()
    wait(s.ready)
    s.start_gateway()
    initial = s.routing()
    assert initial["generation"] == "1" and all(not initial[key] for key in ("routes", "neighbors", "routes6", "neighbors6"))
    assert initial["revision"] == initial["durable_revision"]
    s.packets({}, forward=False)
    body = configuration(1, initial["revision"])
    assert s.request("GET", "/v1/routing", auth=False)[0] == 401
    for role in ("reader", "operator"):
        assert s.request("POST", "/v1/requests", body, role=role)[0] == 403
    missing = dict(body)
    del missing["neighbors6"]
    assert s.request("POST", "/v1/requests", missing, role="admin")[0] == 400
    first = s.submit(body)
    assert first["applied"] and first["routing_generation"] == "2", first
    installed = s.routing()
    assert installed["generation"] == "2" and installed["revision"] == installed["durable_revision"]
    s.packets({4: body["neighbors"][0]["mac"], 6: body["neighbors6"][0]["mac"]})
    assert s.submit(body) == first
    changed = configuration(1, initial["revision"], mac4="02:00:00:00:44:06")
    assert s.request("POST", "/v1/requests", changed, role="admin")[0] == 409
    stale = s.submit(configuration(2, initial["revision"]), status=-116)
    assert not stale["applied"] and s.routing()["generation"] == "2"
    s.report["steps"].append("empty_bootstrap_admin_only_dual_stack_atomic_replace_idempotency_and_stale_revision")

    """CLI 与 HTTP 必须修改同一张路由表，并共享统一配置版本"""
    route_file = s.directory / "routing.txt"
    cli_mac4, cli_mac6 = "02:00:00:00:44:07", "02:00:00:00:66:07"
    route_file.write_text("route 198.19.0.0/24 1 direct\nneighbor 1 198.19.0.5 " + cli_mac4 +
                          "\nroute6 2001:db8:2::/64 1 direct\nneighbor6 1 2001:db8:2::5 " + cli_mac6 + "\n")
    revision = s.routing()["revision"]
    assert s.cli("routing-replace", route_file, 2, ok=False).returncode != 0
    s.cli("request-submit", 44, 1, "routing-replace", route_file, revision)
    completed = wait(lambda: (value if "phase=done" in (value := s.cli("request-get", 44, 1).stdout) else None))
    assert "status=0 applied=yes durable=yes" in completed and "routing-generation=3" in completed, completed
    assert s.routing()["neighbors"][0]["mac"] == cli_mac4
    s.packets({4: cli_mac4, 6: cli_mac6})
    s.report["steps"].append("cli_durable_request_and_http_share_revision_and_live_routing")

    """同一个 managed worker 先执行规则再查路由，规则事务不能暗中推进路由代次"""
    route_revision = s.routing()["revision"]
    s.cli("request-submit", 44, 2, "flow-create", 1, 0, "apply-drop", 700, 0, 0)
    created = wait(lambda: (value if "phase=done" in (value := s.cli("request-get", 44, 2).stdout) else None))
    assert "status=0 applied=yes durable=yes" in created, created
    rule_generation = re.search(r"^id=700 generation=(\d+)$", created, re.MULTILINE)[1]
    blocked_by_rule = s.routing()
    assert blocked_by_rule["generation"] == "3" and int(blocked_by_rule["revision"]) > int(route_revision)
    s.packets({}, forward=False)
    s.cli("request-submit", 44, 3, "flow-remove", 0, 700, rule_generation)
    removed = wait(lambda: (value if "phase=done" in (value := s.cli("request-get", 44, 3).stdout) else None))
    assert "status=0 applied=yes durable=yes" in removed, removed
    unblocked = s.routing()
    assert unblocked["generation"] == "3" and int(unblocked["revision"]) > int(blocked_by_rule["revision"])
    s.packets({4: cli_mac4, 6: cli_mac6})
    s.report["steps"].append("managed_classifier_drop_and_delete_preserve_route_generation_and_share_global_revision")

    before_restart = s.routing()
    snapshot = hashlib.sha256(s.state.read_bytes()).hexdigest()
    s.stop(kill=True)
    assert s.request("GET", "/v1/ready")[0] == 503
    for parameters in ({"routing": False}, {"ports": (0,)}, {"rename": True},
                       {"extra": ("--ip-routing", str(s.directory / "legacy-routing"))}):
        child = s.start(control=s.directory / "rejected-control", **parameters)
        assert child.wait(timeout=20) != 0
        assert not (s.directory / "rejected-control").exists()
        assert hashlib.sha256(s.state.read_bytes()).hexdigest() == snapshot
    s.process = s.start()
    wait(s.ready)
    assert s.routing() == before_restart
    assert s.complete(1) == first
    assert "phase=done" in s.cli("request-get", 44, 1).stdout
    s.packets({4: cli_mac4, 6: cli_mac6})
    s.stop()
    s.report["steps"].append("sigkill_restart_replays_routes_receipts_and_rejects_mode_or_endpoint_changes")

    """先让 pending 文件及目录同步成功，再让路由发布后的同步失败"""
    s.process = s.start(fault=True)
    wait(s.ready)
    revision = s.routing()["revision"]
    fault_body = configuration(3, revision)
    s.marker.write_text("after=2\n")
    code, _ = s.request("POST", "/v1/requests", fault_body, role="admin")
    assert code in (200, 202, 503)
    wait(lambda: "dirty=yes" in s.cli("flow-status").stdout)
    wait(lambda: "workers=0/1" in s.cli("health").stdout)
    assert s.cli("ready", ok=False).returncode != 0
    assert s.routing()["generation"] == "4"
    assert s.routing()["revision"] != s.routing()["durable_revision"]
    pending = s.cli("request-get", 3, 3, ok=False).stdout
    assert "applied=yes" in pending and "durable=no" in pending, pending
    s.packets({}, forward=False, stopped=True)
    s.marker.unlink()
    s.cli("persistence-flush")
    wait(s.ready)
    """停 worker 期间 TAP 可以暂存报文，先确认恢复后处理完这批旧序号再开启下一轮抓包"""
    def drained():
        text = s.cli("stats").stdout
        received = int(re.search(r"rx_packets=(\d+)", text)[1])
        transmitted = int(re.search(r"tx_packets=(\d+)", text)[1])
        return received >= s.deferred_target[0] and transmitted >= s.deferred_target[1]
    wait(drained)
    s.report["packets"]["buffered_then_forwarded"] = 8
    s.deferred_target = None
    fault_result = wait(lambda: s.complete(3))
    assert fault_result["routing_generation"] == "4"
    assert s.submit(fault_body) == fault_result
    s.packets({4: fault_body["neighbors"][0]["mac"], 6: fault_body["neighbors6"][0]["mac"]})
    s.stop()
    s.report["steps"].append("post_publication_fsync_failure_stops_workers_flush_resumes_same_policy_and_receipt")

    """三层模式不依赖端口成对排列，分别检查一个端口和三个端口的启动退出"""
    for count in (1, 3):
        state = s.directory / f"state-{count}"
        control = s.directory / f"control-{count}"
        s.initialize(state)
        child = s.start(state=state, control=control, ports=tuple(range(count)))
        wait(lambda: child.poll() is None and control.exists() and s.cli("ready", control=control, ok=False).returncode == 0)
        child.terminate()
        assert child.wait(timeout=20) == 0
    s.gateway.terminate()
    assert s.gateway.wait(timeout=20) == 0
    for log in s.logs:
        log.flush()
    for log in s.output.glob("*.log"):
        text = log.read_text(errors="replace")
        assert not any(token in text for token in s.tokens.values()), "日志泄露认证令牌"
        assert "ERROR: AddressSanitizer" not in text and "runtime error:" not in text, log.name
    s.report["steps"].append("single_and_three_port_routing_and_orderly_process_cleanup")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--report-dir", type=Path, required=True)
    parser.add_argument("--tap", action="store_true", help="在 root 私有网络命名空间中运行完整双栈正向验收")
    args = parser.parse_args()
    build, output = args.build_dir.resolve(), args.report_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binaries = [build / name for name in ("dppd", "dppctl", "dppd-flow-state", "tests/integration/test_m5_daemon_fault")]
    report = {"success": False, "scope": "managed_routing_tap" if args.tap else "managed_routing_ring_rejection",
              "complete_routing_validation": False, "tap": args.tap, "hardware_acceptance": "not_run",
              "source_sha256": source_digest(ROOT), "steps": [],
              "packets": {"forwarded": 0, "dropped": 0, "worker_stopped": 0},
              "binary_sha256": {str(path.relative_to(build)): hashlib.sha256(path.read_bytes()).hexdigest() for path in binaries},
              "started_utc": datetime.now(timezone.utc).isoformat()}
    scenario = None
    try:
        if args.tap:
            if os.geteuid() != 0 or os.readlink("/proc/self/ns/net") == os.readlink("/proc/1/ns/net"):
                raise RuntimeError("TAP 验收必须在 root 独立网络命名空间中执行")
            for group in ("default", "all"):
                Path(f"/proc/sys/net/ipv6/conf/{group}/disable_ipv6").write_text("1\n")
        with tempfile.TemporaryDirectory(prefix="dppd-managed-routing-") as temporary:
            scenario = Scenario(build, output, Path(temporary), report, args.tap)
            try:
                verify(scenario)
                assert source_digest(ROOT) == report["source_sha256"], "验收期间源码发生变化"
                report["complete_routing_validation"] = args.tap
                report["success"] = True
            finally:
                scenario.cleanup()
    except (OSError, ValueError, KeyError, AssertionError, RuntimeError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
        print(str(error), file=sys.stderr)
    finally:
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        path = output / "report.json"
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(path)
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
