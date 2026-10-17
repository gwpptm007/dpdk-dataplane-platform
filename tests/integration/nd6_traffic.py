#!/usr/bin/env python3
"""在自建三 TAP 中独立扮演 IPv6 邻居，验收 ND 首包、NUD、DAD、VLAN 和 NAT66"""

import argparse
import ctypes
from dataclasses import replace
import os
from pathlib import Path
import re
import select
import socket
import struct
import subprocess
import tempfile
import threading
import time

from conntrack_nat import crc
from ipv4_routing import check
from ipv6_traffic import Flow6
from nat66_traffic import StatefulIPv6Fixture


def wait_until(predicate, message, timeout=3):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.02)
    raise AssertionError(message)


def decoded(data, ancillary):
    for level, kind, auxiliary in ancillary:
        if level == 263 and kind == 8 and len(auxiliary) >= 20:
            status, _, _, _, _, tci, tpid = struct.unpack("=IIIHHHH", auxiliary[:20])
            if status & 16:
                data = data[:12] + struct.pack("!HH", tpid if status & 64 else 0x8100, tci) + data[12:]
    l2 = 14
    while len(data) >= l2 + 4 and data[l2 - 2:l2] in (b"\x81\0", b"\x88\xa8"):
        l2 += 4
    return data, l2


class Neighbor:
    def __init__(self, fixture):
        self.fixture = fixture
        self.links = []
        self.controls, self.packets, self.errors = [], [], []
        self.auto = False
        self.stopped = threading.Event()
        for name in fixture.names:
            link = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
            link.setsockopt(263, 8, 1)
            link.bind((name, 0))
            self.links.append(link)
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def run(self):
        try:
            while not self.stopped.is_set():
                ready, _, _ = select.select(self.links, [], [], .05)
                for link in ready:
                    data, ancillary, _, _ = link.recvmsg(65536, 1024)
                    data, l2 = decoded(data, ancillary)
                    port = self.links.index(link)
                    if len(data) < l2 + 40 or data[6:12] != self.fixture.macs[port] or data[l2 - 2:l2] != b"\x86\xdd":
                        continue
                    if data[l2 + 6] == 58 and len(data) >= l2 + 64 and data[l2 + 40] in (135, 136):
                        self.controls.append((port, data))
                        if self.auto and data[l2 + 40] == 135 and data[l2 + 8:l2 + 24] != bytes(16):
                            target = socket.inet_ntop(socket.AF_INET6, data[l2 + 48:l2 + 64])
                            if target == ("fe80::10" if port == 0 else "fe80::1"):
                                destination = socket.inet_ntop(socket.AF_INET6, data[l2 + 8:l2 + 24])
                                self.send(port, 136, target, destination, target, flags=0x60)
                    else:
                        self.packets.append((port, data))
        except Exception as error:
            self.errors.append(repr(error))

    def send(self, port, kind, source, destination, target, flags=0, option=True, hop=255, corrupt=False, mac=None):
        self.links[port].send(self.fixture.control(port, kind, source, destination, target,
                                                 flags, option, hop, corrupt, mac))

    def close(self):
        self.stopped.set()
        self.thread.join(2)
        check(not self.thread.is_alive(), "neighbor observer did not stop")
        for link in self.links:
            link.close()
        check(not self.errors, f"neighbor observer failed: {self.errors}")


class Fixture(StatefulIPv6Fixture):
    def __init__(self, build, directory, nat66, vlans):
        super().__init__(build, directory, nat66)
        self.vlans = vlans
        self.peer_macs = [bytes.fromhex(value) for value in ("020000001000", "020000002001", "020000003002")]
        suffix = "".join(f",{37 + index}" for index in range(vlans))
        self.extra_arguments += ["--nd6", "0,2001:db8:1::fe" + suffix,
                                 "--nd6", "1,2001:db8:2::fe" + suffix, "--nd6-reachable-ms", "1000"]
        if nat66:
            self.extra_arguments += ["--nd6", "1,2001:db8:100::9" + suffix]

    def setup(self):
        self.setup_routes()
        self.change("route6-set", "2001:db8:1::/64", 0, "fe80::10")
        self.change("route6-set", "2001:db8:2::/64", 1, "fe80::1")
        self.state_acl(2000, 0, "allow", "new,established")
        self.state_acl(2001, 1, "allow", "established")
        for port in (0, 1):
            self.state_acl(2005 + port, port, "allow", "invalid", priority=5)
            self.state_acl(2010 + port, port, "drop", None, priority=100)

    def frame(self, flow, tag, ingress, output=None):
        wire, marker = super().frame(flow, tag, ingress, output)
        if isinstance(flow, Flow6) and output is not None:
            wire = self.peer_macs[output] + wire[6:]
        return wire, marker

    def control(self, port, kind, source, destination, target, flags=0, option=True, hop=255, corrupt=False, mac=None):
        source_mac = self.peer_macs[port] if mac is None else mac
        addresses = socket.inet_pton(socket.AF_INET6, source) + socket.inet_pton(socket.AF_INET6, destination)
        body = bytearray(struct.pack("!BBHI", kind, 0, 0, flags << 24) + socket.inet_pton(socket.AF_INET6, target))
        if option:
            body += bytes([1 if kind == 135 else 2, 1]) + source_mac
        struct.pack_into("!H", body, 2, crc(addresses + struct.pack("!I3xB", len(body), 58) + body))
        if corrupt:
            body[3] ^= 1
        destination_mac = b"\x33\x33" + addresses[-4:] if addresses[16] == 255 else self.macs[port]
        wire = destination_mac + source_mac
        for index in range(self.vlans):
            wire += struct.pack("!HH", 0x88a8 if self.vlans == 2 and index == 0 else 0x8100, 37 + index)
        return wire + b"\x86\xdd" + struct.pack("!IHBB", 6 << 28, len(body), 58, hop) + addresses + body

    def nd_status(self):
        return {key: int(value) for key, value in re.findall(r"([a-z-]+)=(\d+)", self.cli("nd6-status").splitlines()[0])}

    def flush_nd(self):
        # NUD 定时转换也会推进修订号，只在精确版本竞争时重新查询，不掩盖其他错误
        for _ in range(8):
            revision = self.nd_status()["revision"]
            result = subprocess.run([str(self.build / "dppctl"), "--socket", str(self.socket),
                                     "nd6-flush", str(revision)], capture_output=True, text=True, timeout=8)
            if result.returncode == 0:
                return
            check("(-116)" in result.stderr, "unexpected ND flush error: " + result.stderr)
        raise AssertionError("ND flush revision kept changing")

    def hits(self, identity):
        return int(re.search(r"hits=(\d+)", self.cli("count", identity, self.rule_generation(identity))).group(1))


def validate(build, directory, nat66, vlans):
    fixture, neighbor = Fixture(build, directory, nat66, vlans), None
    try:
        fixture.create()
        fixture.start()
        fixture.setup()
        neighbor = Neighbor(fixture)
        original = Flow6(vlans=vlans)
        fixture.sequence += 1
        tag = fixture.sequence
        wire, marker = fixture.frame(original, tag, 0)
        hits = fixture.hits(2000)
        fixture.links[0].send(wire)
        wait_until(lambda: any(port == 1 for port, _ in neighbor.controls), "missing multicast NS for first packet")
        check(fixture.active() == 0 and fixture.nd_status()["pending"] == 1, "unresolved packet committed a session")
        # 有效范围外的 Hop Limit 和坏校验和不能完成解析，等待包也不能泄漏到出口
        for hop, corrupt in ((254, False), (255, True)):
            neighbor.send(1, 136, "fe80::1", "2001:db8:2::fe", "fe80::1", flags=0x60, hop=hop, corrupt=corrupt)
            time.sleep(.04)
            check("state=incomplete" in fixture.cli("nd6-list"), "invalid NA changed neighbor state")
        check(not any(marker in data for _, data in neighbor.packets), "first packet escaped before resolution")
        neighbor.send(1, 136, "fe80::1", "2001:db8:2::fe", "fe80::1", flags=0x60)
        wait_until(lambda: any(marker in data for _, data in neighbor.packets), "held first packet was not released")
        found = [(port, data) for port, data in neighbor.packets if marker in data]
        check(len(found) == 1 and found[0][0] == 1, "first packet duplicated or sent to wrong port")
        mapped = original
        if nat66:
            mapped = replace(original, source="2001:db8:100::9",
                             source_port=struct.unpack_from("!H", found[0][1], 14 + 4 * vlans + 40)[0])
            check(mapped.source_port in (40001, 40002), "ND retry borrowed static NAT66 reservation")
        expected, _ = fixture.frame(replace(mapped, hop=63), tag, 0, 1)
        check(found[0][1] == expected, "ND first packet bytes/checksum changed")
        check(fixture.hits(2000) == hits + 1 and fixture.active() == 1, "ND retry duplicated COUNT or session")
        neighbor.auto = True
        fixture.exchange(mapped.reverse(), ingress=1, output=0, translated=original.reverse())
        fixture.exchange(original, output=1)
        check(fixture.nd_status()["pending"] == 0, "resolved packets remained queued")
        # 只有发送给本机的 NS 才学习源，回应必须保持 Hop Limit、标志、选项和完整校验和
        target = "2001:db8:100::9" if nat66 else "2001:db8:2::fe"
        group = "ff02::1:ff00:9" if nat66 else "ff02::1:ff00:fe"
        before = len(neighbor.controls)
        neighbor.send(1, 135, "2001:db8:2::7", group, target)
        wait_until(lambda: any(data[14 + 4 * vlans + 40] == 136 for _, data in neighbor.controls[before:]), "local NS did not receive NA")
        response = next(data for _, data in neighbor.controls[before:] if data[14 + 4 * vlans + 40] == 136)
        ip = response[14 + 4 * vlans:]
        check(ip[7] == 255 and ip[44] == 0xe0 and ip[64:66] == b"\x02\x01", "bad local NA flags or option")
        check(crc(ip[8:40] + struct.pack("!I3xB", len(ip) - 40, 58) + ip[40:]) == 0, "local NA checksum invalid")
        # 静态邻居必须盖过动态表，并且线上学习不能更改静态配置文件
        static = bytes.fromhex("020000004401")
        fixture.change("neighbor6-set", 1, "fe80::1", "02:00:00:00:44:01")
        fixture.peer_macs[1] = static
        saved = fixture.snapshot.read_bytes()
        fixture.exchange(original, output=1)
        neighbor.send(1, 136, "fe80::1", "2001:db8:2::fe", "fe80::1", flags=0x60)
        time.sleep(.03)
        fixture.exchange(original, output=1)
        check(fixture.snapshot.read_bytes() == saved, "dynamic ND overwrote static snapshot")
        fixture.change("neighbor6-delete", 1, "fe80::1")
        fixture.peer_macs[1] = bytes.fromhex("020000002001")
        fixture.flush_nd()
        fixture.exchange(original, output=1)
        # 未解析时更新 ACL，等待包必须重走新策略，不能沿用排队前的允许结果
        neighbor.auto = False
        fixture.flush_nd()
        fixture.sequence += 1
        waiting, marker = fixture.frame(original, fixture.sequence, 0)
        fixture.links[0].send(waiting)
        wait_until(lambda: fixture.nd_status()["pending"] == 1, "packet did not enter ND queue")
        fixture.state_acl(2000, 0, "drop", "new,established", expected=fixture.rule_generation(2000))
        neighbor.send(1, 136, "fe80::1", "2001:db8:2::fe", "fe80::1", flags=0x60)
        wait_until(lambda: fixture.nd_status()["pending"] == 0, "denied queued packet was retained")
        check(not any(marker in data for _, data in neighbor.packets), "queued packet bypassed updated ACL")
        fixture.state_acl(2000, 0, "allow", "new,established", expected=fixture.rule_generation(2000))
        neighbor.auto = True
        fixture.exchange(original, output=1)
        if vlans == 0 and not nat66:
            # 队列最多保留同一邻居的四个首包，溢出明确丢弃，放行后只能逐个交付一次
            neighbor.auto = False
            fixture.flush_nd()
            before = fixture.stats()["nd6_queue_drops"]
            tags = []
            for _ in range(8):
                fixture.sequence += 1
                tags.append(fixture.sequence)
                packet, _ = fixture.frame(original, fixture.sequence, 0)
                fixture.links[0].send(packet)
            wait_until(lambda: fixture.nd_status()["pending"] == 4 and fixture.stats()["nd6_queue_drops"] >= before + 4,
                       "ND queue bound was not enforced")
            neighbor.send(1, 136, "fe80::1", "2001:db8:2::fe", "fe80::1", flags=0x60)
            wait_until(lambda: fixture.nd_status()["pending"] == 0, "bounded ND queue did not drain")
            expected_frames = [fixture.frame(replace(original, hop=63), tag, 0, 1)[0] for tag in tags[:4]]
            wait_until(lambda: all(any(data == expected for _, data in neighbor.packets) for expected in expected_frames),
                       "bounded ND queue did not deliver its four retained packets")
            for index, tag in enumerate(tags):
                expected, _ = fixture.frame(replace(original, hop=63), tag, 0, 1)
                check(sum(data == expected for _, data in neighbor.packets) == (1 if index < 4 else 0),
                      "ND overflow packet escaped or queued packet duplicated")
            # 线上 MAC 变化必须真正影响数据出口，Override=0 则不能悄悄替换旧值
            replacement = bytes.fromhex("020000005501")
            neighbor.send(1, 136, "fe80::1", "2001:db8:2::fe", "fe80::1", flags=0x40, mac=replacement)
            time.sleep(.03)
            check("mac=02:00:00:00:20:01" in fixture.cli("nd6-list"), "Override=0 replaced MAC")
            neighbor.send(1, 136, "fe80::1", "2001:db8:2::fe", "fe80::1", flags=0x60, mac=replacement)
            wait_until(lambda: "mac=02:00:00:00:55:01" in fixture.cli("nd6-list"), "Override=1 did not replace MAC")
            fixture.peer_macs[1] = replacement
            fixture.exchange(original, output=1)
            neighbor.auto = True
            fixture.flush_nd()
            fixture.exchange(original, output=1)
            neighbor.auto = False
            controls = len(neighbor.controls)
            def target_state(state):
                return any("address=fe80::1 " in line and "state=" + state in line
                           for line in fixture.cli("nd6-list").splitlines())
            wait_until(lambda: target_state("stale"), "reachable neighbor did not become STALE")
            fixture.exchange(original, output=1)
            wait_until(lambda: target_state("probe"), "STALE/DELAY did not start unicast probing", timeout=6)
            wait_until(lambda: target_state("failed"), "unanswered probes did not mark neighbor FAILED", timeout=4)
            probes = [data for port, data in neighbor.controls[controls:]
                      if port == 1 and data[:6] == replacement and data[54] == 135]
            check(len(probes) == 3, "NUD did not use bounded unicast retries")
            fixture.exchange(original, reason="no_neighbor_drops")
            neighbor.auto = True
            fixture.flush_nd()
            fixture.exchange(original, output=1)
        routing, rules = fixture.snapshot.read_bytes(), (directory / "rules").read_bytes()
        neighbor.close()
        neighbor = None
        fixture.stop(crash=True)
        fixture.start()
        check(fixture.nd_status()["active"] == 0 and fixture.active() == 0, "ND cache or sessions survived restart")
        check(fixture.snapshot.read_bytes() == routing and (directory / "rules").read_bytes() == rules, "restart changed durable configuration")
        neighbor = Neighbor(fixture)
        neighbor.auto = True
        fixture.exchange(original, output=1)
        print(f"PASS ND TAP nat66={nat66} vlans={vlans}: first packet, NS/NA, full bytes, COUNT, state ACL recheck, static priority, crash replay", flush=True)
    finally:
        if neighbor is not None:
            neighbor.close()
        fixture.cleanup()


def duplicate(build, directory):
    fixture, neighbor = Fixture(build, directory, False, 0), None
    try:
        fixture.create()
        fixture.start(allow_not_ready=True)
        neighbor = Neighbor(fixture)
        neighbor.send(1, 136, "fe80::1", "ff02::1", "2001:db8:2::fe", flags=0x20)
        wait_until(lambda: "state=duplicate" in fixture.cli("nd6-status"), "DAD did not latch duplicate address")
        check("nd6-not-ready" in fixture.cli("ready", success=False), "duplicate address reported ready")
        fixture.flush_nd()
        check("state=duplicate" in fixture.cli("nd6-status"), "cache flush cleared local address conflict")
        print("PASS ND DAD: duplicate local address blocks readiness and survives cache flush", flush=True)
    finally:
        if neighbor is not None:
            neighbor.close()
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
    for vlans in (0, 1, 2):
        for nat66 in (False, True):
            with tempfile.TemporaryDirectory(prefix="dppd-nd6-") as temporary:
                validate(arguments.build_dir.resolve(), Path(temporary), nat66, vlans)
    with tempfile.TemporaryDirectory(prefix="dppd-nd6-dad-") as temporary:
        duplicate(arguments.build_dir.resolve(), Path(temporary))
    check({name for _, name in socket.if_nameindex()} == {"lo"}, "ND test leaked interfaces")
    print("PASS ND private namespace cleanup", flush=True)


if __name__ == "__main__":
    main()
