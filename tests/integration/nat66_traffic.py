#!/usr/bin/env python3
"""在私有三 TAP 中核对 IPv6 会话、状态 ACL、NAT66 双向改写、资源回收和重启"""

import argparse
import ctypes
from dataclasses import replace
import os
from pathlib import Path
import socket
import tempfile
import time

from conntrack_nat import Flow
from ipv4_routing import check
from ipv6_traffic import Flow6, IPv6Fixture


class StatefulIPv6Fixture(IPv6Fixture):
    def __init__(self, build, directory, nat66, timeout=60):
        # 同一进程始终保留 IPv4 SNAT，验证两种地址族不能互相借用转换策略或端口
        super().__init__(build, directory, True)
        self.auto_snat6 = nat66
        self.extra_arguments = ["--conntrack", "16", "--ct-timeout", str(timeout),
                                "--snat", "0,1,203.0.113.9,40000-40001"]
        if nat66:
            self.extra_arguments += ["--snat6", "0,1,2001:db8:100::9,40000-40002",
                                     "--dnat6", "1,0,udp,2001:db8:100::9,40000,2001:db8:1::8,8000",
                                     "--dnat6", "1,0,tcp,2001:db8:100::9,8443-8445,2001:db8:1::8,443-445",
                                     "--dnat6", "1,0,udp,2001:db8:100::a,any,2001:db8:1::9,same"]

    def setup(self):
        self.setup_routes()
        for port, gateway, mac in ((0, "fe80::10", "02:00:00:00:10:00"),
                                   (1, "fe80::1", "02:00:00:00:20:01"),
                                   (2, "fe80::1", "02:00:00:00:30:02")):
            self.change("neighbor6-set", port, gateway, mac)
        self.change("route6-set", "2001:db8:1::/64", 0, "fe80::10")
        self.change("route6-set", "2001:db8:2::/64", 1, "fe80::1")
        self.state_acl(2000, 0, "allow", "new,established")
        self.state_acl(2001, 1, "allow", "established")
        for port in (0, 1):
            # 允许 INVALID 只用于核对错误计数，worker 仍必须拒绝实际报文
            self.state_acl(2005 + port, port, "allow", "invalid", priority=5)
            self.state_acl(2010 + port, port, "drop", None, priority=100)

    def state_acl(self, identity, port, action, states, priority=10, expected=0,
                  destination="any", protocol="ipv6", destination_ports="any"):
        arguments = ["acl6-set", identity, port, expected, priority, protocol, "any", destination,
                     "any", destination_ports, action, "count"]
        if states:
            arguments.append("state:" + states)
        return self.cli(*arguments)

    def active(self):
        return self.status()["active"]


def validate(build, directory, nat66):
    fixture = StatefulIPv6Fixture(build, directory, nat66)
    try:
        fixture.create()
        fixture.start()
        fixture.setup()
        original = Flow6()
        # 首个回包须被状态 ACL 拒绝，不能用一个未建立的 UDP 反向包绕过入口策略
        fixture.exchange(original.reverse(), ingress=1, reason="nat_miss_drops" if nat66 else "rule_drops")
        check(fixture.active() == 0, "denied reply created a session")
        wire = fixture.exchange(original, output=1)
        fixture.exchange(wire.reverse(), ingress=1, output=0, translated=original.reverse())
        check(fixture.active() == 1, "two directions did not share one IPv6 session")
        sessions = fixture.cli("conntrack-list")
        check("family=ipv6" in sessions and "state=established" in sessions, "IPv6 management view missing")
        check(("kind=snat" in sessions) == nat66, "wrong IPv6 NAT kind")
        if nat66:
            check(wire.source_port != 40000, "SNAT66 borrowed a statically reserved UDP port")
            fixture.exchange(replace(wire.reverse(), source="2001:db8:2::8"), ingress=1, reason="nat_miss_drops")
            fixture.exchange(wire.reverse(), ingress=2, reason="nat_miss_drops")
        for vlan in (0, 1, 2):
            for extensions in ((), ("hop",), ("hop", "dest")):
                outgoing = replace(original, vlans=vlan, extensions=extensions)
                mapped = fixture.exchange(outgoing, output=1)
                fixture.exchange(mapped.reverse(), ingress=1, output=0, translated=outgoing.reverse())
        check(fixture.active() == 1, "VLAN or extension header split the same IPv6 flow")
        # 状态规则由同一批次替换，策略拒绝不能刷新会话，也不能改变原来的映射
        batch = directory / "state-batch"
        for dropped in (True, False):
            action = "drop" if dropped else "allow"
            batch.write_text(f"2000 0 {fixture.rule_generation(2000)} 10 ipv6 any any any any {action} count state:new,established\n"
                             f"2001 1 {fixture.rule_generation(2001)} 10 ipv6 any any any any {action} count state:established\n")
            fixture.cli("acl6-update-batch", batch)
            fixture.exchange(wire.reverse(), ingress=1, output=None if dropped else 0,
                             translated=original.reverse(), reason="rule_drops" if dropped else None)
        fixture.cli("acl6-update-batch", batch, success=False)
        fixture.change("route6-set", "2001:db8:2::/64", 2, "fe80::1")
        fixture.exchange(original, reason="ct_zone_drops")
        fixture.change("route6-set", "2001:db8:2::/64", 1, "fe80::1")
        fixture.exchange(original, output=1)
        before = fixture.active()
        fixture.exchange(original, corrupt=True, reason="ct_checksum_drops")
        fixture.exchange(replace(original, udp_zero=True), reason="ct_checksum_drops")
        fixture.exchange(replace(original, hop=1), reason="ipv6_hop_drops")
        check(fixture.active() == before, "invalid IPv6 packet changed active sessions")
        if nat66:
            fixture.exchange(replace(original, source_port=1501), output=1)
            fixture.exchange(replace(original, source_port=1502), reason="nat_pool_drops")
        # 同一数字端口仍可用于 IPv4，IPv6 地址必须在管理响应中完整显示
        before = fixture.active()
        ipv4 = Flow()
        ipv4_wire = fixture.exchange(ipv4, output=1)
        fixture.exchange(ipv4_wire.reverse(), ingress=1, output=0, translated=ipv4.reverse())
        check(fixture.active() == before + 1, "IPv4 and IPv6 NAT did not coexist")
        all_sessions = fixture.cli("conntrack-list")
        check("family=ipv4" in all_sessions and "family=ipv6" in all_sessions, "mixed session query lost a family")
        fixture.flush()
        fixture.exchange(replace(original, destination="2001:db8:3::7"), reason="no_route_drops")
        fixture.change("route6-set", "2001:db8:3::/64", 1, "fe80::99")
        fixture.exchange(replace(original, destination="2001:db8:3::7"), reason="no_neighbor_drops")
        check(fixture.active() == 0, "failed route or neighbor leaked a candidate")
        fixture.exchange(replace(original, extensions=("fragment",)), reason="ct_unsupported_drops")
        fixture.exchange(replace(original, extensions=("route",)), reason="ct_unsupported_drops")
        fixture.state_acl(2020, 0, "allow", "untracked", protocol="ip:58", priority=1)
        fixture.exchange(replace(original, protocol=58), output=None if nat66 else 1,
                         reason="ct_unsupported_drops" if nat66 else None)
        # 正确握手才进入 OPEN，中途 ACK、错误确认和带负载的 SYN 都不能建立会话
        tcp = replace(original, protocol=6, flags=2, sequence=100, acknowledgment=0,
                      vlans=2, extensions=("hop", "dest"))
        fixture.exchange(replace(tcp, flags=16), reason="ct_invalid_drops")
        fixture.exchange(replace(tcp, payload=b"forbidden-syn-data"), reason="ct_invalid_drops")
        tcp_wire = fixture.exchange(tcp, output=1)
        syn_ack = replace(tcp_wire.reverse(), flags=18, sequence=500, acknowledgment=101)
        fixture.exchange(replace(syn_ack, acknowledgment=102), ingress=1, reason="ct_invalid_drops")
        fixture.exchange(syn_ack, ingress=1, output=0,
                         translated=replace(tcp.reverse(), flags=18, sequence=500, acknowledgment=101))
        ack = replace(tcp, flags=16, sequence=101, acknowledgment=501)
        fixture.exchange(ack, output=1)
        check("tcp=open" in fixture.cli("conntrack-list"), "IPv6 TCP did not reach OPEN")
        fixture.exchange(replace(ack, flags=17, payload=b""), output=1)
        fixture.exchange(replace(tcp_wire.reverse(), flags=20, sequence=501, acknowledgment=102), ingress=1, output=0,
                         translated=replace(tcp.reverse(), flags=20, sequence=501, acknowledgment=102))
        check("tcp=closed" in fixture.cli("conntrack-list"), "IPv6 RST did not close TCP")
        fixture.flush()
        if nat66:
            # ACL 匹配 DNAT 后的后端地址和端口，而不是线缆上的公网服务地址
            inbound = replace(original.reverse(), destination="2001:db8:100::9", destination_port=40000)
            backend = replace(inbound, destination="2001:db8:1::8", destination_port=8000)
            fixture.exchange(inbound, ingress=1, reason="rule_drops")
            check(fixture.active() == 0, "denied DNAT66 request created a session")
            fixture.state_acl(2030, 1, "allow", "new", priority=5,
                              destination="2001:db8:1::8/128", protocol="udp", destination_ports="8000")
            fixture.exchange(inbound, ingress=1, output=0, translated=backend)
            fixture.exchange(backend.reverse(), ingress=0, output=1, translated=inbound.reverse())
            check("kind=dnat dnat-rule=1" in fixture.cli("conntrack-list"), "DNAT66 identity not visible")
            fixture.state_acl(2031, 1, "allow", "new", priority=5,
                              destination="2001:db8:1::8/128", protocol="tcp", destination_ports="443-445")
            for port in (8443, 8444, 8445):
                request = replace(inbound, protocol=6, flags=2, sequence=100, acknowledgment=0,
                                  source_port=2500 + port, destination_port=port)
                private = replace(request, destination="2001:db8:1::8", destination_port=port - 8000)
                fixture.exchange(request, ingress=1, output=0, translated=private)
                response = replace(private.reverse(), flags=18, sequence=500, acknowledgment=101)
                fixture.exchange(response, ingress=0, output=1,
                                 translated=replace(request.reverse(), flags=18, sequence=500, acknowledgment=101))
            fixture.state_acl(2032, 1, "allow", "new", priority=5, destination="2001:db8:1::9/128", protocol="udp")
            for port in (1, 65535):
                request = replace(inbound, destination="2001:db8:100::a", destination_port=port)
                private = replace(request, destination="2001:db8:1::9")
                fixture.exchange(request, ingress=1, output=0, translated=private)
                fixture.exchange(private.reverse(), ingress=0, output=1, translated=request.reverse())
            check("count=3" in fixture.cli("dnat-list") and "family=ipv6" in fixture.cli("dnat-list"), "NAT66 static query missing")
        rules = (directory / "rules").read_bytes()
        routes = fixture.snapshot.read_bytes()
        policy = fixture.cli("get", 2000)
        revision = fixture.status()["revision"]
        fixture.cli("conntrack-flush", revision - 1, success=False)
        fixture.stop(crash=True)
        # 含 IPv6 状态规则时禁用跟踪必须拒绝恢复，不能悄悄把状态条件变成永远不命中
        startup = fixture.extra_arguments
        fixture.extra_arguments = []
        fixture.start(refused=True)
        check((directory / "rules").read_bytes() == rules, "refused replay overwrote IPv6 state policy")
        fixture.extra_arguments = startup
        fixture.start()
        check(fixture.active() == 0, "IPv6 sessions survived restart")
        check((directory / "rules").read_bytes() == rules and fixture.snapshot.read_bytes() == routes, "restart changed durable policy")
        check(fixture.cli("get", 2000) == policy, "IPv6 state ACL did not replay")
        fixture.exchange(wire.reverse(), ingress=1, reason="nat_miss_drops" if nat66 else "rule_drops")
        fixture.exchange(original, output=1)
        print(f"PASS IPv6 {'NAT66 SNAT+DNAT' if nat66 else 'connection tracking'}: state ACL/batch, TCP handshake, UDP replies, full bytes, IPv4 isolation, failures, restart", flush=True)
    finally:
        fixture.cleanup()


def aging(build, directory):
    fixture = StatefulIPv6Fixture(build, directory, True, timeout=1)
    try:
        fixture.create()
        fixture.start()
        fixture.setup()
        wire = fixture.exchange(Flow6(), output=1)
        time.sleep(1.2)
        check(fixture.active() == 0 and fixture.status()["expired"] >= 1, "IPv6 session did not expire")
        fixture.exchange(wire.reverse(), ingress=1, reason="nat_miss_drops")
        fixture.exchange(replace(Flow6(), source_port=1501), output=1)
        print("PASS NAT66 aging: expired reverse rejected, pool reusable", flush=True)
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
    for nat66 in (False, True):
        with tempfile.TemporaryDirectory(prefix="dppd-nat66-") as temporary:
            validate(arguments.build_dir.resolve(), Path(temporary), nat66)
    with tempfile.TemporaryDirectory(prefix="dppd-nat66-aging-") as temporary:
        aging(arguments.build_dir.resolve(), Path(temporary))
    check({name for _, name in socket.if_nameindex()} == {"lo"}, "NAT66 test leaked interfaces")
    print("PASS NAT66 private namespace cleanup", flush=True)


if __name__ == "__main__":
    main()
