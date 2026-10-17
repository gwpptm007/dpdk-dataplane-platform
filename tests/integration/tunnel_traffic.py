#!/usr/bin/env python3
"""在私有三 TAP 中验收隧道观察、双栈外层策略、NAT 与原样保留的内层字节"""

import argparse
import ctypes
from dataclasses import dataclass, replace
import os
from pathlib import Path
import select
import socket
import struct
import tempfile
import time

from conntrack_nat import crc
from ipv4_routing import check
from ipv6_traffic import Flow6, IPv6Fixture


@dataclass(frozen=True)
class TunnelFlow(Flow6):
    ipv4: bool = False


def encapsulation(kind, inner6, vlan, variant="normal"):
    marker = b"DPPD-TUNNEL-ACCEPTANCE-INNER"
    # 内层使用独立私网地址，与外层路由和 NAT 地址无关
    source, destination = ("2001:db8:aa::1", "2001:db8:bb::2") if inner6 else ("172.20.1.1", "172.20.2.2")
    addresses = socket.inet_pton(socket.AF_INET6 if inner6 else socket.AF_INET, source)
    addresses += socket.inet_pton(socket.AF_INET6 if inner6 else socket.AF_INET, destination)
    udp = bytearray(struct.pack("!HHHH", 1111, 2222, 8 + len(marker), 0) + marker)
    pseudo = addresses + (struct.pack("!I3xB", len(udp), 17) if inner6 else struct.pack("!BBH", 0, 17, len(udp)))
    struct.pack_into("!H", udp, 6, crc(pseudo + udp) or 65535)
    if inner6:
        network = struct.pack("!IHBB", 6 << 28, len(udp), 17, 43) + addresses + udp
    else:
        network = bytearray(struct.pack("!BBHHHBBH", 0x45, 0, 20 + len(udp), 5, 0, 43, 17, 0) + addresses)
        struct.pack_into("!H", network, 10, crc(network))
        network += udp
    inner = bytes.fromhex("020000000002020000000001")
    for index in range(vlan):
        inner += struct.pack("!HH", 0x88a8 if vlan == 2 and index == 0 else 0x8100, 100 + index)
    inner += struct.pack("!H", 0x86dd if inner6 else 0x0800) + network
    if kind == "vxlan":
        header = bytes.fromhex("0800000001020300")
    elif kind == "geneve":
        header = bytes.fromhex("0100655801020300") + bytes.fromhex("12340100")
        if variant == "critical":
            header = header[:1] + b"\x40" + header[2:10] + b"\x81" + header[11:]
        elif variant == "bad-option":
            header = header[:-1] + b"\x1f"
    else:
        header = bytes.fromhex("b000655800000000abcdef01abcdef02")
        # GRE checksum 覆盖整个 GRE 包，独立重算供后续完整性验证
        header = header[:4] + struct.pack("!H", crc(header + inner)) + header[6:]
    if variant == "bad-inner":
        offset = 14 + 4 * vlan
        inner = inner[:offset] + b"\x40" + inner[offset + 1:]
    return header + inner


class Fixture(IPv6Fixture):
    def __init__(self, build, directory, nat):
        super().__init__(build, directory, False)
        self.translate = nat
        if nat:
            self.extra_arguments = ["--conntrack", "64", "--snat", "0,1,203.0.113.9,40000-40031",
                                    "--snat6", "0,1,2001:db8:100::9,40000-40031"]

    def setup(self):
        self.setup_routes()
        for port, gateway, mac in ((0, "fe80::10", "02:00:00:00:10:00"),
                                   (1, "fe80::1", "02:00:00:00:20:01")):
            self.change("neighbor6-set", port, gateway, mac)
        self.change("route6-set", "2001:db8:1::/64", 0, "fe80::10")
        self.change("route6-set", "2001:db8:2::/64", 1, "fe80::1")
        # 若错误地将内层字段替换外层，这两条规则会让下面的正常隧道全部丢失
        self.cli("acl-set", 100, 0, 0, 1, "ipv4", "172.20.0.0/16", "any", "any", "any", "drop", "count")
        self.cli("acl6-set", 101, 0, 0, 1, "ipv6", "2001:db8:aa::/48", "any", "any", "any", "drop", "count")

    def frame(self, flow, tag, ingress, output=None):
        if not flow.ipv4:
            return super().frame(flow, tag, ingress, output)
        payload = flow.payload
        addresses = socket.inet_aton(flow.source) + socket.inet_aton(flow.destination)
        upper = bytearray(payload)
        if flow.protocol == 17:
            upper = bytearray(struct.pack("!HHHH", flow.source_port, flow.destination_port, len(payload) + 8, 0) + payload)
            struct.pack_into("!H", upper, 6, crc(addresses + struct.pack("!BBH", 0, 17, len(upper)) + upper) or 65535)
        ip = bytearray(struct.pack("!BBHHHBBH", 0x45, 0, 20 + len(upper), tag, 0, flow.hop, flow.protocol, 0) + addresses)
        struct.pack_into("!H", ip, 10, crc(ip))
        neighbors = {0: "020000001000", 1: "020000002001"}
        ethernet = self.macs[ingress] + bytes.fromhex("020000aabbcc") if output is None else \
            bytes.fromhex(neighbors[output]) + self.macs[output]
        for index in range(flow.vlans):
            ethernet += struct.pack("!HH", 0x88a8 if flow.vlans == 2 and index == 0 else 0x8100, 37 + index)
        return ethernet + bytes.fromhex("0800") + ip + upper, payload

    def exchange_tunnel(self, flow, kind, status="ok", ingress=0, output=1, restored=None):
        self.sequence += 1
        before = self.stats()
        wire, marker = self.frame(flow, self.sequence, ingress)
        for link in self.links:
            while True:
                try:
                    link.recv(65536)
                except BlockingIOError:
                    break
        self.links[ingress].send(wire)
        found = []
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline and not found:
            ready, _, _ = select.select(self.links, [], [], max(0, deadline - time.monotonic()))
            for link in ready:
                data, ancillary, _, _ = link.recvmsg(65536, 1024)
                for level, option, auxiliary in ancillary:
                    if level == 263 and option == 8 and len(auxiliary) >= 20:
                        flags, _, _, _, _, tci, tpid = struct.unpack("=IIIHHHH", auxiliary[:20])
                        if flags & 16:
                            data = data[:12] + struct.pack("!HH", tpid if flags & 64 else 0x8100, tci) + data[12:]
                if marker in data:
                    found.append((self.links.index(link), data))
        check(len(found) == 1 and found[0][0] == output, f"wrong tunnel output: {found}")
        translated = flow if restored is None else restored
        if self.translate and ingress == 0:
            offset = 14 + 4 * flow.vlans + (20 if flow.ipv4 else 40)
            port = struct.unpack_from("!H", found[0][1], offset)[0]
            check(40000 <= port <= 40031, "outer NAT port outside pool")
            translated = replace(flow, source="203.0.113.9" if flow.ipv4 else "2001:db8:100::9", source_port=port)
        expected, _ = self.frame(replace(translated, hop=flow.hop - 1), self.sequence, ingress, output)
        check(found[0][1] == expected, "tunnel outer checksum or preserved inner bytes differ")
        deadline = time.monotonic() + 1
        counter = f"tunnel_{kind}_packets" if kind is not None else "rx_packets"
        while time.monotonic() < deadline:
            after = self.stats()
            if after[counter] >= before[counter] + 1:
                break
            time.sleep(.01)
        check(after[counter] == before[counter] + 1, f"missing {counter}")
        for name in ("malformed", "unsupported"):
            key = f"tunnel_{name}"
            check(after[key] == before[key] + (status == name), f"wrong {key}: {before} -> {after}")
        return translated


def validate(build, directory, nat):
    fixture = Fixture(build, directory, nat)
    try:
        fixture.create()
        fixture.start()
        fixture.setup()
        for ipv4 in (False, True):
            for inner6 in (False, True):
                for vlan in (0, 1, 2):
                    for kind in (("vxlan", "geneve") if nat else ("vxlan", "geneve", "gre")):
                        payload = encapsulation(kind, inner6, vlan)
                        flow = TunnelFlow(ipv4=ipv4, vlans=vlan, payload=payload, protocol=47 if kind == "gre" else 17,
                                          destination_port=4789 if kind == "vxlan" else 6081,
                                          source="10.10.0.2" if ipv4 else "2001:db8:1::1",
                                          destination="198.51.100.7" if ipv4 else "2001:db8:2::7")
                        outgoing = fixture.exchange_tunnel(flow, kind)
                        reverse = replace(outgoing, source=outgoing.destination, destination=outgoing.source,
                                          source_port=outgoing.destination_port, destination_port=outgoing.source_port)
                        original_reverse = replace(flow, source=flow.destination, destination=flow.source,
                                                   source_port=flow.destination_port, destination_port=flow.source_port)
                        # 返回方向目的 UDP 端口已不是标准隧道端口，仍须核对 NAT 还原与完整内层
                        fixture.exchange_tunnel(reverse, "gre" if kind == "gre" else None,
                                                ingress=1, output=0, restored=original_reverse)
        for variant, status in (("critical", "unsupported"), ("bad-option", "malformed"), ("bad-inner", "malformed")):
            flow = TunnelFlow(payload=encapsulation("geneve", False, 0, variant), destination_port=6081)
            fixture.exchange_tunnel(flow, "geneve", status)
        print(f"PASS tunnel nat={nat}: dual-stack outer/inner, VLAN 0/1/2, transit bytes, counters, critical/invalid", flush=True)
    finally:
        fixture.cleanup()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    args = parser.parse_args()
    check(os.geteuid() == 0, "root required for private network namespace")
    check(ctypes.CDLL(None, use_errno=True).unshare(0x40000000) == 0, "cannot create private namespace")
    check({name for _, name in socket.if_nameindex()} == {"lo"}, "namespace was not empty")
    for setting in ("all", "default"):
        Path(f"/proc/sys/net/ipv6/conf/{setting}/disable_ipv6").write_text("1")
    for nat in (False, True):
        with tempfile.TemporaryDirectory(prefix="dppd-tunnel-") as temporary:
            validate(args.build_dir.resolve(), Path(temporary), nat)
    check({name for _, name in socket.if_nameindex()} == {"lo"}, "tunnel test leaked interfaces")
    print("PASS tunnel private namespace cleanup", flush=True)


if __name__ == "__main__":
    main()
