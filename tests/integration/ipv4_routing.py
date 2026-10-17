#!/usr/bin/env python3
"""在自建网络命名空间的三个 TAP 上验收路由管理、完整报文、重启及持久化故障"""

import argparse
import ctypes
import os
from pathlib import Path
import re
import select
import signal
import socket
import stat
import struct
import subprocess
import tempfile
import time


def check(value, message):
    if not value:
        raise AssertionError(message)


def run(*arguments):
    result = subprocess.run([str(a) for a in arguments], capture_output=True, text=True, timeout=15)
    check(result.returncode == 0, f"{arguments}: {result.stdout}{result.stderr}")
    return result.stdout


def checksum(data):
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return (~total) & 65535


class Fixture:
    def __init__(self, build, directory):
        self.build, self.directory = build, directory
        self.process = None
        self.socket = directory / "control"
        self.snapshot = directory / "routing.bin"
        self.log = directory / "daemon.log"
        self.sequence = 0
        self.names = [f"d4{os.getpid() % 100000}{c}" for c in "abc"]
        self.indices, self.links = [], []

    def create(self):
        for name in self.names:
            run("ip", "tuntap", "add", "dev", name, "mode", "tap", "multi_queue")
            self.indices.append(socket.if_nametoindex(name))
            run("ip", "link", "set", "dev", name, "up")

    def cli(self, *arguments, success=True):
        result = subprocess.run([str(self.build / "dppctl"), "--socket", str(self.socket), *map(str, arguments)],
                                capture_output=True, text=True, timeout=8)
        output = result.stdout + result.stderr
        check((result.returncode == 0) == success, f"CLI {arguments}: {output}")
        return output

    def start(self, faults=False, refused=False):
        check(self.process is None, "previous daemon still exists")
        cpus = sorted(os.sched_getaffinity(0))
        check(len(cpus) >= 2, "two CPUs required")
        executable = self.build / ("tests/integration/test_ipv4_store_window" if faults else "dppd")
        command = [str(executable), f"--lcores=0@{cpus[0]},1@{cpus[1]}", "--main-lcore=0", "--no-pci",
                   "--no-huge", "-m", "64", "--no-telemetry", f"--file-prefix={self.directory.name}"]
        command += [f"--vdev=net_tap{i},iface={name}" for i, name in enumerate(self.names)]
        command += ["--", "--ports", "0,1,2", "--queues", "1", "--mbufs", "8191", "--cache", "0",
                    "--control-socket", str(self.socket), "--state-path", str(self.directory / "rules"),
                    "--ipv4-routing", str(self.snapshot)]
        with self.log.open("w") as output:
            self.process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                check(refused and self.process.returncode != 0, self.log.read_text())
                self.process = None
                return
            if self.socket.exists():
                check(not refused, "invalid routing snapshot accepted")
                # socket 建立早于工作线程启动，使用正式就绪查询等待完整初始化
                ready = subprocess.run([str(self.build / "dppctl"), "--socket", str(self.socket), "ready"],
                                       capture_output=True, text=True, timeout=3)
                if ready.returncode == 0:
                    # 旧抓包 socket 会保留前一次关口的 ENETDOWN，新进程就绪后建立新的观察端点
                    for link in self.links:
                        link.close()
                    self.links = []
                    for name in self.names:
                        link = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
                        link.setsockopt(263, 8, 1)  # PACKET_AUXDATA 保留被内核剥离的 VLAN
                        link.bind((name, 0))
                        link.setblocking(False)
                        self.links.append(link)
                    self.macs = [bytes.fromhex(re.search(r"mac=([0-9a-f:]+)", self.cli("port-show", i)).group(1)
                                              .replace(":", "")) for i in range(3)]
                    return
            time.sleep(.05)
        raise AssertionError("daemon startup timeout: " + self.log.read_text())

    def stop(self, crash=False, verify=True):
        if self.process is None:
            return
        process = self.process
        try:
            process.send_signal(signal.SIGKILL if crash else signal.SIGTERM)
            process.wait(timeout=10)
            check(not verify or crash or process.returncode == 0, self.log.read_text())
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            self.process = None
            # 仅在自有进程确已退出后回收该临时目录的遗留 socket
            if self.socket.exists():
                check(stat.S_ISSOCK(self.socket.lstat().st_mode), "control path replaced")
                self.socket.unlink()

    def generation(self):
        return int(re.search(r"generation=(\d+)", self.cli("routing-status")).group(1))

    def change(self, *arguments):
        generation = self.generation()
        output = self.cli(*arguments, generation)
        check(f"generation={generation + 1} " in output, output)

    def stats(self):
        return {key: int(value) for key, value in re.findall(r"([a-z][a-z0-9_]*)=(\d+)", self.cli("stats"))}

    def frame(self, destination, ttl=64, bad_checksum=False, vlan=False):
        self.sequence += 1
        payload = b"DPPD-IPV4-CLOSURE" + struct.pack("!I", self.sequence) + bytes(range(64))
        udp = struct.pack("!HHHH", 12345, 23456, len(payload) + 8, 0) + payload
        ip = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 77, 0, ttl, 17, 0,
                                   socket.inet_aton("198.18.0.10"), socket.inet_aton(destination)))
        struct.pack_into("!H", ip, 10, checksum(ip) ^ (1 if bad_checksum else 0))
        ethernet = self.macs[0] + bytes.fromhex("020000aabbcc")
        ethernet += bytes.fromhex("810000250800" if vlan else "0800")
        return ethernet + ip + udp, payload

    def exchange(self, destination, output_port=None, neighbor=None, reason=None, **options):
        before = self.stats() if reason else None
        frame, payload = self.frame(destination, **options)
        for link in self.links:
            while True:
                try:
                    link.recv(65536)
                except BlockingIOError:
                    break
        self.links[0].send(frame)
        observed = []
        deadline = time.monotonic() + (.25 if output_port is None else 2)
        while time.monotonic() < deadline:
            readable, _, _ = select.select(self.links, [], [], max(0, deadline - time.monotonic()))
            for link in readable:
                packet, ancillary, _, _ = link.recvmsg(65536, 1024)
                # TAP 进入内核后可能把 VLAN 移入辅助信息，按内核给出的原标签还原线缆字节
                for level, kind, data in ancillary:
                    if level == 263 and kind == 8 and len(data) >= 20:
                        status, _, _, _, _, tci, tpid = struct.unpack("=IIIHHHH", data[:20])
                        if status & (1 << 4):
                            packet = packet[:12] + struct.pack("!HH", tpid if status & (1 << 6) else 0x8100, tci) + packet[12:]
                if payload in packet:
                    observed.append((self.links.index(link), packet))
            if observed:
                break
        if output_port is None:
            check(not observed, f"drop unexpectedly forwarded to {observed}")
            if reason:
                after = self.stats()
                check(after[reason] >= before[reason] + 1, f"missing {reason}: {before} -> {after}")
        else:
            expected = bytearray(frame)
            expected[:6], expected[6:12] = bytes.fromhex(neighbor.replace(":", "")), self.macs[output_port]
            offset = 18 if options.get("vlan") else 14
            expected[offset + 8] -= 1
            expected[offset + 10:offset + 12] = b"\0\0"
            struct.pack_into("!H", expected, offset + 10, checksum(expected[offset:offset + 20]))
            check(observed == [(output_port, bytes(expected))],
                  f"wrong output expected port={output_port} bytes={expected.hex()} observed={observed}")

    def cleanup(self):
        self.stop(verify=False)
        for link in self.links:
            link.close()
        for name, index in zip(self.names, self.indices):
            check(socket.if_nametoindex(name) == index, "test TAP identity changed")
            run("ip", "link", "delete", "dev", name)


def validate(build, directory):
    fixture = Fixture(build, directory)
    a, b = "02:00:00:00:11:01", "02:00:00:00:22:02"
    try:
        fixture.create()
        fixture.start()
        check(fixture.generation() == 1, "initial generation")
        fixture.change("neighbor-set", 1, "192.0.2.1", a)
        fixture.change("route-set", "10.0.0.0/8", 1, "192.0.2.1")
        fixture.change("neighbor-set", 2, "192.0.2.2", b)
        fixture.change("route-set", "10.2.0.0/16", 2, "192.0.2.2")
        fixture.exchange("10.2.3.4", 2, b)
        fixture.exchange("10.3.3.4", 1, a, vlan=True)
        fixture.change("neighbor-set", 2, "10.4.0.8", b)
        fixture.change("route-set", "10.4.0.0/16", 2, "direct")
        fixture.exchange("10.4.0.8", 2, b)
        fixture.change("route-set", "10.2.0.0/16", 2, "192.0.2.9")
        fixture.exchange("10.2.3.4", reason="no_neighbor_drops")
        fixture.change("route-set", "10.2.0.0/16", 2, "192.0.2.2")
        fixture.exchange("11.2.3.4", reason="no_route_drops")
        fixture.exchange("10.2.3.4", ttl=1, reason="ipv4_ttl_drops")
        fixture.exchange("10.2.3.4", bad_checksum=True, reason="ipv4_checksum_drops")
        saved, generation = fixture.snapshot.read_bytes(), fixture.generation()
        fixture.cli("route-set", "10.2.0.0/16", 1, "192.0.2.1", generation - 1, success=False)
        fixture.cli("route-set", "10.2.0.0/16", 8, "192.0.2.1", generation, success=False)
        fixture.cli("neighbor-set", 1, "192.0.2.1", "01:00:00:00:00:01", generation, success=False)
        replacement = directory / "replacement.txt"
        replacement.write_text("route 10.0.0.0/8 2 192.0.2.2\nbroken\n")
        fixture.cli("routing-replace", replacement, generation, success=False)
        check(saved == fixture.snapshot.read_bytes() and generation == fixture.generation(), "rejected update changed state")
        replacement.write_text(f"route 10.0.0.0/8 2 192.0.2.2\nneighbor 2 192.0.2.2 {b}\n")
        fixture.change("routing-replace", replacement)
        fixture.exchange("10.3.3.4", 2, b)
        fixture.cli("routing-flush")
        # 软件规则优先于 TTL 检查，重启重放后仍须保持相同语义
        fixture.cli("apply-filter", 901, 0, 0, "udp", "any", "10.9.0.0/16", "any", "any",
                    "drop", "count", "software")
        fixture.exchange("10.9.0.8", ttl=1, reason="rule_drops")
        policy = fixture.cli("get", 901)
        policy_generation = int(re.search(r"generation=(\d+)", policy).group(1))
        check("hits=1 " in fixture.cli("count", 901, policy_generation), "software COUNT missing")
        saved, shown = fixture.snapshot.read_bytes(), fixture.cli("routing-show")
        fixture.stop(crash=True)
        fixture.start()
        check(saved == fixture.snapshot.read_bytes() and shown == fixture.cli("routing-show"), "restart changed configuration")
        fixture.exchange("10.3.3.4", 2, b, vlan=True)
        fixture.exchange("10.9.0.8", ttl=1, reason="rule_drops")
        check(fixture.cli("get", 901) == policy, "restart changed policy generation or content")
        check("hits=1 " in fixture.cli("count", 901, policy_generation), "replayed software COUNT missing")
        fixture.stop()
        # 损坏、截断和稳定端口映射错配均须拒绝，不能启动时用空表覆盖证据
        for damaged in (saved[:-1], saved[:32] + bytes([saved[32] ^ 1]) + saved[33:]):
            fixture.snapshot.write_bytes(damaged)
            fixture.start(refused=True)
            check(fixture.snapshot.read_bytes() == damaged, "bad snapshot overwritten")
        fixture.snapshot.write_bytes(saved)
        fixture.names[1], fixture.names[2] = fixture.names[2], fixture.names[1]
        try:
            fixture.start(refused=True)
            check(fixture.snapshot.read_bytes() == saved, "binding mismatch overwritten")
        finally:
            fixture.names[1], fixture.names[2] = fixture.names[2], fixture.names[1]
        print("PASS IPv4 daemon: three TAPs, LPM/direct/VLAN/full bytes, drops, CRUD, replacement, restart, bad snapshots", flush=True)
        fixture.start(faults=True)
        for mode in ("file", "directory"):
            generation, saved = fixture.generation(), fixture.snapshot.read_bytes()
            (directory / "route-store-fault").write_text(mode)
            output = fixture.cli("route-set", "10.0.0.0/8", 1, "192.0.2.1", generation, success=False)
            check("dirty=yes" in output and fixture.generation() == generation, output)
            disk_generation = struct.unpack_from("<Q", fixture.snapshot.read_bytes(), 16)[0]
            check(disk_generation == generation + (mode == "directory"), "unexpected rename window")
            fixture.cli("route-set", "10.0.0.0/8", 1, "192.0.2.1", generation, success=False)
            check("routing-dirty" in fixture.cli("ready", success=False), "dirty routing reported ready")
            fixture.exchange("10.3.3.4", 2, b)
            check("dirty=no" in fixture.cli("routing-flush"), "flush did not repair")
            check(fixture.snapshot.read_bytes() == saved, "flush did not restore active configuration")
            print(f"PASS IPv4 daemon {mode} fsync failure: old active route, dirty health, blocked writes, explicit flush", flush=True)
        # 目录同步失败后崩溃不能假称磁盘回滚，重启恢复已经出现的完整候选版本
        generation = fixture.generation()
        (directory / "route-store-fault").write_text("directory")
        fixture.cli("neighbor-set", 1, "192.0.2.1", a, generation, success=False)
        candidate = fixture.snapshot.read_bytes()
        fixture.stop(crash=True)
        fixture.start()
        check(fixture.generation() == generation + 1 and fixture.snapshot.read_bytes() == candidate, "uncertain restart version")
        fixture.exchange("10.3.3.4", 2, b)
        fixture.change("neighbor-delete", 1, "192.0.2.1")
        fixture.change("route-delete", "10.0.0.0/8")
        fixture.exchange("10.3.3.4", reason="no_route_drops")
        fixture.stop()
        print("PASS IPv4 daemon uncertain commit restart and route/neighbor deletion", flush=True)
    finally:
        fixture.cleanup()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    args = parser.parse_args()
    check(os.geteuid() == 0, "root required to create an isolated namespace and TAPs")
    build = args.build_dir.resolve()
    check(ctypes.CDLL(None, use_errno=True).unshare(0x40000000) == 0, "cannot create isolated network namespace")
    check({name for _, name in socket.if_nameindex()} == {"lo"}, "namespace was not empty")
    # 只关闭新命名空间内的 IPv6 自动报文，避免后台地址探测影响丢弃计数
    for setting in ("all", "default"):
        Path(f"/proc/sys/net/ipv6/conf/{setting}/disable_ipv6").write_text("1")
    with tempfile.TemporaryDirectory(prefix="dppd-l3-") as temporary:
        validate(build, Path(temporary))
    check({name for _, name in socket.if_nameindex()} == {"lo"}, "test leaked interfaces")
    print("PASS IPv4 namespace cleanup", flush=True)


if __name__ == "__main__":
    main()
