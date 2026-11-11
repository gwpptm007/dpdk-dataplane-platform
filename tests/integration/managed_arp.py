#!/usr/bin/env python3
"""通过正式管理入口和三 TAP 验收动态 ARP 配置、等待报文、NAT 与故障恢复"""

import argparse
from dataclasses import replace
from datetime import datetime, timezone
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

from conntrack_nat import Flow, crc
from ipv6_traffic import Flow6
from managed_nat import ManagedNat, Traffic, policy as nat_policy, routes, roundtrip
from managed_routing import ROOT, source_digest, wait
from nd6_traffic import Neighbor, decoded


class ArpTraffic(Traffic):
    """预期帧仍由独立模型计算，允许测试静态映射覆盖动态 MAC"""

    def __init__(self, scenario):
        super().__init__(scenario)
        self.names = scenario.names
        self.vlans = ()
        self.peer_macs = [bytes.fromhex(value) for value in ("020000001000", "020000002001", "020000003002")]

    def frame(self, flow, tag, ingress, output=None):
        wire, marker = super().frame(flow, tag, ingress, output)
        if output is not None:
            wire = self.peer_macs[output] + wire[6:]
        return wire, marker

    def exchange(self, *args, **kwargs):
        self.scenario.report["packets"]["injected"] += 1
        return super().exchange(*args, **kwargs)


class ArpNeighbor(Neighbor):
    """原始套接字扮演下一跳，只回答明确请求，不替 daemon 做解析"""

    def run(self):
        import select
        try:
            while not self.stopped.is_set():
                ready, _, _ = select.select(self.links, [], [], .05)
                for link in ready:
                    data, ancillary, _, _ = link.recvmsg(65536, 1024)
                    data, l2 = decoded(data, ancillary)
                    port = self.links.index(link)
                    if len(data) < l2 or data[6:12] != self.fixture.macs[port]:
                        continue
                    if data[l2 - 2:l2] == b"\x08\x06" and len(data) >= l2 + 28:
                        self.controls.append((port, data))
                        body = data[l2:l2 + 28]
                        gateway = "192.0.2.10" if port == 0 else "192.0.2.20"
                        if self.auto and body[7] == 1 and body[14:18] != bytes(4) and body[24:28] == socket.inet_aton(gateway):
                            self.send(port, 2, gateway, socket.inet_ntoa(body[14:18]))
                    elif data[l2 - 2:l2] in (b"\x08\x00", b"\x86\xdd"):
                        self.packets.append((port, data))
        except Exception as error:
            self.errors.append(repr(error))

    def send(self, port, operation, source, target, mac=None, corrupt=False, vlans=None):
        source_mac = self.fixture.peer_macs[port] if mac is None else mac
        vlans = self.fixture.vlans if vlans is None else vlans
        destination = self.fixture.macs[port] if operation == 2 else bytes([255]) * 6
        wire = destination + source_mac
        for index, tag in enumerate(vlans):
            wire += struct.pack("!HH", 0x88a8 if len(vlans) == 2 and index == 0 else 0x8100, tag)
        body = struct.pack("!HHBBH", 1, 0x0800, 6, 4, operation)
        body += (bytes([source_mac[0] ^ 2]) + source_mac[1:]) if corrupt else source_mac
        body += socket.inet_aton(source) + (destination if operation == 2 else bytes(6)) + socket.inet_aton(target)
        self.links[port].send((wire + b"\x08\x06" + body).ljust(60, b"\0"))


class ManagedArp(ManagedNat):
    def __init__(self, *args):
        super().__init__(*args)
        self.observer = None

    def arp(self, resource="policy"):
        code, value = self.request("GET", "/v1/arp/" + resource)
        assert code == 200, (resource, code, value)
        return value["data"]

    def arp_request(self, policy, reset=False, **overrides):
        current = self.arp()
        self.request_sequence += 1
        return {"operation": "arp_replace", "sequence": str(self.request_sequence), "revision": current["revision"],
                "reset_neighbors": reset, "expected_neighbor_epoch": current["neighbor_epoch"],
                "expected_neighbor_revision": current["neighbor_revision"], "policy": policy, **overrides}

    def completed(self, sequence):
        code, value = self.request("GET", "/v1/requests/" + str(sequence), role="admin")
        if code == 202:
            return None
        assert code == 200, (code, value)
        result = value["data"]
        if result["state"] != "complete":
            return None
        assert result["durable"] and not result["blocked"], result
        return result

    def apply_arp(self, policy, reset=False, status=0, **overrides):
        """只有明确返回 ESTALE 且没有应用时才换新键重试，不能重做结果不明的清理请求"""
        for _ in range(8):
            body = self.arp_request(policy, reset, **overrides)
            code, reply = self.request("POST", "/v1/requests", body, role="admin")
            assert code in (200, 202), (code, reply)
            result = wait(lambda: self.completed(body["sequence"]))
            if result["status"] == -116 and status != -116 and not overrides:
                assert not result["applied"]
                continue
            assert result["status"] == status and result["applied"] == (status == 0), result
            return body, result
        raise AssertionError("ARP 状态持续变化，无法取得可提交的精确版本")

    def flush_arp(self):
        for _ in range(8):
            current = self.arp("status")
            code, value = self.request("POST", "/v1/arp/neighbors/flush",
                                       {"epoch": current["epoch"], "revision": current["revision"]}, role="operator")
            if code == 409:
                continue
            assert code == 200, (code, value)
            return current
        raise AssertionError("ARP 清理查询版本持续变化")

    def attach(self):
        self.traffic = ArpTraffic(self)
        self.observer = ArpNeighbor(self.traffic)
        return self.traffic

    def detach(self):
        observer, self.observer = self.observer, None
        try:
            if observer is not None:
                observer.close()
        finally:
            super().detach()

    def cleanup(self):
        """观察线程报告异常时仍关闭正式进程，失败验收不能留下占用 TAP 的后台服务"""
        try:
            self.detach()
        finally:
            super().cleanup()

    def queue(self, flow, ingress=0):
        """直接注入一帧，保留唯一负载标识，后续确认它被释放一次或确实未被转发"""
        self.traffic.sequence += 1
        tag = self.traffic.sequence
        frame, marker = self.traffic.frame(flow, tag, ingress)
        before = self.traffic.stats()["rx_packets"]
        self.traffic.links[ingress].send(frame)
        self.report["packets"]["injected"] += 1
        wait(lambda: self.traffic.stats()["rx_packets"] > before)
        return tag, marker

    def released(self, original, translated, tag, marker, ingress=0, output=1, learn_snat=False):
        wait(lambda: any(marker in data for _, data in self.observer.packets))
        matches = [(port, data) for port, data in self.observer.packets if marker in data]
        if learn_snat:
            """ARP 重试会释放未提交的候选端口，只接受合法动态池，然后独立重算整帧与校验和"""
            assert len(matches) == 1 and matches[0][0] == output, matches
            offset = 14 + 4 * int(original.vlan) + 20
            port = struct.unpack_from("!H", matches[0][1], offset)[0]
            assert port in (40001, 40002), "ARP 首包占用了静态 DNAT 保留端口或池外端口"
            translated = replace(translated, source_port=port)
        expected, _ = self.traffic.frame(replace(translated, ttl=original.ttl - 1), tag, ingress, output)
        assert matches == [(output, expected)], ([(port, data.hex()) for port, data in matches], expected.hex())
        self.report["packets"]["forwarded"] += 1
        return translated

    def absent(self, marker):
        assert not any(marker in data for _, data in self.observer.packets), "已经清理或拒绝的等待报文仍然发出"

def arp_policy(reachable=30000, vlans=(), enabled=True):
    return {"reachable_ms": reachable if enabled else 0,
            "locals": [{"port_id": port, "address": address, "vlans": list(vlans)} for port, address in
                       ((0, "192.0.2.1"), (1, "192.0.2.2"), (1, "203.0.113.9"))] if enabled else []}


def dynamic_routes(sequence, revision, static=False):
    value = routes(sequence, revision)
    if not static:
        value["neighbors"] = []
    return value


def wait_preferred(s):
    wait(s.ready)
    status = s.arp("status")
    assert status["enabled"] and all(item["state"] == "preferred" for item in status["locals"]), status


def neighbor_state(s, address, state):
    return any(item["address"] == address and item["state"] == state for item in s.arp("neighbors")["entries"])


def control_body(frame):
    _, l2 = decoded(frame, [])
    body = frame[l2:l2 + 28]
    assert len(frame) == 60 and body[:7] == bytes.fromhex("00010800060400")
    return body



def verify(s):
    s.initialize()
    s.process = s.start()
    wait(s.ready)
    s.start_gateway()
    s.request_sequence = 1
    s.submit(dynamic_routes(1, s.routing()["revision"]))
    traffic = s.attach()
    original = Flow()
    assert not s.arp("status")["enabled"] and s.arp()["policy"] == arp_policy(enabled=False)
    traffic.exchange(original, reason="no_neighbor_drops")
    body = s.arp_request(arp_policy())
    assert s.request("GET", "/v1/arp/policy", auth=False)[0] == 401
    for role in ("reader", "operator"):
        assert s.request("POST", "/v1/requests", body, role=role)[0] == 403
    for key in ("reset_neighbors", "expected_neighbor_epoch", "expected_neighbor_revision"):
        incomplete = {name: value for name, value in body.items() if name != key}
        assert s.request("POST", "/v1/requests", incomplete, role="admin")[0] == 400
    installed = s.submit(body)
    assert installed["applied"] and not s.ready(), "冲突检测未完成就报告就绪"
    traffic.exchange(original, reason="arp_not_ready_drops")
    wait_preferred(s)
    wait(lambda: len(s.observer.controls) >= 15)
    for item in arp_policy()["locals"]:
        frames = [control_body(frame) for port, frame in s.observer.controls if port == item["port_id"]
                  and control_body(frame)[24:28] == socket.inet_aton(item["address"])]
        assert sum(frame[14:18] == bytes(4) for frame in frames) == 3
        assert sum(frame[14:18] == frame[24:28] for frame in frames) == 2
        assert all(frame[7] == 1 and frame[18:24] == bytes(6) for frame in frames)
    assert s.submit(body) == installed
    assert s.request("POST", "/v1/requests", dict(body, policy=arp_policy(30001)), role="admin")[0] == 409
    s.observer.auto = True
    roundtrip(traffic, original, original)
    assert not s.view("status")["enabled"]
    s.report["steps"].append("admin_enable_idempotency_three_acd_probes_two_announcements_and_stateless_forwarding")

    s.flush_arp()
    s.observer.auto = False
    s.apply(nat_policy(nat=True))
    tag, marker = s.queue(original)
    wait(lambda: int(s.arp("status")["pending"]) == 1)
    assert int(s.view("status")["active"]) == 0
    invalid = int(s.arp("status")["invalid"])
    s.observer.send(1, 2, "192.0.2.20", "192.0.2.2", corrupt=True)
    s.observer.send(1, 3, "192.0.2.20", "192.0.2.2")
    s.observer.send(1, 2, "192.0.2.20", "192.0.2.2", vlans=(37,))
    wait(lambda: int(s.arp("status")["invalid"]) >= invalid + 3)
    assert int(s.arp("status")["pending"]) == 1 and int(s.view("status")["active"]) == 0
    s.absent(marker)
    s.observer.send(1, 2, "192.0.2.20", "192.0.2.2")
    wire = s.released(original, replace(original, source="203.0.113.9"), tag, marker, learn_snat=True)
    s.observer.auto = True
    roundtrip(traffic, original, wire)
    before = len(s.observer.controls)
    s.observer.send(1, 1, "192.0.2.88", "203.0.113.9")
    wait(lambda: any(control_body(frame)[7] == 2 for _, frame in s.observer.controls[before:]))
    answer = next(frame for _, frame in s.observer.controls[before:] if control_body(frame)[7] == 2)
    assert answer[:6] == traffic.peer_macs[1]
    assert control_body(answer)[14:18] == socket.inet_aton("203.0.113.9")
    assert not any(item["address"] == "192.0.2.88" for item in s.arp("neighbors")["entries"])
    s.observer.send(1, 2, "192.0.2.20", "192.0.2.2", mac=bytes.fromhex("020000005501"))
    time.sleep(.05)
    roundtrip(traffic, original, wire)
    s.report["steps"].append("first_packet_waits_without_ct_commit_bad_arp_rejected_nat_reverse_local_reply_and_unsolicited_protection")

    saved = s.state.read_bytes()
    page = s.arp("neighbors")
    stale_page = "/v1/arp/neighbors?after=" + page["next_id"] + "&epoch=" + page["epoch"] + "&revision=" + page["revision"]
    assert s.request("GET", stale_page)[0] == 200
    current = s.arp("status")
    flush = {"epoch": current["epoch"], "revision": current["revision"]}
    assert s.request("POST", "/v1/arp/neighbors/flush", flush)[0] == 403
    assert s.request("POST", "/v1/arp/neighbors/flush", dict(flush, epoch=str(int(flush["epoch"]) ^ 1 or 2)),
                     role="operator")[0] == 409
    s.flush_arp()
    assert s.state.read_bytes() == saved and int(s.view("status")["active"]) == 1
    assert s.request("GET", stale_page)[0] == 409
    roundtrip(traffic, original, wire)
    before = s.arp()
    s.apply_arp(arp_policy(30001), status=-16)
    s.apply_arp(arp_policy(), reset=True, status=-116, revision=str(int(before["revision"]) - 1))
    s.apply_arp(arp_policy(), reset=True, status=-116,
                expected_neighbor_epoch=str(int(before["neighbor_epoch"]) ^ 1 or 2))
    assert s.arp()["policy"] == before["policy"] and int(s.view("status")["active"]) == 1
    s.request_sequence += 1
    static = dynamic_routes(s.request_sequence, s.routing()["revision"], static=True)
    static["neighbors"][1]["mac"] = "02:00:00:00:44:01"
    s.submit(static)
    traffic.peer_macs[1] = bytes.fromhex("020000004401")
    saved = s.state.read_bytes()
    traffic.exchange(original, output=1, translated=wire)
    assert s.state.read_bytes() == saved
    traffic.peer_macs[1] = bytes.fromhex("020000002001")
    s.request_sequence += 1
    s.submit(dynamic_routes(s.request_sequence, s.routing()["revision"]))
    s.report["steps"].append("epoch_revision_guards_flush_preserves_file_and_ct_static_mapping_takes_priority")

    s.observer.auto = False
    s.flush_arp()
    tag, marker = s.queue(original)
    wait(lambda: int(s.arp("status")["pending"]) == 1)
    s.apply_arp(arp_policy(1000), reset=True)
    assert int(s.arp("status")["pending"]) == 0 and int(s.view("status")["active"]) == 1
    wait_preferred(s)
    s.absent(marker)
    s.report["packets"]["dropped"] += 1
    s.observer.auto = True
    roundtrip(traffic, original, wire)
    s.observer.auto = False
    wait(lambda: neighbor_state(s, "192.0.2.20", "stale"), seconds=3)
    control_start = len(s.observer.controls)
    traffic.exchange(original, output=1, translated=wire)
    wait(lambda: neighbor_state(s, "192.0.2.20", "failed"), seconds=10)
    probes = [frame for port, frame in s.observer.controls[control_start:] if port == 1
              and control_body(frame)[24:28] == socket.inet_aton("192.0.2.20")]
    assert len(probes) == 3 and all(frame[:6] == bytes([255]) * 6 for frame in probes)
    traffic.exchange(original, reason="no_neighbor_drops")
    s.report["steps"].append("explicit_reset_discards_waiter_preserves_ct_and_refresh_uses_three_bounded_broadcasts")

    s.flush_arp()
    before = traffic.stats()["arp_queue_drops"]
    pending = [s.queue(original) for _ in range(8)]
    wait(lambda: int(s.arp("status")["pending"]) == 4 and traffic.stats()["arp_queue_drops"] >= before + 4)
    wait(lambda: int(s.arp("status")["pending"]) == 0, seconds=7)
    for _, marker in pending:
        s.absent(marker)
    assert traffic.stats()["arp_queue_drops"] >= before + 8
    s.report["packets"]["dropped"] += 8
    s.flush_arp()
    tag, marker = s.queue(original)
    wait(lambda: int(s.arp("status")["pending"]) == 1)
    s.cli("request-submit", 44, 1, "flow-create", 1, 0, "apply-drop", 700, 0, 0)
    created = wait(lambda: (value if "phase=done" in (value := s.cli("request-get", 44, 1).stdout) else None))
    generation = re.search(r"^id=700 generation=(\d+)$", created, re.MULTILINE)[1]
    s.observer.send(1, 2, "192.0.2.20", "192.0.2.2")
    wait(lambda: int(s.arp("status")["pending"]) == 0)
    s.absent(marker)
    s.report["packets"]["dropped"] += 1
    s.cli("request-submit", 44, 2, "flow-remove", 0, 700, generation)
    wait(lambda: "phase=done" in s.cli("request-get", 44, 2).stdout)
    s.report["steps"].append("four_packets_per_neighbor_bounded_timeout_and_waiters_recheck_current_acl")

    s.flush_arp()
    s.apply_arp(arp_policy(), reset=True)
    s.observer.send(1, 1, "0.0.0.0", "192.0.2.2")
    wait(lambda: any(item["state"] == "duplicate" for item in s.arp("status")["locals"]))
    assert not s.ready() and "arp-not-ready" in s.cli("ready", ok=False).stderr + s.cli("ready", ok=False).stdout
    s.flush_arp()
    assert any(item["state"] == "duplicate" for item in s.arp("status")["locals"])
    traffic.exchange(original, reason="arp_not_ready_drops")
    s.apply_arp(arp_policy(), reset=True)
    wait_preferred(s)
    s.observer.send(1, 1, "192.0.2.2", "192.0.2.2")
    wait(lambda: not s.ready())
    assert any(item["state"] == "duplicate" for item in s.arp("status")["locals"])
    s.report["steps"].append("simultaneous_probe_and_later_address_conflict_withdraw_local_address_flush_cannot_clear_conflict")

    """ND 与 ARP 保存到同一文件，但清缓存和策略替换各自拥有独立的版本与启动身份"""
    code, nd = s.request("GET", "/v1/nd6/policy")
    assert code == 200
    nd = nd["data"]
    s.request_sequence += 1
    nd_policy = {"reachable_ms": 30000, "locals": [{"port_id": 2, "address": "2001:db8:3::1", "vlans": []}]}
    nd_body = {"operation": "nd6_replace", "sequence": str(s.request_sequence), "revision": nd["revision"],
               "expected_neighbor_epoch": nd["neighbor_epoch"], "expected_neighbor_revision": nd["neighbor_revision"],
               "reset_neighbors": False, "policy": nd_policy}
    s.submit(nd_body)
    s.apply_arp(arp_policy(vlans=(37,)), reset=True)
    traffic.vlans = (37,)
    s.observer.auto = True
    wait_preferred(s)
    tagged = replace(original, vlan=True)
    tagged_wire = replace(wire, vlan=True)
    roundtrip(traffic, tagged, tagged_wire)
    exported = s.directory / "arp-policy.txt"
    exported.write_text(s.cli("arp-policy-show").stdout)
    current = s.arp()
    s.cli("request-submit", 55, 1, "arp-replace", exported, current["revision"], "true",
          current["neighbor_epoch"], current["neighbor_revision"])
    completed = wait(lambda: (value if "phase=done" in (value := s.cli("request-get", 55, 1).stdout) else None))
    assert "status=0 applied=yes durable=yes" in completed, completed
    wait_preferred(s)
    assert s.arp()["policy"] == current["policy"]
    roundtrip(traffic, tagged, tagged_wire)
    nd_status = s.request("GET", "/v1/nd6/status")[1]["data"]
    s.flush_arp()
    assert s.request("GET", "/v1/nd6/status")[1]["data"] == nd_status
    ipv6 = Flow6()
    # 独立验证 IPv6 的静态邻居和 NAT66 不受 ARP 标签配置影响
    tag6 = replace(ipv6, source="2001:db8:100::9", source_port=40001)
    roundtrip(traffic, ipv6, tag6)
    s.report["steps"].append("single_vlan_arp_nd_policy_coexistence_and_ipv6_nat66_unaffected")

    snapshot, old = s.state.read_bytes(), s.arp()
    old_nd = s.request("GET", "/v1/nd6/policy")[1]["data"]
    s.detach()
    s.stop(kill=True)
    s.process = s.start(fault=True)
    wait(lambda: s.control.exists())
    traffic = s.attach()
    traffic.vlans = (37,)
    s.observer.auto = True
    wait_preferred(s)
    recovered = s.arp()
    assert recovered["policy"] == old["policy"] and recovered["generation"] == old["generation"]
    assert recovered["neighbor_epoch"] != old["neighbor_epoch"] and s.state.read_bytes() == snapshot
    assert s.request("GET", "/v1/nd6/policy")[1]["data"]["policy"] == old_nd["policy"]
    assert int(s.arp("status")["active"]) == 0 and int(s.view("status")["active"]) == 0
    assert s.request("POST", "/v1/arp/neighbors/flush",
                     {"epoch": old["neighbor_epoch"], "revision": recovered["neighbor_revision"]}, role="operator")[0] == 409
    roundtrip(traffic, tagged, tagged_wire)
    s.report["steps"].append("sigkill_recovers_arp_nd_policies_and_receipts_with_new_epoch_and_empty_caches")

    s.flush_arp()
    fault_body = s.arp_request(arp_policy(31000, (37,)), reset=True)
    s.marker.write_text("after=2\n")
    code, _ = s.request("POST", "/v1/requests", fault_body, role="admin")
    assert code in (200, 202, 503)
    wait(lambda: "workers=0/1" in s.cli("health").stdout)
    assert not s.ready() and s.arp()["revision"] != s.arp()["durable_revision"]
    code, blocked = s.request("GET", "/v1/requests/" + fault_body["sequence"], role="admin")
    assert code == 503 and blocked["data"]["applied"] and not blocked["data"]["durable"] and blocked["data"]["blocked"]
    s.marker.unlink()
    s.cli("persistence-flush")
    wait_preferred(s)
    result = wait(lambda: s.completed(fault_body["sequence"]))
    assert result["applied"] and s.submit(fault_body) == result
    roundtrip(traffic, tagged, tagged_wire)
    s.apply_arp(arp_policy(enabled=False), status=-16)
    s.apply_arp(arp_policy(enabled=False), reset=True)
    traffic.exchange(tagged, reason="no_neighbor_drops")
    s.request_sequence += 1
    s.submit(dynamic_routes(s.request_sequence, s.routing()["revision"], static=True))
    roundtrip(traffic, tagged, tagged_wire)
    s.report["steps"].append("post_apply_fsync_failure_pauses_workers_explicit_flush_resumes_idempotently_and_disable_restores_static_only")
    """保存失败退出时仍须先归还 ARP 等待包，再释放网口缓冲池"""
    s.request_sequence += 1
    s.submit(dynamic_routes(s.request_sequence, s.routing()["revision"]))
    s.apply_arp(arp_policy(vlans=(37,)), reset=True)
    wait_preferred(s)
    s.observer.auto = False
    _, pending_marker = s.queue(tagged)
    wait(lambda: int(s.arp("status")["pending"]) == 1)
    s.marker.write_text("after=2\n")
    s.cli("request-submit", 44, 3, "flow-create", 1, 0, "apply-drop", 701, 0, 0, ok=False)
    wait(lambda: "dirty=yes" in s.cli("flow-status").stdout)
    wait(lambda: "workers=0/1" in s.cli("health").stdout)
    assert int(s.arp("status")["pending"]) == 1 and not s.ready()
    s.absent(pending_marker)
    s.detach()
    s.process.terminate()
    assert s.process.wait(timeout=25) == 1
    s.report["packets"]["dropped"] += 1
    s.report["steps"].append("dirty_shutdown_releases_pending_packet_before_mempool_teardown_and_reports_failure")
    s.gateway.terminate()
    assert s.gateway.wait(timeout=20) == 0
    for log in s.logs:
        log.flush()
    for path in s.output.glob("*.log"):
        contents = path.read_text(errors="replace")
        assert not any(token in contents for token in s.tokens.values()), "日志泄露认证令牌"
        assert "ERROR: AddressSanitizer" not in contents and "runtime error:" not in contents, path.name
    assert s.report["packets"]["injected"] == s.report["packets"]["forwarded"] + s.report["packets"]["dropped"]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--report-dir", type=Path, required=True)
    parser.add_argument("--tap", action="store_true")
    args = parser.parse_args()
    build, output = args.build_dir.resolve(), args.report_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binaries = [build / name for name in ("dppd", "dppctl", "dppd-flow-state", "tests/integration/test_m5_daemon_fault")]
    report = {"success": False, "scope": "managed_dynamic_arp_three_tap", "hardware_acceptance": "not_run",
              "source_sha256": source_digest(ROOT), "steps": [],
              "packets": {"injected": 0, "forwarded": 0, "dropped": 0, "worker_stopped": 0, "buffered_then_acd_drop": 0},
              "binary_sha256": {str(path.relative_to(build)): hashlib.sha256(path.read_bytes()).hexdigest() for path in binaries},
              "started_utc": datetime.now(timezone.utc).isoformat()}
    try:
        if not args.tap or os.geteuid() != 0 or os.readlink("/proc/self/ns/net") == os.readlink("/proc/1/ns/net"):
            raise RuntimeError("验收必须指定 --tap 并在 root 私有网络命名空间中执行")
        for group in ("default", "all"):
            Path(f"/proc/sys/net/ipv6/conf/{group}/disable_ipv6").write_text("1\n")
        with tempfile.TemporaryDirectory(prefix="dppd-managed-arp-") as temporary:
            scenario = ManagedArp(build, output, Path(temporary), report, True)
            try:
                verify(scenario)
                assert source_digest(ROOT) == report["source_sha256"], "验收期间源码发生变化"
                report["success"] = True
            finally:
                if not report["success"] and scenario.process is not None and scenario.process.poll() is None:
                    for command in ("arp-status", "arp-list", "stats", "health"):
                        scenario.cli(command, ok=False)
                scenario.cleanup()
                report["all_children_exited"] = all(child.poll() is not None for child in scenario.children)
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
