#!/usr/bin/env python3
"""通过正式管理入口和三 TAP 验收动态 ND 配置、等待报文、NAT66 与故障恢复"""

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
from nd6_traffic import Fixture, Neighbor, decoded


class NdTraffic(Traffic):
    """报文与校验和沿用独立测试模型，邻居应答使用另外的原始套接字观察线程"""

    def __init__(self, scenario):
        super().__init__(scenario)
        self.names = scenario.names
        self.vlans = 0
        self.peer_macs = [bytes.fromhex(value) for value in ("020000001000", "020000002001", "020000003002")]

    def control(self, *args, **kwargs):
        return Fixture.control(self, *args, **kwargs)

    def frame(self, flow, tag, ingress, output=None):
        wire, marker = super().frame(flow, tag, ingress, output)
        if isinstance(flow, Flow6) and output is not None:
            wire = self.peer_macs[output] + wire[6:]
        return wire, marker

    def exchange(self, *args, **kwargs):
        self.scenario.report["packets"]["injected"] += 1
        return super().exchange(*args, **kwargs)


class ManagedNd(ManagedNat):
    def __init__(self, *args):
        super().__init__(*args)
        self.observer = None

    def nd(self, resource="policy"):
        code, value = self.request("GET", "/v1/nd6/" + resource)
        assert code == 200, (resource, code, value)
        return value["data"]

    def nd_request(self, policy, reset=False, **overrides):
        current = self.nd()
        self.request_sequence += 1
        return {"operation": "nd6_replace", "sequence": str(self.request_sequence), "revision": current["revision"],
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

    def apply_nd(self, policy, reset=False, status=0, **overrides):
        """只有明确返回 ESTALE 且没有应用时才换新键重试，不能重做结果不明的清理请求"""
        for _ in range(8):
            body = self.nd_request(policy, reset, **overrides)
            code, reply = self.request("POST", "/v1/requests", body, role="admin")
            assert code in (200, 202), (code, reply)
            result = wait(lambda: self.completed(body["sequence"]))
            if result["status"] == -116 and status != -116 and not overrides:
                assert not result["applied"]
                continue
            assert result["status"] == status and result["applied"] == (status == 0), result
            return body, result
        raise AssertionError("ND 状态持续变化，无法取得可提交的精确版本")

    def flush_nd(self):
        for _ in range(8):
            current = self.nd("status")
            code, value = self.request("POST", "/v1/nd6/neighbors/flush",
                                       {"epoch": current["epoch"], "revision": current["revision"]}, role="operator")
            if code == 409:
                continue
            assert code == 200, (code, value)
            return current
        raise AssertionError("ND 清理查询版本持续变化")

    def attach(self):
        self.traffic = NdTraffic(self)
        self.observer = Neighbor(self.traffic)
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
            """ND 重试会释放未提交的候选端口，只接受合法动态池，然后独立重算整帧与校验和"""
            assert len(matches) == 1 and matches[0][0] == output, matches
            offset = 14 + 4 * original.vlans + 40 + 8 * len(original.extensions)
            port = struct.unpack_from("!H", matches[0][1], offset)[0]
            assert port in (40001, 40002), "ND 首包占用了静态 DNAT 保留端口或池外端口"
            translated = replace(translated, source_port=port)
        expected, _ = self.traffic.frame(replace(translated, hop=original.hop - 1), tag, ingress, output)
        assert matches == [(output, expected)], ([(port, data.hex()) for port, data in matches], expected.hex())
        self.report["packets"]["forwarded"] += 1
        return translated

    def absent(self, marker):
        assert not any(marker in data for _, data in self.observer.packets), "已经清理或拒绝的等待报文仍然发出"


def nd_policy(reachable=30000, vlans=(), enabled=True):
    return {"reachable_ms": reachable if enabled else 0,
            "locals": [{"port_id": port, "address": address, "vlans": list(vlans)} for port, address in
                       ((0, "2001:db8:1::fe"), (1, "2001:db8:2::fe"),
                        (1, "2001:db8:100::9"), (1, "2001:db8:100::a"))] if enabled else []}


def dynamic_routes(sequence, revision, static=False):
    value = routes(sequence, revision)
    if not static:
        value["neighbors6"] = []
    return value


def control_fields(frame):
    _, l2 = decoded(frame, [])
    ip, body = frame[l2:l2 + 40], frame[l2 + 40:]
    assert ip[6:8] == b"\x3a\xff" and len(body) == int.from_bytes(ip[4:6], "big")
    assert crc(ip[8:40] + struct.pack("!I3xB", len(body), 58) + body) == 0
    return ip, body


def wait_preferred(s):
    wait(s.ready)
    status = s.nd("status")
    assert status["enabled"] and all(item["state"] == "preferred" for item in status["locals"]), status


def neighbor_state(s, address, state):
    return any(item["address"] == address and item["state"] == state for item in s.nd("neighbors")["entries"])


def learn_mapping(s, original):
    tag, marker = s.queue(original)
    mapped = s.released(original, replace(original, source="2001:db8:100::9"), tag, marker, learn_snat=True)
    s.traffic.exchange(mapped.reverse(), ingress=1, output=0, translated=original.reverse())
    return mapped


def verify(s):
    s.initialize()
    s.process = s.start()
    wait(s.ready)
    s.start_gateway()
    s.request_sequence = 1
    s.submit(dynamic_routes(1, s.routing()["revision"]))
    traffic = s.attach()
    original, wire = Flow6(), replace(Flow6(), source="2001:db8:100::9", source_port=40001)
    assert s.nd()["policy"] == nd_policy(enabled=False) and not s.nd("status")["enabled"]
    traffic.exchange(original, reason="no_neighbor_drops")
    body = s.nd_request(nd_policy())
    assert s.request("GET", "/v1/nd6/policy", auth=False)[0] == 401
    for role in ("reader", "operator"):
        assert s.request("POST", "/v1/requests", body, role=role)[0] == 403
    for key in ("reset_neighbors", "expected_neighbor_epoch", "expected_neighbor_revision"):
        incomplete = {name: value for name, value in body.items() if name != key}
        assert s.request("POST", "/v1/requests", incomplete, role="admin")[0] == 400
    installed = s.submit(body)
    assert installed["applied"] and not s.ready(), "本机 IPv6 地址尚未完成 DAD 却报告就绪"
    wait_preferred(s)
    targets = set()
    for _, frame in s.observer.controls:
        ip, control = control_fields(frame)
        if control[0] == 135 and ip[8:24] == bytes(16):
            assert len(control) == 24, "DAD 不应携带源 MAC 选项"
            targets.add(socket.inet_ntop(socket.AF_INET6, control[8:24]))
    assert targets == {item["address"] for item in nd_policy()["locals"]}
    assert s.submit(body) == installed
    assert s.request("POST", "/v1/requests", dict(body, policy=nd_policy(30001)), role="admin")[0] == 409
    s.observer.auto = True
    roundtrip(traffic, original, original)
    assert not s.view("status")["enabled"] and int(s.view("status")["active"]) == 0
    s.flush_nd()
    s.observer.auto = False
    s.apply(nat_policy(nat=True))
    s.report["steps"].append("disabled_static_only_admin_enable_dad_readiness_and_durable_idempotency")

    tag, marker = s.queue(original)
    wait(lambda: int(s.nd("status")["pending"]) == 1)
    assert int(s.view("status")["active"]) == 0
    wait(lambda: any(port == 1 and control_fields(frame)[0][8:24] != bytes(16)
                     for port, frame in s.observer.controls))
    for hop, corrupt in ((254, False), (255, True)):
        s.observer.send(1, 136, "fe80::1", "2001:db8:2::fe", "fe80::1", flags=0x60, hop=hop, corrupt=corrupt)
    wait(lambda: int(s.nd("status")["invalid"]) >= 2)
    assert int(s.nd("status")["pending"]) == 1 and int(s.view("status")["active"]) == 0
    s.absent(marker)
    s.observer.send(1, 136, "fe80::1", "2001:db8:2::fe", "fe80::1", flags=0x60)
    wire = s.released(original, wire, tag, marker, learn_snat=True)
    wait(lambda: int(s.nd("status")["pending"]) == 0 and int(s.view("status")["active"]) == 1)
    s.observer.auto = True
    roundtrip(traffic, original, wire)
    before = len(s.observer.controls)
    s.observer.send(1, 135, "2001:db8:2::7", "ff02::1:ff00:9", "2001:db8:100::9")
    wait(lambda: any(control_fields(frame)[1][0] == 136 for _, frame in s.observer.controls[before:]))
    answer = next(frame for _, frame in s.observer.controls[before:] if control_fields(frame)[1][0] == 136)
    assert control_fields(answer)[1][4] == 0xe0
    s.report["steps"].append("held_first_packet_valid_na_releases_once_nat66_reverse_and_local_public_address_answer")

    saved = s.state.read_bytes()
    for index in range(9):
        s.observer.send(0, 135, f"fe80::{100 + index:x}", "ff02::1:ff00:fe", "2001:db8:1::fe")
    wait(lambda: int(s.nd("status")["active"]) >= 11)
    page = s.nd("neighbors")
    assert page["more"] and len(page["entries"]) == 8
    next_path = ("/v1/nd6/neighbors?after=" + page["next_id"] + "&epoch=" + page["epoch"] +
                 "&revision=" + page["revision"])
    assert s.request("GET", next_path)[0] == 200
    current = s.nd("status")
    flush = {"epoch": current["epoch"], "revision": current["revision"]}
    assert s.request("POST", "/v1/nd6/neighbors/flush", flush)[0] == 403
    assert s.request("POST", "/v1/nd6/neighbors/flush", dict(flush, epoch=str(int(flush["epoch"]) ^ 1 or 2)),
                     role="operator")[0] == 409
    s.flush_nd()
    assert s.state.read_bytes() == saved and int(s.view("status")["active"]) == 1
    assert s.request("GET", next_path)[0] == 409
    roundtrip(traffic, original, wire)
    before = s.nd()
    s.apply_nd(nd_policy(30001), status=-16)
    s.apply_nd(nd_policy(), reset=True, status=-116, revision=str(int(before["revision"]) - 1))
    s.apply_nd(nd_policy(), reset=True, status=-116,
               expected_neighbor_epoch=str(int(before["neighbor_epoch"]) ^ 1 or 2))
    unbound = nd_policy()
    unbound["locals"][0]["port_id"] = 3
    unbound_body = s.nd_request(unbound, reset=True)
    code, rejected = s.request("POST", "/v1/requests", unbound_body, role="admin")
    assert code == 503 and rejected["status"] == -19
    assert s.request("GET", "/v1/requests/" + unbound_body["sequence"], role="admin")[0] == 404
    assert s.nd()["policy"] == before["policy"] and int(s.view("status")["active"]) == 1
    roundtrip(traffic, original, wire)
    s.request_sequence += 1
    static = dynamic_routes(s.request_sequence, s.routing()["revision"], static=True)
    static["neighbors6"][1]["mac"] = "02:00:00:00:44:01"
    s.submit(static)
    old_mac = traffic.peer_macs[1]
    traffic.peer_macs[1] = bytes.fromhex("020000004401")
    saved = s.state.read_bytes()
    s.observer.send(1, 136, "fe80::1", "2001:db8:2::fe", "fe80::1", flags=0x60, mac=old_mac)
    traffic.exchange(original, output=1, translated=wire)
    assert s.state.read_bytes() == saved, "动态邻居学习不应改写静态配置"
    traffic.peer_macs[1] = old_mac
    s.request_sequence += 1
    s.submit(dynamic_routes(s.request_sequence, s.routing()["revision"]))
    traffic.exchange(original, output=1, translated=wire)
    s.report["steps"].append("paged_epoch_guards_runtime_flush_failed_policy_keeps_ct_and_static_neighbor_has_priority")

    s.observer.auto = False
    s.flush_nd()
    tag, marker = s.queue(original)
    wait(lambda: int(s.nd("status")["pending"]) == 1)
    dropped_before = int(s.nd("status")["queue_drops"])
    s.apply_nd(nd_policy(1000), status=-16)
    s.apply_nd(nd_policy(1000), reset=True)
    assert int(s.nd("status")["pending"]) == 0 and int(s.nd("status")["active"]) == 0
    assert int(s.nd("status")["queue_drops"]) > dropped_before and int(s.view("status")["active"]) == 1
    s.observer.send(1, 136, "fe80::1", "2001:db8:2::fe", "fe80::1", flags=0x60)
    wait_preferred(s)
    s.absent(marker)
    s.report["packets"]["dropped"] += 1
    s.observer.auto = True
    roundtrip(traffic, original, wire)
    s.observer.auto = False
    wait(lambda: neighbor_state(s, "fe80::1", "stale"), seconds=3)
    control_start = len(s.observer.controls)
    traffic.exchange(original, output=1, translated=wire)
    wait(lambda: neighbor_state(s, "fe80::1", "probe"), seconds=7)
    wait(lambda: neighbor_state(s, "fe80::1", "failed"), seconds=5)
    probes = [frame for port, frame in s.observer.controls[control_start:]
              if port == 1 and frame[:6] == traffic.peer_macs[1] and control_fields(frame)[1][0] == 135]
    assert len(probes) == 3
    traffic.exchange(original, reason="no_neighbor_drops")
    s.report["steps"].append("explicit_reset_cancels_waiters_preserves_ct_restarts_dad_and_nud_bounded_unicast_probes")

    s.flush_nd()
    before = traffic.stats()["nd6_queue_drops"]
    pending = [s.queue(original) for _ in range(8)]
    wait(lambda: int(s.nd("status")["pending"]) == 4 and traffic.stats()["nd6_queue_drops"] >= before + 4)
    wait(lambda: int(s.nd("status")["pending"]) == 0, seconds=7)
    for _, marker in pending:
        s.absent(marker)
    assert traffic.stats()["nd6_queue_drops"] >= before + 8
    s.report["packets"]["dropped"] += 8
    s.flush_nd()
    tag, marker = s.queue(original)
    wait(lambda: int(s.nd("status")["pending"]) == 1)
    s.cli("request-submit", 44, 1, "flow-create", 1, 0, "apply-drop", 700, 0, 0)
    created = wait(lambda: (value if "phase=done" in (value := s.cli("request-get", 44, 1).stdout) else None))
    generation = re.search(r"^id=700 generation=(\d+)$", created, re.MULTILINE)[1]
    s.observer.send(1, 136, "fe80::1", "2001:db8:2::fe", "fe80::1", flags=0x60)
    wait(lambda: int(s.nd("status")["pending"]) == 0)
    s.absent(marker)
    s.report["packets"]["dropped"] += 1
    s.cli("request-submit", 44, 2, "flow-remove", 0, 700, generation)
    wait(lambda: "phase=done" in s.cli("request-get", 44, 2).stdout)
    s.report["steps"].append("bounded_first_packet_queue_timeout_reclaims_all_and_retries_recheck_current_acl")

    s.flush_nd()
    s.apply_nd(nd_policy(), reset=True)
    s.observer.send(1, 136, "fe80::1", "ff02::1", "2001:db8:2::fe", flags=0x20)
    wait(lambda: any(item["state"] == "duplicate" for item in s.nd("status")["locals"]))
    assert not s.ready()
    s.flush_nd()
    assert any(item["state"] == "duplicate" for item in s.nd("status")["locals"])
    traffic.exchange(original, reason="nd6_not_ready_drops")
    original4, wire4 = Flow(), replace(Flow(), source="203.0.113.9", source_port=40001)
    roundtrip(traffic, original4, wire4)
    s.apply_nd(nd_policy(), reset=True)
    wait_preferred(s)
    s.observer.auto = True
    roundtrip(traffic, original, wire)
    s.report["steps"].append("dad_duplicate_blocks_ipv6_and_readiness_not_ipv4_flush_does_not_clear_conflict_policy_repairs")

    s.observer.auto = False
    s.flush_nd()
    traffic.vlans = 2
    s.apply_nd(nd_policy(vlans=(37, 38)), reset=True)
    wait_preferred(s)
    traffic.exchange(original, reason="no_neighbor_drops")
    s.observer.auto = True
    tagged = replace(original, vlans=2)
    roundtrip(traffic, tagged, replace(wire, vlans=2))
    exported = s.directory / "nd-policy.txt"
    exported.write_text(s.cli("nd6-policy-show").stdout)
    current = s.nd()
    s.cli("request-submit", 55, 1, "nd6-replace", exported, current["revision"], "true",
          current["neighbor_epoch"], current["neighbor_revision"])
    completed = wait(lambda: (value if "phase=done" in (value := s.cli("request-get", 55, 1).stdout) else None))
    assert "status=0 applied=yes durable=yes" in completed, completed
    wait_preferred(s)
    assert s.nd()["policy"] == current["policy"]
    roundtrip(traffic, tagged, replace(wire, vlans=2))
    s.report["steps"].append("double_vlan_domain_rejects_untagged_data_and_cli_export_roundtrip_matches_http")

    before_restart, saved = s.nd(), s.state.read_bytes()
    s.detach()
    s.stop(kill=True)
    s.process = s.start()
    wait(lambda: s.control.exists() and all(name in {entry for _, entry in socket.if_nameindex()} for name in s.names))
    traffic = s.attach()
    traffic.vlans = 2
    recovered = s.nd()
    assert s.state.read_bytes() == saved
    for key in ("policy", "generation", "revision", "durable_revision"):
        assert recovered[key] == before_restart[key]
    assert recovered["neighbor_epoch"] != before_restart["neighbor_epoch"]
    assert int(s.nd("status")["active"]) == 0 and int(s.view("status")["active"]) == 0
    assert s.request("POST", "/v1/nd6/neighbors/flush",
                     {"epoch": before_restart["neighbor_epoch"], "revision": recovered["neighbor_revision"]},
                     role="operator")[0] == 409
    s.apply_nd(nd_policy(vlans=(37, 38)), reset=True, status=-116,
               expected_neighbor_epoch=before_restart["neighbor_epoch"])
    assert s.submit(body) == installed
    wait_preferred(s)
    s.observer.auto = True
    wire = learn_mapping(s, tagged)
    s.detach()
    s.stop()
    s.report["steps"].append("sigkill_restores_policy_and_receipts_restarts_dad_caches_and_ct_empty_old_epoch_rejected")

    s.process = s.start(fault=True)
    wait(lambda: s.control.exists() and all(name in {entry for _, entry in socket.if_nameindex()} for name in s.names))
    traffic = s.attach()
    traffic.vlans = 2
    wait_preferred(s)
    s.observer.auto = True
    wire = learn_mapping(s, tagged)
    changed = nd_policy(30001, vlans=(37, 38))
    fault_body = s.nd_request(changed, reset=True)
    s.marker.write_text("after=2\n")
    code, _ = s.request("POST", "/v1/requests", fault_body, role="admin")
    assert code in (200, 202, 503)
    wait(lambda: "dirty=yes" in s.cli("flow-status").stdout)
    wait(lambda: "workers=0/1" in s.cli("health").stdout)
    assert not s.ready() and s.nd()["policy"] == changed
    assert s.nd()["revision"] != s.nd()["durable_revision"]
    code, blocked = s.request("GET", "/v1/requests/" + fault_body["sequence"], role="admin")
    assert code == 503 and blocked["data"]["applied"] and not blocked["data"]["durable"] and blocked["data"]["blocked"]
    stopped = traffic.stats()
    frame, _ = traffic.frame(tagged, 999999, 0)
    traffic.links[0].send(frame)
    s.report["packets"]["injected"] += 1
    time.sleep(.15)
    assert traffic.stats()["rx_packets"] == stopped["rx_packets"]
    s.report["packets"]["worker_stopped"] += 1
    s.marker.unlink()
    s.cli("persistence-flush")
    wait(lambda: traffic.stats()["nd6_not_ready_drops"] > stopped["nd6_not_ready_drops"])
    s.report["packets"]["buffered_then_dad_drop"] += 1
    wait_preferred(s)
    result = wait(lambda: s.completed(fault_body["sequence"]))
    assert result["applied"] and s.submit(fault_body) == result
    assert all(item["state"] == "preferred" for item in s.nd("status")["locals"])
    roundtrip(traffic, tagged, replace(wire, vlans=2))
    s.report["steps"].append("post_apply_fsync_failure_pauses_workers_resume_keeps_dad_gate_and_retry_does_not_reset_again")

    s.apply_nd(nd_policy(enabled=False), status=-16)
    s.apply_nd(nd_policy(enabled=False), reset=True)
    assert not s.nd("status")["enabled"] and int(s.nd("status")["pending"]) == 0
    traffic.exchange(tagged, reason="no_neighbor_drops")
    s.request_sequence += 1
    s.submit(dynamic_routes(s.request_sequence, s.routing()["revision"], static=True))
    roundtrip(traffic, tagged, replace(wire, vlans=2))
    assert int(s.nd("status")["active"]) == 0
    s.report["steps"].append("explicit_disable_removes_dynamic_resolution_static_neighbor_route_and_nat66_remain_usable")

    """用普通流事务制造写盘失败，保留真实等待报文，检查退出时不把悬挂 mbuf 留给已销毁的池"""
    s.request_sequence += 1
    s.submit(dynamic_routes(s.request_sequence, s.routing()["revision"]))
    s.apply_nd(nd_policy(vlans=(37, 38)), reset=True)
    wait_preferred(s)
    s.observer.auto = False
    _, pending_marker = s.queue(tagged)
    wait(lambda: int(s.nd("status")["pending"]) == 1)
    s.marker.write_text("after=2\n")
    s.cli("request-submit", 44, 3, "flow-create", 1, 0, "apply-drop", 701, 0, 0, ok=False)
    wait(lambda: "dirty=yes" in s.cli("flow-status").stdout)
    wait(lambda: "workers=0/1" in s.cli("health").stdout)
    assert int(s.nd("status")["pending"]) == 1 and not s.ready()
    s.absent(pending_marker)
    s.detach()
    s.process.terminate()
    assert s.process.wait(timeout=25) == 1, "带未落盘状态退出必须报告失败，不能伪装成成功关闭"
    s.report["packets"]["dropped"] += 1
    s.report["steps"].append("dirty_shutdown_with_pending_packet_exits_failure_without_forwarding_or_sanitizer_error")
    s.gateway.terminate()
    assert s.gateway.wait(timeout=20) == 0
    for log in s.logs:
        log.flush()
    for path in s.output.glob("*.log"):
        contents = path.read_text(errors="replace")
        assert not any(token in contents for token in s.tokens.values()), "日志泄露认证令牌"
        assert "ERROR: AddressSanitizer" not in contents and "runtime error:" not in contents, path.name
    assert s.report["packets"]["injected"] == sum(s.report["packets"][key]
                                                 for key in ("forwarded", "dropped", "buffered_then_dad_drop"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--report-dir", type=Path, required=True)
    parser.add_argument("--tap", action="store_true")
    args = parser.parse_args()
    build, output = args.build_dir.resolve(), args.report_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binaries = [build / name for name in ("dppd", "dppctl", "dppd-flow-state", "tests/integration/test_m5_daemon_fault")]
    report = {"success": False, "scope": "managed_dynamic_nd_three_tap", "hardware_acceptance": "not_run",
              "source_sha256": source_digest(ROOT), "steps": [],
              "packets": {"injected": 0, "forwarded": 0, "dropped": 0, "worker_stopped": 0, "buffered_then_dad_drop": 0},
              "binary_sha256": {str(path.relative_to(build)): hashlib.sha256(path.read_bytes()).hexdigest() for path in binaries},
              "started_utc": datetime.now(timezone.utc).isoformat()}
    try:
        if not args.tap or os.geteuid() != 0 or os.readlink("/proc/self/ns/net") == os.readlink("/proc/1/ns/net"):
            raise RuntimeError("验收必须指定 --tap 并在 root 私有网络命名空间中执行")
        for group in ("default", "all"):
            Path(f"/proc/sys/net/ipv6/conf/{group}/disable_ipv6").write_text("1\n")
        with tempfile.TemporaryDirectory(prefix="dppd-managed-nd-") as temporary:
            scenario = ManagedNd(build, output, Path(temporary), report, True)
            try:
                verify(scenario)
                assert source_digest(ROOT) == report["source_sha256"], "验收期间源码发生变化"
                report["success"] = True
            finally:
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
