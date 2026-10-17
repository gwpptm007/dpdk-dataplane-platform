#!/usr/bin/env python3
"""在自有三 TAP 上验收软件 ACL 的允许/拒绝、范围、协议、版本、原子更新和重启"""

import argparse
import ctypes
import os
from pathlib import Path
import re
import socket
import struct
import tempfile

from ipv4_routing import Fixture, check, checksum


class ACLFixture(Fixture):
    def frame(self, destination, ttl=64, bad_checksum=False, vlan=False, source="198.18.0.10",
              source_port=1500, destination_port=8500, protocol=17, fragment=False):
        self.sequence += 1
        payload = b"ACL-PACKET-TEST!" + struct.pack("!I", self.sequence) + bytes(range(60))
        if protocol == 17:
            transport = struct.pack("!HHHH", source_port, destination_port, len(payload) + 8, 0) + payload
        elif protocol == 6:
            header = bytearray(struct.pack("!HHIIBBHHH", source_port, destination_port, 1, 1, 0x50, 0x18, 4096, 0, 0))
            pseudo = socket.inet_aton(source) + socket.inet_aton(destination) + struct.pack("!BBH", 0, 6, len(header) + len(payload))
            struct.pack_into("!H", header, 16, checksum(pseudo + header + payload))
            transport = header + payload
        else:
            transport = payload
        ip = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(transport), 91,
                                   0x2000 if fragment else 0, ttl, protocol, 0,
                                   socket.inet_aton(source), socket.inet_aton(destination)))
        struct.pack_into("!H", ip, 10, checksum(ip) ^ (1 if bad_checksum else 0))
        ethernet = self.macs[0] + bytes.fromhex("020000aabbcc")
        ethernet += bytes.fromhex("810000250800" if vlan else "0800")
        return ethernet + ip + transport, payload

    def rule_generation(self, identity):
        return int(re.search(r"generation=(\d+)", self.cli("get", identity)).group(1))

    def acl(self, identity, priority, protocol, source="any", destination="any", source_ports="any",
            destination_ports="any", action="allow", expected=0):
        return self.cli("acl-set", identity, 0, expected, priority, protocol, source, destination,
                        source_ports, destination_ports, action, "count", "mark:77")

    def hits(self, identity):
        return int(re.search(r"hits=(\d+)", self.cli("count", identity, self.rule_generation(identity))).group(1))


def validate(build, directory):
    fixture = ACLFixture(build, directory)
    neighbor = "02:00:00:00:11:01"
    try:
        fixture.create()
        fixture.start()
        fixture.change("neighbor-set", 1, "192.0.2.1", neighbor)
        fixture.change("route-set", "10.0.0.0/8", 1, "192.0.2.1")
        # 空 ACL 默认继续转发，随后使用末尾 IPv4 拒绝项形成明确的白名单
        fixture.exchange("10.2.0.8", 1, neighbor)
        fixture.acl(101, 10, "udp", "198.18.0.0/24", "10.2.0.0/16", "1000-2000", "8000-9000")
        fixture.acl(102, 100, "ipv4", action="drop")
        for source_port, destination_port in ((1000, 8000), (2000, 9000), (1500, 8500)):
            before = fixture.hits(101)
            fixture.exchange("10.2.0.8", 1, neighbor, source_port=source_port, destination_port=destination_port, vlan=True)
            check(fixture.hits(101) == before + 1, "winning allow COUNT missing")
        for options in ({"source_port": 999}, {"source_port": 2001}, {"destination_port": 7999},
                        {"destination_port": 9001}, {"source": "198.18.1.10"}, {"protocol": 6},
                        {"fragment": True}):
            before, allow_hits = fixture.hits(102), fixture.hits(101)
            fixture.exchange("10.2.0.8", reason="rule_drops", **options)
            check(fixture.hits(102) == before + 1 and fixture.hits(101) == allow_hits, "first-match COUNT semantics")
        fixture.exchange("10.3.0.8", reason="rule_drops")
        # 允许只是继续后续处理，TTL 和校验和错误仍由路由阶段拒绝
        fixture.exchange("10.2.0.8", ttl=1, reason="ipv4_ttl_drops")
        fixture.exchange("10.2.0.8", bad_checksum=True, reason="ipv4_checksum_drops")
        fixture.exchange("10.3.0.8", ttl=1, reason="rule_drops")
        fixture.acl(103, 5, "tcp", destination="10.4.0.0/16", destination_ports="443")
        fixture.exchange("10.4.0.8", 1, neighbor, protocol=6, destination_port=443)
        fixture.exchange("10.4.0.8", protocol=6, destination_port=444, reason="rule_drops")
        fixture.acl(104, 5, "ip:17", destination="10.7.0.0/16")
        fixture.exchange("10.7.0.8", 1, neighbor, fragment=True)
        fixture.exchange("10.7.0.8", protocol=6, reason="rule_drops")
        check("backend=software" in fixture.cli("rule-status", 101), "ACL was offloaded")
        detail = fixture.cli("get", 101)
        check("udp-range src=1000-2000 dst=8000-9000" in detail and "allow" in detail, detail)
        generation = fixture.rule_generation(101)
        saved = (directory / "rules").read_bytes()
        fixture.cli("acl-set", 101, 0, generation + 1, 10, "udp", "any", "any", "any", "any", "drop", success=False)
        fixture.cli("acl-set", 101, 0, generation, 10, "udp", "any", "any", "2000-1000", "any", "drop", success=False)
        check((directory / "rules").read_bytes() == saved, "rejected update changed snapshot")
        batch = directory / "acl.txt"
        batch.write_text(f"101 0 {generation} 10 udp 198.18.0.0/24 10.2.0.0/16 1000-2000 8000-9000 drop count\n"
                         f"102 0 {fixture.rule_generation(102)} 100 ipv4 any any any any allow count\n")
        fixture.cli("acl-update-batch", batch)
        fixture.exchange("10.2.0.8", reason="rule_drops")
        fixture.exchange("10.3.0.8", 1, neighbor)
        fixture.cli("acl-update-batch", batch, success=False)
        details = {identity: fixture.cli("get", identity) for identity in (101, 102, 103, 104)}
        saved, routes = (directory / "rules").read_bytes(), fixture.snapshot.read_bytes()
        fixture.stop(crash=True)
        fixture.start()
        check((directory / "rules").read_bytes() == saved and fixture.snapshot.read_bytes() == routes, "restart rewrote snapshots")
        for identity, expected in details.items():
            check(fixture.cli("get", identity) == expected and fixture.hits(identity) == 0, "restart changed rule or retained volatile COUNT")
        fixture.exchange("10.2.0.8", reason="rule_drops")
        fixture.exchange("10.3.0.8", 1, neighbor)
        fixture.exchange("10.4.0.8", 1, neighbor, protocol=6, destination_port=443)
        fixture.exchange("10.7.0.8", 1, neighbor, fragment=True)
        # 在线修改重放后的精确版本，再删除策略，未命中恢复默认继续转发
        fixture.acl(101, 10, "udp", "198.18.0.0/24", "10.2.0.0/16", "1000-2000", "8000-9000",
                    expected=fixture.rule_generation(101))
        fixture.exchange("10.2.0.8", 1, neighbor)
        fixture.stop()
        fixture.start(faults=True)
        for mode in ("file", "directory"):
            generation, saved = fixture.rule_generation(101), (directory / "rules").read_bytes()
            (directory / "acl-store-fault").write_text(mode)
            fixture.cli("acl-set", 101, 0, generation, 10, "udp", "any", "10.2.0.0/16", "any", "any", "drop", "count", success=False)
            # 继承现有规则持久化语义，保存失败时规则已经生效，不能误报为保留旧 ACL
            check(fixture.rule_generation(101) > generation, "failed save did not report applied generation")
            check(((directory / "rules").read_bytes() == saved) == (mode == "file"), "wrong rule save window")
            fixture.exchange("10.2.0.8", reason="rule_drops")
            check("persistence-dirty" in fixture.cli("ready", success=False), "dirty ACL reported ready")
            (directory / "acl-store-fault").write_text(mode)
            active_generation = fixture.rule_generation(101)
            fixture.cli("acl-set", 101, 0, fixture.rule_generation(101), 10, "ipv4", "any", "any", "any", "any", "allow", success=False)
            check(fixture.rule_generation(101) == active_generation, "failed repair allowed another ACL update")
            fixture.cli("persistence-flush")
            check("dirty=no" in fixture.cli("persistence-status"), "ACL flush failed")
            fixture.acl(101, 10, "udp", "198.18.0.0/24", "10.2.0.0/16", "1000-2000", "8000-9000",
                        expected=fixture.rule_generation(101))
            fixture.exchange("10.2.0.8", 1, neighbor)
            print(f"PASS ACL {mode} fsync failure: applied status, dirty readiness, blocked writes, flush", flush=True)
        saved, before = (directory / "rules").read_bytes(), fixture.cli("get", 101)
        (directory / "acl-store-fault").write_text("file")
        fixture.cli("acl-set", 101, 0, fixture.rule_generation(101), 10, "ipv4", "any", "any", "any", "any", "drop", success=False)
        fixture.stop(crash=True)
        fixture.start()
        check((directory / "rules").read_bytes() == saved and fixture.cli("get", 101) == before, "dirty crash invented an unsaved ACL")
        fixture.exchange("10.2.0.8", 1, neighbor)
        for identity in (101, 102, 103, 104):
            fixture.cli("delete", identity, fixture.rule_generation(identity))
        fixture.exchange("10.3.0.8", 1, neighbor)
        fixture.stop()
        # 损坏规则快照只能拒绝启动，不允许空 ACL 意外放行
        damaged = bytearray((directory / "rules").read_bytes())
        damaged[40] ^= 1
        (directory / "rules").write_bytes(damaged)
        fixture.start(refused=True)
        check((directory / "rules").read_bytes() == damaged, "bad ACL snapshot overwritten")
        print("PASS ACL TAP: allow/drop, range edges, TCP/UDP/protocol/fragments, VLAN/full bytes, COUNT, stale versions, batch update, crash replay, deletion, bad snapshot", flush=True)
    finally:
        fixture.cleanup()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    args = parser.parse_args()
    check(os.geteuid() == 0, "root required for an empty private network namespace")
    build = args.build_dir.resolve()
    check(ctypes.CDLL(None, use_errno=True).unshare(0x40000000) == 0, "cannot create private namespace")
    check({name for _, name in socket.if_nameindex()} == {"lo"}, "namespace was not empty")
    for setting in ("all", "default"):
        Path(f"/proc/sys/net/ipv6/conf/{setting}/disable_ipv6").write_text("1")
    with tempfile.TemporaryDirectory(prefix="dppd-acl-") as temporary:
        validate(build, Path(temporary))
    check({name for _, name in socket.if_nameindex()} == {"lo"}, "test leaked a TAP")
    print("PASS ACL private namespace cleanup", flush=True)


if __name__ == "__main__":
    main()
