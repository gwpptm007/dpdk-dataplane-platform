#!/usr/bin/env python3
"""通过正式管理协议和三 TAP 验收双栈连接跟踪、NAT 策略事务及恢复"""

import argparse
from dataclasses import replace
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import time
import traceback

from conntrack_nat import Flow
from ipv6_traffic import Flow6, IPv6Fixture
from managed_routing import ROOT, Scenario, source_digest, wait


class Traffic(IPv6Fixture):
    """复用独立计算完整报文的测试模型，管理策略仍全部经过正式网关"""

    def __init__(self, scenario):
        self.scenario, self.sequence = scenario, 0
        self.nat, self.auto_snat6 = False, False
        self.links, self.macs = [], []
        for name in scenario.names:
            link = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
            link.setsockopt(263, 8, 1)
            link.bind((name, 0))
            link.setblocking(False)
            identity = fcntl.ioctl(link.fileno(), 0x8927, struct.pack("256s", name.encode()))
            self.links.append(link)
            self.macs.append(identity[18:24])

    def stats(self):
        return {key: int(value) for key, value in re.findall(r"([a-z][a-z0-9_]*)=(\d+)", self.scenario.cli("stats").stdout)}

    def exchange(self, flow, ingress=0, output=None, **options):
        """丢包必须先证明入口收到报文，成功则逐字节核对 MAC、地址、端口和校验和"""
        before = self.stats()["rx_packets"]
        result = super().exchange(flow, ingress=ingress, output=output, **options)
        wait(lambda: self.stats()["rx_packets"] >= before + 1)
        counts = self.scenario.report["packets"]
        counts["forwarded" if output is not None else "dropped"] += 1
        return result

    def close(self):
        for link in self.links:
            link.close()


class ManagedNat(Scenario):
    def __init__(self, *args):
        super().__init__(*args)
        self.request_sequence = 0
        self.traffic = None

    def start(self, **options):
        return super().start(**dict({"ports": (0, 1, 2)}, **options))

    def view(self, resource="policy"):
        code, value = self.request("GET", "/v1/conntrack/" + resource)
        assert code == 200, (resource, code, value)
        return value["data"]

    def policy_request(self, policy, reset=False, **overrides):
        current = self.view()
        self.request_sequence += 1
        return {"operation": "conntrack_replace", "sequence": str(self.request_sequence),
                "revision": current["revision"], "reset_sessions": reset,
                "expected_session_epoch": current["session_epoch"],
                "expected_session_revision": current["session_revision"], "policy": policy, **overrides}

    def apply(self, policy, reset=False, status=0, **overrides):
        body = self.policy_request(policy, reset, **overrides)
        return body, self.submit(body, status)

    def attach(self):
        self.traffic = Traffic(self)
        return self.traffic

    def detach(self):
        if self.traffic:
            self.traffic.close()
            self.traffic = None

    def cleanup(self):
        self.detach()
        super().cleanup()


def policy(nat=False, capacity=32, timeout=120):
    """保留 UDP 40000 给静态端口转发，动态 SNAT 只能分配剩下两个端口"""
    result = {"capacity": capacity, "timeout_s": timeout if capacity else 0,
              "snat": None, "snat6": None, "dnat": []}
    if not nat:
        return result
    for family, public, private, extra in (("ipv4", "203.0.113.9", "10.10.0.9", "203.0.113.10"),
                                          ("ipv6", "2001:db8:100::9", "2001:db8:1::8", "2001:db8:100::a")):
        key = "snat" if family == "ipv4" else "snat6"
        result[key] = {"inside_port": 0, "outside_port": 1, "address": public, "first_port": 40000, "last_port": 40002}
        for protocol, first, last, backend in (("udp", 40000, 40000, 8000), ("tcp", 8443, 8444, 443), ("udp", 0, 65535, 0)):
            target = ("10.10.0.10" if family == "ipv4" else "2001:db8:1::9") if first == 0 else private
            result["dnat"].append({"family": family, "protocol": protocol, "inside_port": 0, "outside_port": 1,
                                   "public_address": extra if first == 0 else public, "public_first": first,
                                   "public_last": last, "private_address": target, "private_first": backend})
    return result


def routes(sequence, revision):
    result = {"operation": "routing_replace", "sequence": str(sequence), "revision": revision}
    result["routes"] = [{"prefix": prefix, "port": port, "next_hop": gateway}
                         for prefix, port, gateway in (("10.10.0.0/24", 0, "192.0.2.10"),
                                                       ("198.51.100.0/24", 1, "192.0.2.20"))]
    result["neighbors"] = [{"port": port, "address": gateway, "mac": mac}
                            for port, gateway, mac in ((0, "192.0.2.10", "02:00:00:00:10:00"),
                                                       (1, "192.0.2.20", "02:00:00:00:20:01"))]
    result["routes6"] = [{"prefix": prefix, "port": port, "next_hop": gateway}
                          for prefix, port, gateway in (("2001:db8:1::/64", 0, "fe80::10"),
                                                        ("2001:db8:2::/64", 1, "fe80::1"))]
    result["neighbors6"] = [{"port": port, "address": gateway, "mac": mac}
                             for port, gateway, mac in ((0, "fe80::10", "02:00:00:00:10:00"),
                                                        (1, "fe80::1", "02:00:00:00:20:01"))]
    return result


def roundtrip(traffic, original, translated, ingress=0):
    output = 1 - ingress
    traffic.exchange(original, ingress=ingress, output=output, translated=translated)
    traffic.exchange(translated.reverse(), ingress=output, output=ingress, translated=original.reverse())


def tcp_handshake(traffic, original, translated, ingress):
    """先 SYN，再 SYN-ACK，最后 ACK，检查双向转换没有破坏 TCP 状态机"""
    output = 1 - ingress
    traffic.exchange(original, ingress=ingress, output=output, translated=translated)
    response = replace(translated.reverse(), flags=18, sequence=500, acknowledgment=101)
    traffic.exchange(response, ingress=output, output=ingress,
                     translated=replace(original.reverse(), flags=18, sequence=500, acknowledgment=101))
    traffic.exchange(replace(original, flags=16, sequence=101, acknowledgment=501), ingress=ingress, output=output,
                     translated=replace(translated, flags=16, sequence=101, acknowledgment=501))


def verify(s):
    s.initialize()
    s.process = s.start()
    wait(s.ready)
    s.start_gateway()
    s.request_sequence = 1
    s.submit(routes(1, s.routing()["revision"]))
    traffic = s.attach()
    initial = s.view()
    assert initial["policy"] == policy(capacity=0)
    assert not s.view("status")["enabled"]
    original4 = Flow(vlan=True)
    original6 = Flow6(vlans=2, extensions=("hop", "dest"))
    roundtrip(traffic, original4, original4)
    roundtrip(traffic, original6, original6)
    assert int(s.view("status")["active"]) == 0
    body = s.policy_request(policy())
    assert s.request("GET", "/v1/conntrack/policy", auth=False)[0] == 401
    for role in ("reader", "operator"):
        assert s.request("POST", "/v1/requests", body, role=role)[0] == 403
    for key in ("expected_session_epoch", "expected_session_revision", "reset_sessions"):
        incomplete = {name: value for name, value in body.items() if name != key}
        assert s.request("POST", "/v1/requests", incomplete, role="admin")[0] == 400
    enabled = s.submit(body)
    assert enabled["applied"]
    assert s.view()["revision"] == s.view()["durable_revision"]
    roundtrip(traffic, original4, original4)
    roundtrip(traffic, original6, original6)
    assert int(s.view("status")["active"]) == 2
    assert s.submit(body) == enabled
    conflict = dict(body, policy=policy(timeout=121))
    assert s.request("POST", "/v1/requests", conflict, role="admin")[0] == 409
    assert int(s.view("status")["active"]) == 2
    s.report["steps"].append("disabled_bypass_admin_enable_dual_stack_tracking_and_durable_idempotency")

    """失败候选不能清空旧会话，也不能让已经建立的正反向映射失效"""
    before = s.view()
    _, refused = s.apply(policy(nat=True), status=-16)
    assert not refused["applied"] and s.view()["policy"] == before["policy"]
    _, stale = s.apply(policy(nat=True), reset=True, status=-116, revision=initial["revision"])
    assert not stale["applied"]
    epoch = int(s.view()["session_epoch"])
    _, stale_epoch = s.apply(policy(nat=True), reset=True, status=-116, expected_session_epoch=str(epoch ^ 1 or 2))
    assert not stale_epoch["applied"] and int(s.view("status")["active"]) == 2
    _, stale_sessions = s.apply(policy(nat=True), reset=True, status=-116,
                                expected_session_revision=str(int(s.view()["session_revision"]) - 1))
    assert not stale_sessions["applied"]
    unbound = policy(nat=True)
    unbound["snat"]["outside_port"] = 3
    _, bad_endpoint = s.apply(unbound, reset=True, status=-19)
    assert not bad_endpoint["applied"] and int(s.view("status")["active"]) == 2
    roundtrip(traffic, original4, original4)
    roundtrip(traffic, original6, original6)
    _, installed = s.apply(policy(nat=True), reset=True)
    assert installed["applied"] and int(s.view("status")["active"]) == 0
    assert s.view()["policy"] == policy(nat=True)
    s.report["steps"].append("active_sessions_reject_default_update_stale_global_and_epoch_preserve_live_flows_explicit_reset")

    """文本策略可直接导出再提交，CLI 与 HTTP 查询必须看到相同的全局版本和完整双栈规则"""
    policy_file = s.directory / "policy.txt"
    policy_file.write_text(s.cli("conntrack-policy-show").stdout)
    current = s.view()
    s.cli("request-submit", 44, 1, "conntrack-replace", policy_file, current["revision"], "false",
          current["session_epoch"], current["session_revision"])
    completed = wait(lambda: (value if "phase=done" in (value := s.cli("request-get", 44, 1).stdout) else None))
    assert "status=0 applied=yes durable=yes" in completed, completed
    assert s.view()["policy"] == current["policy"]
    assert int(s.view()["revision"]) > int(current["revision"])
    s.report["steps"].append("cli_exported_full_policy_resubmission_and_http_share_global_revision")

    """IPv4 与 IPv6 共享会话容量，但各自的 SNAT 端口池和 DNAT 预留独立生效"""
    wire4 = replace(original4, source="203.0.113.9", source_port=40001)
    wire6 = replace(original6, source="2001:db8:100::9", source_port=40001)
    roundtrip(traffic, original4, wire4)
    roundtrip(traffic, original6, wire6)
    for original, address in ((original4, wire4.source), (original6, wire6.source)):
        second = replace(original, source_port=12346)
        roundtrip(traffic, second, replace(second, source=address, source_port=40002))
        traffic.exchange(replace(original, source_port=12347), reason="nat_pool_drops")
    traffic.exchange(wire4.reverse(), ingress=2, reason="nat_miss_drops")
    traffic.exchange(wire6.reverse(), ingress=2, reason="nat_miss_drops")
    for public, backend in ((Flow(source="198.51.100.7", destination="203.0.113.9", source_port=25000,
                                  destination_port=40000), "10.10.0.9"),
                             (Flow6(source="2001:db8:2::7", destination="2001:db8:100::9", source_port=25000,
                                    destination_port=40000), "2001:db8:1::8")):
        roundtrip(traffic, public, replace(public, destination=backend, destination_port=8000), ingress=1)
        whole = replace(public, destination="203.0.113.10" if isinstance(public, Flow) else "2001:db8:100::a",
                        source_port=25001, destination_port=5353)
        whole_backend = "10.10.0.10" if isinstance(public, Flow) else "2001:db8:1::9"
        roundtrip(traffic, whole, replace(whole, destination=whole_backend), ingress=1)
        syn = replace(public, source_port=25002, destination_port=8444, protocol=6, flags=2,
                      sequence=100, acknowledgment=0, payload=b"")
        tcp_handshake(traffic, syn, replace(syn, destination=backend, destination_port=444), ingress=1)
    assert int(s.view("status")["active"]) == 10
    s.report["steps"].append("snat4_nat66_pool_reservation_exhaustion_wrong_ingress_dnat4_dnat6_port_range_address_and_tcp_handshake")

    """分页游标同时绑定进程代号和会话修订，不能把不同快照拼成一张表"""
    code, page = s.request("GET", "/v1/conntrack/sessions")
    assert code == 200 and int(page["data"]["count"]) == 4 and page["data"]["more"], page
    first_page = page["data"]
    next_path = ("/v1/conntrack/sessions?after=" + str(first_page["next_id"]) + "&epoch=" +
                 str(first_page["epoch"]) + "&revision=" + str(first_page["revision"]))
    code, page = s.request("GET", next_path)
    assert code == 200 and int(page["data"]["count"]) == 4, page
    epoch, revision = s.view("status")["epoch"], s.view("status")["revision"]
    assert s.cli("conntrack-flush", revision, ok=False).returncode != 0
    assert int(s.view("status")["active"]) == 10
    assert s.request("POST", "/v1/conntrack/sessions/flush", {"epoch": epoch, "revision": revision})[0] == 403
    assert s.request("POST", "/v1/conntrack/sessions/flush",
                     {"epoch": epoch, "revision": str(int(revision) - 1)}, role="operator")[0] == 409
    code, flushed = s.request("POST", "/v1/conntrack/sessions/flush", {"epoch": epoch, "revision": revision}, role="operator")
    assert code == 200 and int(s.view("status")["active"]) == 0, flushed
    assert s.request("GET", next_path)[0] == 409
    traffic.exchange(wire4.reverse(), ingress=1, reason="nat_miss_drops")
    traffic.exchange(wire6.reverse(), ingress=1, reason="nat_miss_drops")
    roundtrip(traffic, original4, wire4)
    roundtrip(traffic, original6, wire6)
    s.report["steps"].append("session_snapshot_pagination_operator_guarded_flush_and_old_mapping_rejection")

    before_restart = s.view()
    s.detach()
    s.stop(kill=True)
    assert s.request("GET", "/v1/ready")[0] == 503
    s.process = s.start()
    wait(s.ready)
    traffic = s.attach()
    recovered = s.view()
    for key in ("policy", "generation", "revision", "durable_revision"):
        assert recovered[key] == before_restart[key], (key, recovered, before_restart)
    assert recovered["session_epoch"] != before_restart["session_epoch"]
    assert int(s.view("status")["active"]) == 0
    assert s.request("GET", next_path)[0] == 409
    assert s.request("POST", "/v1/conntrack/sessions/flush",
                     {"epoch": before_restart["session_epoch"], "revision": recovered["session_revision"]},
                     role="operator")[0] == 409
    assert s.submit(body) == enabled
    _, old_epoch = s.apply(policy(nat=True), reset=True, status=-116,
                           expected_session_epoch=before_restart["session_epoch"])
    assert not old_epoch["applied"]
    traffic.exchange(wire4.reverse(), ingress=1, reason="nat_miss_drops")
    traffic.exchange(wire6.reverse(), ingress=1, reason="nat_miss_drops")
    roundtrip(traffic, original4, wire4)
    roundtrip(traffic, original6, wire6)
    s.detach()
    s.stop()
    s.report["steps"].append("sigkill_replays_policy_receipts_sessions_volatile_old_epoch_and_old_nat_reply_rejected")

    """发布后落盘失败必须停 worker，恢复只补持久化，不能再次清空新建立的会话"""
    s.process = s.start(fault=True)
    wait(s.ready)
    traffic = s.attach()
    roundtrip(traffic, original4, wire4)
    changed_policy = policy(nat=True, timeout=121)
    fault_body = s.policy_request(changed_policy, reset=True)
    s.marker.write_text("after=2\n")
    code, _ = s.request("POST", "/v1/requests", fault_body, role="admin")
    assert code in (200, 202, 503)
    wait(lambda: "dirty=yes" in s.cli("flow-status").stdout)
    wait(lambda: "workers=0/1" in s.cli("health").stdout)
    assert s.cli("ready", ok=False).returncode != 0
    assert s.view()["policy"] == changed_policy and int(s.view("status")["active"]) == 0
    assert s.view()["revision"] != s.view()["durable_revision"]
    stopped_rx = traffic.stats()["rx_packets"]
    frame, _ = traffic.frame(original4, 99999, 0)
    traffic.links[0].send(frame)
    time.sleep(0.15)
    assert traffic.stats()["rx_packets"] == stopped_rx
    s.report["packets"]["worker_stopped"] += 1
    s.marker.unlink()
    s.cli("persistence-flush")
    wait(s.ready)
    wait(lambda: traffic.stats()["rx_packets"] >= stopped_rx + 1)
    s.report["packets"]["buffered_then_processed"] += 1
    result = wait(lambda: s.complete(fault_body["sequence"]))
    assert result["applied"]
    wait(lambda: int(s.view("status")["active"]) == 1)
    assert s.submit(fault_body) == result
    assert int(s.view("status")["active"]) == 1
    roundtrip(traffic, original4, wire4)
    roundtrip(traffic, original6, wire6)
    s.report["steps"].append("post_apply_fsync_failure_pauses_workers_flush_recovers_and_retry_does_not_reset_again")

    _, disabled_busy = s.apply(policy(capacity=0), status=-16)
    assert not disabled_busy["applied"]
    _, disabled = s.apply(policy(capacity=0), reset=True)
    assert disabled["applied"] and not s.view("status")["enabled"]
    roundtrip(traffic, original4, original4)
    roundtrip(traffic, original6, original6)
    assert int(s.view("status")["active"]) == 0
    assert s.view()["revision"] == s.view()["durable_revision"]
    s.detach()
    s.stop()
    s.gateway.terminate()
    assert s.gateway.wait(timeout=20) == 0
    for log in s.logs:
        log.flush()
    for path in s.output.glob("*.log"):
        text = path.read_text(errors="replace")
        assert not any(token in text for token in s.tokens.values()), "日志泄露认证令牌"
        assert "ERROR: AddressSanitizer" not in text and "runtime error:" not in text, path.name
    s.report["steps"].append("explicit_disable_with_reset_restores_stateless_dual_stack_routing_and_clean_shutdown")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--report-dir", type=Path, required=True)
    parser.add_argument("--tap", action="store_true")
    args = parser.parse_args()
    build, output = args.build_dir.resolve(), args.report_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binaries = [build / name for name in ("dppd", "dppctl", "dppd-flow-state", "tests/integration/test_m5_daemon_fault")]
    report = {"success": False, "scope": "managed_conntrack_nat_three_tap", "hardware_acceptance": "not_run",
              "source_sha256": source_digest(ROOT), "steps": [],
              "packets": {"forwarded": 0, "dropped": 0, "worker_stopped": 0, "buffered_then_processed": 0},
              "binary_sha256": {str(path.relative_to(build)): hashlib.sha256(path.read_bytes()).hexdigest() for path in binaries},
              "started_utc": datetime.now(timezone.utc).isoformat()}
    try:
        if not args.tap or os.geteuid() != 0 or os.readlink("/proc/self/ns/net") == os.readlink("/proc/1/ns/net"):
            raise RuntimeError("验收必须指定 --tap 并在 root 私有网络命名空间中执行")
        for group in ("default", "all"):
            Path(f"/proc/sys/net/ipv6/conf/{group}/disable_ipv6").write_text("1\n")
        with tempfile.TemporaryDirectory(prefix="dppd-managed-nat-") as temporary:
            scenario = ManagedNat(build, output, Path(temporary), report, True)
            try:
                verify(scenario)
                assert source_digest(ROOT) == report["source_sha256"], "验收期间源码发生变化"
                report["success"] = True
            finally:
                scenario.cleanup()
    except (OSError, ValueError, KeyError, AssertionError, RuntimeError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
        traceback.print_exc()
    finally:
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        path = output / "report.json"
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(path)
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
