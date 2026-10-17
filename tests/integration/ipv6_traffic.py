#!/usr/bin/env python3
"""在私有三 TAP 中验收 IPv6 解析、ACL、双向路由、持久重启和 IPv4 NAT 共存"""

import argparse
import ctypes
from dataclasses import dataclass, replace
import os
from pathlib import Path
import re
import select
import socket
import struct
import tempfile
import time

from conntrack_nat import Flow, StatefulFixture, crc
from ipv4_routing import check


@dataclass(frozen=True)
class Flow6:
    source: str = "2001:db8:1::1"
    destination: str = "2001:db8:2::7"
    source_port: int = 1500
    destination_port: int = 443
    protocol: int = 17
    hop: int = 64
    vlans: int = 0
    extensions: tuple = ()
    udp_zero: bool = False
    icmp_type: int = 128
    flags: int = 0x18
    sequence: int = 101
    acknowledgment: int = 501
    payload: bytes | None = None

    def reverse(self):
        return replace(self, source=self.destination, destination=self.source,
                       source_port=self.destination_port, destination_port=self.source_port,
                       icmp_type=129 if self.icmp_type == 128 else self.icmp_type)


class IPv6Fixture(StatefulFixture):
    def __init__(self, build, directory, nat):
        super().__init__(build, directory, nat, timeout=60, capacity=32)
        if not nat:
            self.extra_arguments = []
        self.routing_option = "--ip-routing"

    def setup(self):
        self.setup_routes()
        check(len(self.snapshot.read_bytes()) == 11712, "IPv4-only file unexpectedly changed format")
        for port, gateway, mac in ((0, "fe80::10", "02:00:00:00:10:00"),
                                   (1, "fe80::1", "02:00:00:00:20:01"),
                                   (2, "fe80::1", "02:00:00:00:30:02")):
            self.change("neighbor6-set", port, gateway, mac)
        self.change("neighbor6-set", 2, "2001:db8:2::8", "02:00:00:00:30:02")
        for prefix, port, gateway in (("::/0", 2, "fe80::1"), ("2001:db8:1::/64", 0, "fe80::10"),
                                      ("2001:db8:2::/64", 1, "fe80::1"),
                                      ("2001:db8:2:0:8000::/65", 2, "fe80::1"),
                                      ("2001:db8:2::8/128", 2, "direct")):
            self.change("route6-set", prefix, port, gateway)
        check(len(self.snapshot.read_bytes()) == 28096 and self.snapshot.read_bytes()[8] == 2,
              "IPv6 did not upgrade routing format")
        self.cli("acl-set", 1000, 0, 0, 10, "ipv4", "198.18.0.0/16", "any", "any", "any", "drop", "count")
        self.acl6(1101, "udp", "allow", source_ports="1000-2000", destination_ports="443-445")
        self.acl6(1102, "tcp", "allow", destination_ports="443")
        self.acl6(1103, "ip:58", "allow")
        self.cli("acl6-set", 1104, 1, 0, 10, "ipv6", "2001:db8:2::/64", "2001:db8:1::/64",
                 "any", "any", "allow", "count", "state:established" if self.nat else "state:untracked")
        self.acl6(1200, "ipv6", "drop", priority=100, source="any", destination="any")
        self.cli("acl6-set", 1201, 1, 0, 100, "ipv6", "any", "any", "any", "any", "drop", "count")
        check((self.directory / "rules").read_bytes()[8] == 3, "IPv6 ACL did not use complete v3 records")

    def rule_generation(self, identity):
        return int(re.search(r"generation=(\d+)", self.cli("get", identity)).group(1))

    def acl6(self, identity, protocol, action, expected=0, priority=10, source="2001:db8:1::/64",
             destination="2001:db8::/32", source_ports="any", destination_ports="any"):
        return self.cli("acl6-set", identity, 0, expected, priority, protocol, source, destination,
                        source_ports, destination_ports, action, "count", "mark:77")

    def frame(self, flow, tag, ingress, output=None):
        if isinstance(flow, Flow):
            return super().frame(flow, tag, ingress, output)
        payload = b"DPPD-IPV6-CLOSURE" + struct.pack("!I", tag) + b"odd-payload"
        if flow.payload is not None:
            payload = flow.payload
        elif flow.protocol == 6 and flow.flags & 2:
            payload = b""
        if flow.protocol == 17:
            upper = bytearray(struct.pack("!HHHH", flow.source_port, flow.destination_port, len(payload) + 8, 0) + payload)
            checksum_offset = 6
        elif flow.protocol == 6:
            upper = bytearray(struct.pack("!HHIIBBHHH", flow.source_port, flow.destination_port,
                                          flow.sequence, flow.acknowledgment, 0x50, flow.flags, 8192, 0, 0) + payload)
            checksum_offset = 16
        elif flow.protocol == 58:
            upper = bytearray(struct.pack("!BBHHH", flow.icmp_type, 0, 0, 81, tag & 65535) + payload)
            checksum_offset = 2
        else:
            upper, checksum_offset = bytearray(payload), None
        addresses = socket.inet_pton(socket.AF_INET6, flow.source) + socket.inet_pton(socket.AF_INET6, flow.destination)
        if checksum_offset is not None and not (flow.protocol == 17 and flow.udp_zero):
            # 按完整伪首部及实际负载独立求和，包含扩展头时仍使用最终传输协议
            value = crc(addresses + struct.pack("!I3xB", len(upper), flow.protocol) + upper)
            struct.pack_into("!H", upper, checksum_offset, value or (65535 if flow.protocol == 17 else 0))
        body, next_header = bytes(upper), flow.protocol
        for extension in reversed(flow.extensions):
            kind = {"hop": 0, "dest": 60, "option": 0, "route": 43, "fragment": 44}[extension]
            options = bytes.fromhex("800000000000") if extension == "option" else bytes(6)
            body = bytes([next_header, 0]) + options + body
            next_header = kind
        header = struct.pack("!IHBB", (6 << 28) | 0xabcde, len(body), next_header, flow.hop) + addresses
        neighbors = {0: "020000001000", 1: "020000002001", 2: "020000003002"}
        ethernet = self.macs[ingress] + bytes.fromhex("020000aabbcc") if output is None else \
            bytes.fromhex(neighbors[output]) + self.macs[output]
        for index in range(flow.vlans):
            ethernet += struct.pack("!HH", 0x88a8 if flow.vlans == 2 and index == 0 else 0x8100, 37 + index)
        return ethernet + bytes.fromhex("86dd") + header + body, payload

    def exchange(self, flow, ingress=0, output=None, reason=None, corrupt=False, bad_length=False, translated=None, **options):
        if isinstance(flow, Flow):
            return super().exchange(flow, ingress, output, reason=reason, corrupt=corrupt, translated=translated, **options)
        self.sequence += 1
        before = self.stats() if reason else None
        wire, marker = self.frame(flow, self.sequence, ingress)
        if corrupt:
            wire = wire[:-1] + bytes([wire[-1] ^ 1])
        if bad_length:
            offset = 14 + 4 * flow.vlans + 4
            wire = wire[:offset] + b"\xff\xff" + wire[offset + 2:]
        for link in self.links:
            while True:
                try:
                    link.recv(65536)
                except BlockingIOError:
                    break
        self.links[ingress].send(wire)
        found = []
        deadline = time.monotonic() + (.15 if output is None else 2)
        while time.monotonic() < deadline:
            ready, _, _ = select.select(self.links, [], [], max(0, deadline - time.monotonic()))
            for link in ready:
                data, ancillary, _, _ = link.recvmsg(65536, 1024)
                for level, kind, auxiliary in ancillary:
                    if level == 263 and kind == 8 and len(auxiliary) >= 20:
                        status, _, _, _, _, tci, tpid = struct.unpack("=IIIHHHH", auxiliary[:20])
                        if status & 16:
                            data = data[:12] + struct.pack("!HH", tpid if status & 64 else 0x8100, tci) + data[12:]
                l2 = 14 + 4 * flow.vlans
                identity = marker in data if marker else len(data) >= l2 + 40 and data[l2:l2 + 4] == bytes.fromhex("600abcde")
                if identity:
                    found.append((self.links.index(link), data))
            if found:
                break
        if output is None:
            check(not found, f"IPv6 unexpectedly forwarded: {found}")
            if reason:
                check(self.stats()[reason] >= before[reason] + 1, f"missing IPv6 drop reason {reason}")
            return
        wire_flow = flow if translated is None else translated
        if getattr(self, "auto_snat6", False) and ingress == 0 and translated is None:
            check(len(found) == 1, f"missing NAT66 output: {found}")
            offset = 14 + 4 * flow.vlans + 40 + 8 * len(flow.extensions)
            port = struct.unpack_from("!H", found[0][1], offset)[0]
            check(40001 <= port <= 40002 or flow.protocol == 6 and port == 40000, "NAT66 mapping outside available pool")
            wire_flow = replace(flow, source="2001:db8:100::9", source_port=port)
        expected, _ = self.frame(replace(wire_flow, hop=flow.hop - 1), self.sequence, ingress, output)
        check(found == [(output, expected)], f"IPv6 bytes/port/checksum mismatch: {found}, expected={expected.hex()}")
        return wire_flow


def validate(build, directory, nat):
    fixture = IPv6Fixture(build, directory, nat)
    try:
        fixture.create()
        fixture.start()
        fixture.setup()
        ordinary = Flow6()
        for vlans in (0, 1, 2):
            for extensions in ((), ("hop",), ("hop", "dest")):
                for protocol in (6, 17, 58):
                    fixture.exchange(replace(ordinary, vlans=vlans, extensions=extensions, protocol=protocol,
                                             flags=2 if nat and protocol == 6 else 0x18, acknowledgment=0), output=1)
        fixture.exchange(ordinary.reverse(), ingress=1, output=0)
        fixture.exchange(replace(ordinary, destination="2001:db8:3::7"), output=2)
        fixture.exchange(replace(ordinary, destination="2001:db8:2:0:8000::7"), output=2)
        fixture.exchange(replace(ordinary, destination="2001:db8:2::8"), output=2)
        for port, accepted in ((999, False), (1000, True), (2000, True), (2001, False)):
            fixture.exchange(replace(ordinary, source_port=port), output=1 if accepted else None,
                             reason=None if accepted else "rule_drops")
        for port, accepted in ((442, False), (443, True), (445, True), (446, False)):
            fixture.exchange(replace(ordinary, destination_port=port), output=1 if accepted else None,
                             reason=None if accepted else "rule_drops")
        fixture.acl6(1300, "ipv6", "drop", priority=1, source="2001:db8:1:0:8000::/65")
        fixture.exchange(replace(ordinary, source="2001:db8:1:0:8000::1"), reason="rule_drops")
        fixture.exchange(replace(ordinary, source="2001:db8:1:0:7fff::1"), output=1)
        fixture.delete_rule(1300)
        # 同一个命令原子替换两条 IPv6 规则，旧版本重试不能覆盖新表
        batch = directory / "batch"
        original_generations = {identity: fixture.rule_generation(identity) for identity in (1101, 1200)}
        for dropped in (True, False):
            batch.write_text(f"1101 0 {fixture.rule_generation(1101)} 10 udp 2001:db8:1::/64 2001:db8::/32 "
                             f"1000-2000 443-445 {'drop' if dropped else 'allow'} count mark:77\n"
                             f"1200 0 {fixture.rule_generation(1200)} 100 ipv6 any any any any "
                             f"{'allow' if dropped else 'drop'} count\n")
            fixture.cli("acl6-update-batch", batch)
            fixture.exchange(ordinary, output=None if dropped else 1, reason="rule_drops" if dropped else None)
        before = (directory / "rules").read_bytes()
        fixture.cli("acl6-update-batch", batch, success=False)
        check((directory / "rules").read_bytes() == before, "stale IPv6 batch changed policy")
        check(fixture.rule_generation(1101) > original_generations[1101], "IPv6 batch did not advance generation")
        # 移除 ACL 后核对协议、转发错误本身的归因，允许动作不能掩盖这些错误
        for identity in (1101, 1102, 1103, 1104, 1200, 1201):
            fixture.delete_rule(identity)
        fixture.exchange(replace(ordinary, hop=1), reason="ipv6_hop_drops")
        fixture.exchange(ordinary, corrupt=True, reason="ct_checksum_drops" if nat else "ipv6_checksum_drops")
        fixture.exchange(replace(ordinary, udp_zero=True), reason="ct_checksum_drops" if nat else "ipv6_checksum_drops")
        fixture.exchange(ordinary, bad_length=True, reason="rx_malformed")
        for extension in ("route", "fragment", "option"):
            fixture.exchange(replace(ordinary, extensions=(extension,)), reason="ct_unsupported_drops" if nat else "ipv6_unsupported_drops")
        fixture.exchange(replace(ordinary, protocol=58, icmp_type=135), reason="ct_unsupported_drops" if nat else "ipv6_unsupported_drops")
        fixture.exchange(replace(ordinary, destination="ff02::1"), reason="ct_unsupported_drops" if nat else "ipv6_unsupported_drops")
        fixture.exchange(replace(ordinary, source="fe80::1"), reason="ct_unsupported_drops" if nat else "ipv6_unsupported_drops")
        fixture.change("route6-delete", "::/0")
        fixture.exchange(replace(ordinary, destination="2001:db8:3::7"), reason="no_route_drops")
        fixture.change("neighbor6-delete", 2, "fe80::1")
        fixture.exchange(replace(ordinary, destination="2001:db8:2:0:8000::7"), reason="no_neighbor_drops")
        fixture.change("neighbor6-set", 2, "fe80::1", "02:00:00:00:30:02")
        generation, saved = fixture.generation(), fixture.snapshot.read_bytes()
        fixture.cli("route6-set", "2001:db8:2::/64", 1, "fe80::1", generation - 1, success=False)
        fixture.cli("route6-set", "2001:db8:2::/64", 9, "fe80::1", generation, success=False)
        check(fixture.generation() == generation and fixture.snapshot.read_bytes() == saved, "rejected IPv6 route changed state")
        if nat:
            active6 = fixture.status()["active"]
            check(active6 > 0, "IPv6 did not enter dual-stack conntrack")
            sessions = fixture.cli("conntrack-list")
            check("family=ipv6 kind=none" in sessions and "nat=yes" not in sessions, "IPv4 SNAT changed IPv6 sessions")
            original = Flow()
            wire = fixture.exchange(original, output=1)
            fixture.exchange(wire.reverse(), ingress=1, output=0, translated=original.reverse())
            check(fixture.status()["active"] == active6 + 1, "IPv4 SNAT stopped working with IPv6 routes")
        fixture.acl6(1101, "udp", "allow", source_ports="1000-2000", destination_ports="443-445")
        fixture.acl6(1200, "ipv6", "drop", priority=100, source="any", destination="any")
        details = {identity: fixture.cli("get", identity) for identity in (1000, 1101, 1200)}
        rules, routes, shown = (directory / "rules").read_bytes(), fixture.snapshot.read_bytes(), fixture.cli("routing-show")
        fixture.stop(crash=True)
        fixture.start()
        check((directory / "rules").read_bytes() == rules and fixture.snapshot.read_bytes() == routes, "restart rewrote dual-stack snapshots")
        check(fixture.cli("routing-show") == shown, "restart changed IPv6 routes or neighbors")
        for identity, detail in details.items():
            check(fixture.cli("get", identity) == detail, "restart changed mixed ACL content or generation")
        fixture.exchange(ordinary, output=1)
        fixture.exchange(replace(ordinary, destination_port=446), reason="rule_drops")
        fixture.stop()
        fixture.start(faults=True)
        for mode in ("file", "directory"):
            generation, saved = fixture.generation(), fixture.snapshot.read_bytes()
            (directory / "route-store-fault").write_text(mode)
            fixture.cli("route6-set", "2001:db8:2::/64", 2, "fe80::1", generation, success=False)
            check(fixture.generation() == generation and "routing-dirty" in fixture.cli("ready", success=False), "failed IPv6 save published a route")
            fixture.exchange(ordinary, output=1)
            fixture.cli("routing-flush")
            check(fixture.snapshot.read_bytes() == saved, "IPv6 route flush failed to restore active configuration")
        generation = fixture.rule_generation(1101)
        (directory / "acl-store-fault").write_text("file")
        fixture.cli("acl6-set", 1101, 0, generation, 10, "ipv6", "any", "any", "any", "any", "drop", "count", success=False)
        fixture.exchange(ordinary, reason="rule_drops")
        check("persistence-dirty" in fixture.cli("ready", success=False), "failed IPv6 ACL save reported ready")
        fixture.cli("persistence-flush")
        fixture.acl6(1101, "udp", "allow", expected=fixture.rule_generation(1101), source_ports="1000-2000", destination_ports="443-445")
        fixture.exchange(ordinary, output=1)
        fixture.stop()
        valid_routes = fixture.snapshot.read_bytes()
        damaged = bytearray(valid_routes)
        damaged[12000] ^= 1
        fixture.snapshot.write_bytes(damaged)
        fixture.start(refused=True)
        check(fixture.snapshot.read_bytes() == damaged, "damaged dual-stack snapshot overwritten")
        fixture.snapshot.write_bytes(valid_routes)
        valid_rules = (directory / "rules").read_bytes()
        damaged_rules = bytearray(valid_rules)
        damaged_rules[120] ^= 1
        (directory / "rules").write_bytes(damaged_rules)
        fixture.start(refused=True)
        check((directory / "rules").read_bytes() == damaged_rules, "damaged IPv6 ACL snapshot overwritten")
        print(f"PASS IPv6 {'with IPv4 SNAT' if nat else 'standalone'}: TCP/UDP/ICMPv6, extensions, VLAN, LPM, ACL ranges/batch, full bytes, faults, mixed replay", flush=True)
    finally:
        fixture.cleanup()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    arguments = parser.parse_args()
    check(os.geteuid() == 0, "root required for a private network namespace")
    check(ctypes.CDLL(None, use_errno=True).unshare(0x40000000) == 0, "cannot create namespace")
    check({name for _, name in socket.if_nameindex()} == {"lo"}, "namespace was not empty")
    for setting in ("all", "default"):
        Path(f"/proc/sys/net/ipv6/conf/{setting}/disable_ipv6").write_text("1")
    for nat in (False, True):
        with tempfile.TemporaryDirectory(prefix="dppd-ipv6-") as temporary:
            validate(arguments.build_dir.resolve(), Path(temporary), nat)
    check({name for _, name in socket.if_nameindex()} == {"lo"}, "IPv6 test leaked interfaces")
    print("PASS IPv6 private namespace cleanup", flush=True)


if __name__ == "__main__":
    main()
