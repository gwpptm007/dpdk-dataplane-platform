#!/usr/bin/env python3
"""在自建三 TAP 中验收连接跟踪、状态 ACL、SNAT 回包、资源耗尽、过期与重启"""

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

from ipv4_routing import Fixture, check


def crc(data):
    if len(data) & 1:
        data += b"\0"
    value = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while value >> 16:
        value = (value & 65535) + (value >> 16)
    return (~value) & 65535


@dataclass(frozen=True)
class Flow:
    source: str = "10.10.0.2"
    destination: str = "198.51.100.7"
    source_port: int = 12345
    destination_port: int = 443
    protocol: int = 17
    flags: int = 16
    sequence: int = 101
    acknowledgment: int = 501
    payload: bytes = b"odd-payload"
    ttl: int = 64
    vlan: bool = False
    udp_zero: bool = False
    fragment: bool = False

    def reverse(self):
        return replace(self, source=self.destination, destination=self.source,
                       source_port=self.destination_port, destination_port=self.source_port)


class StatefulFixture(Fixture):
    def __init__(self, build, directory, nat, timeout=10, capacity=8):
        super().__init__(build, directory)
        self.nat = nat
        self.extra_arguments = ["--conntrack", str(capacity), "--ct-timeout", str(timeout)]
        if nat:
            self.extra_arguments += ["--snat", "0,1,203.0.113.9,40000-40001"]

    def setup_routes(self):
        self.change("neighbor-set", 0, "192.0.2.10", "02:00:00:00:10:00")
        self.change("neighbor-set", 1, "192.0.2.20", "02:00:00:00:20:01")
        self.change("route-set", "10.10.0.0/24", 0, "192.0.2.10")
        self.change("route-set", "198.51.100.0/24", 1, "192.0.2.20")

    def status(self):
        return {key: int(value) for key, value in re.findall(r"([a-z-]+)=(\d+)", self.cli("conntrack-status"))}

    def flush(self):
        self.cli("conntrack-flush", self.status()["revision"])
        check(self.status()["active"] == 0, "flush left active sessions")

    def acl(self, identity, port, action, state=None, priority=10):
        arguments = ["acl-set", identity, port, 0, priority, "ipv4", "any", "any", "any", "any", action, "count"]
        if state:
            arguments.append("state:" + state)
        self.cli(*arguments)

    def delete_rule(self, identity):
        generation = int(re.search(r"generation=(\d+)", self.cli("get", identity)).group(1))
        self.cli("delete", identity, generation)

    def frame(self, flow, tag, ingress, output=None):
        # 所有预期输出均重新按完整伪首部和负载计算，不复用生产增量改写公式
        payload = b"DPPD-CT-NAT" + struct.pack("!I", tag) + flow.payload
        if flow.protocol == 6 and flow.flags & 2:
            payload = b""  # 首版握手不支持携带数据的 SYN
        if flow.protocol == 17:
            transport = bytearray(struct.pack("!HHHH", flow.source_port, flow.destination_port, len(payload) + 8, 0) + payload)
            offset = 6
        else:
            transport = bytearray(struct.pack("!HHIIBBHHH", flow.source_port, flow.destination_port,
                                              flow.sequence, flow.acknowledgment, 0x50, flow.flags, 8192, 0, 0) + payload)
            offset = 16
        addresses = socket.inet_aton(flow.source) + socket.inet_aton(flow.destination)
        if flow.protocol == 6 or not flow.udp_zero:
            value = crc(addresses + struct.pack("!BBH", 0, flow.protocol, len(transport)) + transport)
            struct.pack_into("!H", transport, offset, value or (65535 if flow.protocol == 17 else 0))
        ip = bytearray(struct.pack("!BBHHHBBH", 0x45, 0, 20 + len(transport), 81,
                                   0x2000 if flow.fragment else 0, flow.ttl, flow.protocol, 0) + addresses)
        struct.pack_into("!H", ip, 10, crc(ip))
        if output is None:
            ethernet = self.macs[ingress] + bytes.fromhex("020000aabbcc")
        else:
            ethernet = bytes.fromhex("020000001000" if output == 0 else "020000002001") + self.macs[output]
        ethernet += bytes.fromhex("810000250800" if flow.vlan else "0800")
        return ethernet + ip + transport, payload

    def exchange(self, flow, ingress=0, output=None, translated=None, reason=None, corrupt=False):
        self.sequence += 1
        before = self.stats() if reason else None
        frame, marker = self.frame(flow, self.sequence, ingress)
        if corrupt:
            frame = frame[:-1] + bytes([frame[-1] ^ 1])
        for link in self.links:
            while True:
                try:
                    link.recv(65536)
                except BlockingIOError:
                    break
        self.links[ingress].send(frame)
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
                # SYN 没有负载，使用 IP ID、序号和端口识别，其他包使用唯一标签
                l2 = 18 if flow.vlan else 14
                identity = marker in data if marker else len(data) >= l2 + 40 and data[l2 + 4:l2 + 6] == b"\0Q"
                if identity:
                    found.append((self.links.index(link), data))
            if found:
                break
        if output is None:
            check(not found, f"unexpected forwarding: {found}")
            if reason:
                check(self.stats()[reason] >= before[reason] + 1, f"missing drop reason {reason}")
            return None
        check(len(found) == 1 and found[0][0] == output, f"wrong port or count: {found}")
        wire = flow if translated is None else translated
        if self.nat and ingress == 0 and translated is None:
            l2 = 18 if flow.vlan else 14
            port = struct.unpack_from("!H", found[0][1], l2 + 20)[0]
            check(40000 <= port <= 40001, "mapping outside pool")
            wire = replace(flow, source="203.0.113.9", source_port=port)
        expected, _ = self.frame(replace(wire, ttl=flow.ttl - 1), self.sequence, ingress, output)
        check(found[0][1] == expected, f"full bytes/checksum mismatch expected={expected.hex()} actual={found[0][1].hex()}")
        return wire


def validate(build, directory, nat):
    fixture = StatefulFixture(build, directory, nat)
    try:
        fixture.create()
        fixture.start()
        fixture.setup_routes()
        fixture.acl(1100, 0, "allow", "new,established")
        fixture.acl(1101, 1, "allow", "established")
        fixture.acl(1200, 0, "drop", priority=100)
        fixture.acl(1201, 1, "drop", priority=100)
        original = Flow(vlan=True)
        unsolicited = original.reverse()
        if nat:
            unsolicited = replace(unsolicited, destination="203.0.113.9", destination_port=40000)
        fixture.exchange(unsolicited, ingress=1, reason="rule_drops")
        check(fixture.status()["active"] == 0, "denied reply created state")
        wire = fixture.exchange(original, output=1)
        fixture.exchange(wire.reverse(), ingress=1, output=0, translated=original.reverse())
        check(fixture.status()["active"] == 1 and "state=established" in fixture.cli("conntrack-list"), "UDP did not establish")
        before = fixture.cli("conntrack-list")
        fixture.acl(1300, 0, "drop", "established", priority=1)
        fixture.exchange(original, reason="rule_drops")
        check(fixture.cli("conntrack-list") == before, "ACL denial refreshed or counted a session")
        fixture.delete_rule(1300)
        repeated = fixture.exchange(original, output=1)
        check(repeated.source_port == wire.source_port, "existing flow changed mapping")
        fixture.exchange(replace(original, destination="198.51.101.7", source_port=12346), reason="no_route_drops")
        check(fixture.status()["active"] == 1, "route failure created state")
        syn = Flow(protocol=6, flags=2, sequence=100, acknowledgment=0, payload=b"")
        syn_wire = fixture.exchange(syn, output=1)
        syn_ack = replace(syn_wire.reverse(), flags=18, sequence=500, acknowledgment=101)
        fixture.exchange(syn_ack, ingress=1, output=0, translated=replace(syn.reverse(), flags=18, sequence=500, acknowledgment=101))
        ack = replace(syn, flags=16, sequence=101, acknowledgment=501, payload=b"tcp-data")
        fixture.exchange(replace(ack, acknowledgment=999), reason="rule_drops")
        check("tcp=syn-ack" in fixture.cli("conntrack-list"), "bad ACK advanced TCP")
        fixture.exchange(ack, output=1, translated=replace(ack, source=syn_wire.source, source_port=syn_wire.source_port))
        check("tcp=open" in fixture.cli("conntrack-list"), "TCP handshake not open")
        fixture.exchange(replace(ack, flags=17, payload=b""), output=1,
                         translated=replace(ack, flags=17, payload=b"", source=syn_wire.source, source_port=syn_wire.source_port))
        fixture.exchange(replace(syn_ack, flags=20, payload=b""), ingress=1, output=0,
                         translated=replace(syn.reverse(), flags=20, sequence=500, acknowledgment=101, payload=b""))
        check("tcp=closed" in fixture.cli("conntrack-list"), "RST did not close")
        for identity in (1100, 1101, 1200, 1201):
            fixture.delete_rule(identity)
        fixture.flush()
        if nat:
            fixture.exchange(wire.reverse(), ingress=1, reason="nat_miss_drops")
        fixture.exchange(replace(original, source_port=30000), corrupt=True, reason="ct_checksum_drops")
        fixture.exchange(replace(original, source_port=30000, ttl=1), reason="ipv4_ttl_drops")
        fixture.exchange(replace(original, source_port=30000, fragment=True), reason="ct_unsupported_drops")
        check(fixture.status()["active"] == 0, "bad packet created a session")
        limit = 2 if nat else 8
        for i in range(limit):
            fixture.exchange(replace(original, source_port=31000 + i, udp_zero=i == 0), output=1)
        fixture.exchange(replace(original, source_port=32000), reason="nat_pool_drops" if nat else "ct_full_drops")
        check(fixture.status()["active"] == limit, "capacity or pool exhaustion changed existing sessions")
        if not nat:
            first_page = fixture.cli("conntrack-list")
            next_id = int(re.search(r"next-id=(\d+)", first_page).group(1))
            page_revision = int(re.search(r"revision=(\d+)", first_page).group(1))
            check("count=4 " in first_page and "more=yes" in first_page, "first page boundary")
            check("count=4 " in fixture.cli("conntrack-list", next_id, page_revision), "second page boundary")
        revision = fixture.status()["revision"]
        fixture.cli("conntrack-flush", revision - 1, success=False)
        fixture.flush()
        # 状态规则与路由可以重放，但运行时映射不保存，旧外网回包必须拒绝
        fixture.acl(1100, 0, "allow", "new,established")
        fixture.acl(1101, 1, "allow", "established")
        fixture.acl(1201, 1, "drop", priority=100)
        old_wire = fixture.exchange(original, output=1)
        saved = (directory / "rules").read_bytes()
        fixture.stop(crash=True)
        fixture.start()
        check(fixture.status()["active"] == 0 and (directory / "rules").read_bytes() == saved, "restart retained a session or changed policy")
        fixture.exchange(old_wire.reverse(), ingress=1, reason="rule_drops")
        fixture.exchange(original, output=1)
        check(fixture.status()["active"] == 1, "fresh connection after restart failed")
        fixture.stop()
        arguments = fixture.extra_arguments
        fixture.extra_arguments = []
        try:
            fixture.start(refused=True)
            check((directory / "rules").read_bytes() == saved, "disabled tracking overwrote state ACL")
        finally:
            fixture.extra_arguments = arguments
        print(f"PASS {'SNAT' if nat else 'conntrack'} TAP: state ACL, UDP/TCP handshake, full bytes/checksums, faults, limits, flush, crash restart", flush=True)
    finally:
        fixture.cleanup()


def aging(build, directory, nat):
    fixture = StatefulFixture(build, directory, nat, timeout=1, capacity=1)
    if nat:
        fixture.extra_arguments[-1] = "0,1,203.0.113.9,40000-40000"
    try:
        fixture.create()
        fixture.start()
        fixture.setup_routes()
        previous = fixture.exchange(Flow(), output=1)
        deadline = time.monotonic() + 3
        while fixture.status()["active"] != 0:
            check(time.monotonic() < deadline, "idle session did not expire")
            time.sleep(.05)
        reused = fixture.exchange(Flow(source_port=12346), output=1)
        if nat:
            check(previous.source_port == reused.source_port, "single-port pool was not released")
        check(fixture.status()["active"] == 1 and fixture.status()["expired"] == 1, "capacity/port not reusable after aging")
        fixture.stop()
        print(f"PASS {'SNAT' if nat else 'conntrack'} idle GC and resource reuse", flush=True)
    finally:
        fixture.cleanup()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    arguments = parser.parse_args()
    check(os.geteuid() == 0, "root required for an empty private namespace")
    build = arguments.build_dir.resolve()
    check(ctypes.CDLL(None, use_errno=True).unshare(0x40000000) == 0, "cannot create private network namespace")
    check({name for _, name in socket.if_nameindex()} == {"lo"}, "namespace was not empty")
    for setting in ("all", "default"):
        Path(f"/proc/sys/net/ipv6/conf/{setting}/disable_ipv6").write_text("1")
    for nat in (False, True):
        with tempfile.TemporaryDirectory(prefix="dppd-stateful-") as temporary:
            validate(build, Path(temporary), nat)
        with tempfile.TemporaryDirectory(prefix="dppd-aging-") as temporary:
            aging(build, Path(temporary), nat)
    check({name for _, name in socket.if_nameindex()} == {"lo"}, "test leaked interfaces")
    print("PASS stateful namespace cleanup", flush=True)


if __name__ == "__main__":
    main()
