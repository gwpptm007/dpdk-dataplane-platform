#!/usr/bin/env python3
"""在自建 TAP 中验收静态 DNAT、端口转发、SNAT 共存、状态 ACL 与重启"""

import argparse
import ctypes
from dataclasses import replace
import os
from pathlib import Path
import socket
import tempfile
import time

from conntrack_nat import Flow, StatefulFixture
from ipv4_routing import check


class DnatFixture(StatefulFixture):
    def __init__(self, build, directory, combined, timeout=20, capacity=8):
        super().__init__(build, directory, False, timeout, capacity)
        self.combined = combined
        self.policies = [
            "1,0,tcp,203.0.113.9,443,10.10.0.9,8443",
            "1,0,udp,203.0.113.9,40000-40001,10.10.0.9,8000-8001",
            "1,0,udp,203.0.113.10,any,10.10.0.10,same",
            "1,0,tcp,203.0.113.11,any,10.10.0.11,same",
            "1,0,udp,203.0.113.12,5353,10.10.0.12,53",
            "1,0,tcp,203.0.113.13,2222-2223,10.10.0.13,22-23",
        ]
        if combined:
            self.extra_arguments += ["--snat", "0,1,203.0.113.9,40000-40002"]
        for policy in self.policies:
            self.extra_arguments += ["--dnat", policy]

    def policy(self):
        # 先验证四条一页的边界，再核对第二页和超过末尾的空页
        first, second = self.cli("dnat-list"), self.cli("dnat-list", 4)
        check("count=4 " in first and "next-id=4 more=yes" in first, "first DNAT page")
        check("count=2 " in second and "next-id=6 more=no" in second, "second DNAT page")
        check("count=0 " in self.cli("dnat-list", 999), "DNAT end cursor")
        check("private=10.10.0.9:8000-8001" in first and "public=203.0.113.10:0-65535" in first,
              "DNAT query lost port range or address mapping")
        check("dnat-rules=6" in self.cli("nat-status"), "DNAT count missing")
        return first + second

    def install_acl(self):
        # 外口按转换后的后端地址与端口放行，公网五元组不应直接命中这些规则
        for identity, protocol, address, ports in (
            (1101, "udp", "10.10.0.9/32", "8000-8001"),
            (1102, "tcp", "10.10.0.9/32", "8443"),
            (1103, "ipv4", "10.10.0.0/24", "any"),
        ):
            self.cli("acl-set", identity, 1, 0, 10 if identity != 1103 else 20, protocol,
                     "198.51.100.0/24", address, "any", ports, "allow", "count", "state:new,established")
        self.cli("acl-set", 1104, 0, 0, 10, "ipv4", "10.10.0.0/24", "198.51.100.0/24",
                 "any", "any", "allow", "count", "state:established")
        if self.combined:
            self.cli("acl-set", 1105, 0, 0, 5, "ipv4", "10.10.0.2/32", "any", "any", "any",
                     "allow", "count", "state:new,established")
        self.acl(1200, 0, "drop", priority=100)
        self.acl(1201, 1, "drop", priority=100)

    def remove_acl(self):
        identities = [1101, 1102, 1103, 1104, 1200, 1201] + ([1105] if self.combined else [])
        for identity in identities:
            self.delete_rule(identity)

    def roundtrip(self, original, backend):
        self.exchange(original, ingress=1, output=0, translated=backend)
        self.exchange(backend.reverse(), output=1, translated=original.reverse())

    def tcp(self, public="203.0.113.9", private="10.10.0.9", port=443, target=8443):
        syn = Flow(source="198.51.100.7", destination=public, destination_port=port,
                   protocol=6, flags=2, sequence=100, acknowledgment=0, vlan=True)
        backend = replace(syn, destination=private, destination_port=target)
        self.exchange(syn, ingress=1, output=0, translated=backend)
        syn_ack = replace(backend.reverse(), flags=18, sequence=500, acknowledgment=101)
        self.exchange(syn_ack, output=1,
                      translated=replace(syn.reverse(), flags=18, sequence=500, acknowledgment=101))
        ack = replace(syn, flags=16, sequence=101, acknowledgment=501)
        self.exchange(replace(ack, acknowledgment=999), ingress=1, reason="rule_drops")
        self.exchange(ack, ingress=1, output=0, translated=replace(backend, flags=16, sequence=101, acknowledgment=501))
        check("tcp=open" in self.cli("conntrack-list"), "DNAT TCP handshake incomplete")
        return ack


def validate(build, directory, combined):
    fixture = DnatFixture(build, directory, combined)
    original = Flow(source="198.51.100.7", destination="203.0.113.9", destination_port=40000, vlan=True)
    backend = replace(original, destination="10.10.0.9", destination_port=8000)
    try:
        fixture.create()
        fixture.start()
        fixture.setup_routes()
        policy = fixture.policy()
        fixture.install_acl()
        # ACL 拒绝既不能建立新会话，也不能刷新既有会话
        fixture.acl(1300, 1, "drop", "new", priority=1)
        fixture.exchange(original, ingress=1, reason="rule_drops")
        check(fixture.status()["active"] == 0, "denied DNAT created a session")
        fixture.delete_rule(1300)
        fixture.roundtrip(original, backend)
        before = fixture.cli("conntrack-list")
        fixture.acl(1300, 1, "drop", "established", priority=1)
        fixture.exchange(original, ingress=1, reason="rule_drops")
        check(fixture.cli("conntrack-list") == before, "denied DNAT refreshed a session")
        fixture.delete_rule(1300)
        check("kind=dnat dnat-rule=2" in before, "session lost static rule identity")
        fixture.roundtrip(replace(original, destination_port=40001, udp_zero=True),
                          replace(backend, destination_port=8001, udp_zero=True))
        for port in (1, 65535):
            whole = replace(original, destination="203.0.113.10", destination_port=port)
            fixture.roundtrip(whole, replace(whole, destination="10.10.0.10"))
        single = replace(original, destination="203.0.113.12", destination_port=5353)
        fixture.roundtrip(single, replace(single, destination="10.10.0.12", destination_port=53))
        fixture.flush()
        fixture.tcp()
        fixture.flush()
        fixture.tcp("203.0.113.11", "10.10.0.11", 65535, 65535)
        fixture.flush()
        for offset in (0, 1):
            fixture.tcp("203.0.113.13", "10.10.0.13", 2222 + offset, 22 + offset)
        fixture.flush()
        fixture.remove_acl()
        # 无 ACL 时也必须拒绝无映射、错误方向、畸形报文和非法出口
        fixture.exchange(replace(original, destination_port=39999), ingress=1, reason="nat_miss_drops")
        fixture.exchange(original, reason="ct_zone_drops")
        fixture.exchange(original, ingress=2, reason="ct_zone_drops")
        fixture.exchange(original, ingress=1, corrupt=True, reason="ct_checksum_drops")
        fixture.exchange(replace(original, ttl=1), ingress=1, reason="ipv4_ttl_drops")
        fixture.exchange(replace(original, fragment=True), ingress=1, reason="ct_unsupported_drops")
        fixture.exchange(backend.reverse(), reason="nat_miss_drops")
        fixture.change("route-delete", "10.10.0.0/24")
        fixture.exchange(original, ingress=1, reason="no_route_drops")
        fixture.change("route-set", "10.10.0.0/24", 2, "192.0.2.20")
        fixture.change("neighbor-set", 2, "192.0.2.20", "02:00:00:00:20:01")
        fixture.exchange(original, ingress=1, reason="ct_zone_drops")
        fixture.change("route-set", "10.10.0.0/24", 0, "192.0.2.10")
        fixture.change("neighbor-delete", 0, "192.0.2.10")
        fixture.exchange(original, ingress=1, reason="no_neighbor_drops")
        fixture.change("neighbor-set", 0, "192.0.2.10", "02:00:00:00:10:00")
        check(fixture.status()["active"] == 0, "failed DNAT published state")
        for i in range(8):
            incoming = replace(original, source_port=21000 + i)
            fixture.roundtrip(incoming, replace(backend, source_port=21000 + i))
        fixture.exchange(replace(original, source_port=22000), ingress=1, reason="ct_full_drops")
        check(fixture.status()["active"] == 8, "DNAT capacity limit failed")
        fixture.flush()
        if combined:
            # 两个静态 UDP 端口被保留，动态出站只能取得第三个，TCP 不受 UDP 保留影响
            outbound = Flow()
            wire = replace(outbound, source="203.0.113.9", source_port=40002)
            fixture.roundtrip(original, backend)
            fixture.exchange(outbound, output=1, translated=wire)
            fixture.exchange(wire.reverse(), ingress=1, output=0, translated=outbound.reverse())
            fixture.exchange(replace(outbound, source_port=12346), reason="nat_pool_drops")
            fixture.flush()
            fixture.exchange(outbound, output=1, translated=wire)
            fixture.flush()
        fixture.install_acl()
        fixture.roundtrip(original, backend)
        old_ack = fixture.tcp()
        saved_rules = (directory / "rules").read_bytes()
        saved_routes = fixture.snapshot.read_bytes()
        fixture.stop(crash=True)
        fixture.start()
        check(fixture.status()["active"] == 0 and fixture.policy() == policy, "restart changed DNAT policy or retained sessions")
        check((directory / "rules").read_bytes() == saved_rules and fixture.snapshot.read_bytes() == saved_routes,
              "restart changed persistent ACL or routes")
        fixture.exchange(backend.reverse(), reason="rule_drops")
        fixture.exchange(old_ack, ingress=1, reason="rule_drops")
        fixture.roundtrip(original, backend)
        fixture.stop()
        # 冲突参数必须拒绝启动，不能打开管理入口或覆盖已有策略快照
        fixture.extra_arguments += ["--dnat", "1,0,udp,203.0.113.9,40000,10.10.0.99,9000"]
        fixture.start(refused=True)
        check((directory / "rules").read_bytes() == saved_rules and fixture.snapshot.read_bytes() == saved_routes,
              "invalid DNAT configuration changed snapshots")
        print(f"PASS DNAT {'with SNAT' if combined else 'standalone'}: address/port/range, TCP/UDP, full bytes/checksums, ACL, faults, capacity, reservations, restart", flush=True)
    finally:
        fixture.cleanup()


def aging(build, directory):
    fixture = DnatFixture(build, directory, True, timeout=1, capacity=1)
    original = Flow(source="198.51.100.7", destination="203.0.113.9", destination_port=40000)
    backend = replace(original, destination="10.10.0.9", destination_port=8000)
    try:
        fixture.create()
        fixture.start()
        fixture.setup_routes()
        fixture.roundtrip(original, backend)
        deadline = time.monotonic() + 3
        while fixture.status()["active"] != 0:
            check(time.monotonic() < deadline, "DNAT did not expire")
            time.sleep(.05)
        fixture.exchange(backend.reverse(), reason="nat_miss_drops")
        fixture.exchange(Flow(), output=1, translated=replace(Flow(), source="203.0.113.9", source_port=40002))
        fixture.flush()
        fixture.roundtrip(original, backend)
        check(fixture.policy() and fixture.status()["active"] == 1, "aging or flush lost static policy")
        fixture.stop()
        print("PASS DNAT aging: capacity reuse, old reply rejection, static reservation after GC and flush", flush=True)
    finally:
        fixture.cleanup()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    arguments = parser.parse_args()
    check(os.geteuid() == 0, "root required for a private namespace")
    check(ctypes.CDLL(None, use_errno=True).unshare(0x40000000) == 0, "cannot create private namespace")
    check({name for _, name in socket.if_nameindex()} == {"lo"}, "namespace was not empty")
    for setting in ("all", "default"):
        Path(f"/proc/sys/net/ipv6/conf/{setting}/disable_ipv6").write_text("1")
    for combined in (False, True):
        with tempfile.TemporaryDirectory(prefix="dppd-dnat-") as temporary:
            validate(arguments.build_dir.resolve(), Path(temporary), combined)
    with tempfile.TemporaryDirectory(prefix="dppd-dnat-aging-") as temporary:
        aging(arguments.build_dir.resolve(), Path(temporary))
    check({name for _, name in socket.if_nameindex()} == {"lo"}, "DNAT test leaked interfaces")
    print("PASS DNAT private namespace cleanup", flush=True)


if __name__ == "__main__":
    main()
