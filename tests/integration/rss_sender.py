#!/usr/bin/env python3
"""Send bounded IPv4 UDP/TCP flows to an explicitly verified data NIC on Linux."""

import argparse
import ipaddress
import socket
import struct
import time


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return (~total) & 0xffff


def mac_address(value):
    try:
        parts = value.split(":")
        if len(parts) != 6 or any(len(part) != 2 for part in parts):
            raise ValueError
        result = bytes(int(part, 16) for part in parts)
        if result == bytes(6) or result[0] & 1:
            raise ValueError
        return result
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected a nonzero unicast MAC address") from error


def frame(source_mac, destination_mac, source_ip, destination_ip, flow, sequence, protocol="udp"):
    payload = b"DPPRSS01" + struct.pack("!IQ", flow, sequence)
    udp_length = 8 + len(payload)
    udp = struct.pack("!HHHH", 20000 + flow, 10000, udp_length, 0)
    pseudo = source_ip + destination_ip + struct.pack("!BBH", 0, 17, udp_length)
    udp = udp[:6] + struct.pack("!H", checksum(pseudo + udp + payload) or 0xffff)
    transport, protocol_number = udp, 17
    if protocol == "tcp":
        transport = struct.pack("!HHIIBBHHH", 20000 + flow, 10000,
                                sequence & 0xffffffff, 0, 0x50, 0x10, 4096, 0, 0)
        pseudo = source_ip + destination_ip + struct.pack("!BBH", 0, 6, len(transport) + len(payload))
        transport = transport[:16] + struct.pack("!H", checksum(pseudo + transport + payload)) + transport[18:]
        protocol_number = 6
    header = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(transport) + len(payload),
                         sequence & 0xffff, 0, 64, protocol_number, 0, source_ip, destination_ip)
    header = header[:10] + struct.pack("!H", checksum(header)) + header[12:]
    return destination_mac + source_mac + b"\x08\x00" + header + transport + payload


def probe(sock, source_mac, destination_mac, source_ip, destination_ip):
    arp = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 1)
    arp += source_mac + source_ip + destination_mac + destination_ip
    sock.settimeout(0.2)
    for _ in range(3):
        sock.send(destination_mac + source_mac + b"\x08\x06" + arp)
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            try:
                data = sock.recv(2048)
            except socket.timeout:
                continue
            if (len(data) >= 42 and data[:6] == source_mac and
                    data[6:12] == destination_mac and data[12:14] == b"\x08\x06" and
                    data[14:22] == struct.pack("!HHBBH", 1, 0x0800, 6, 4, 2) and
                    data[22:28] == destination_mac and data[28:32] == destination_ip and
                    data[32:38] == source_mac and data[38:42] == source_ip):
                return True
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interface", required=True)
    parser.add_argument("--destination-mac", required=True, type=mac_address)
    parser.add_argument("--source-ip", type=ipaddress.IPv4Address, default="192.168.100.2")
    parser.add_argument("--destination-ip", type=ipaddress.IPv4Address, default="192.168.100.1")
    parser.add_argument("--flows", type=int, default=64)
    parser.add_argument("--protocol", choices=("udp", "tcp"), default="udp")
    parser.add_argument("--vary-source-ip", action="store_true",
                        help="increment source IPv4 per flow for PMDs supporting IPv4 RSS only")
    parser.add_argument("--packets", type=int, default=4096)
    parser.add_argument("--rate", type=int, default=1000, help="packets per second")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--probe-only", action="store_true")
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--receiver-dpdk", action="store_true",
                      help="skip ARP only after the Linux data NIC probe passed and DPDK took ownership")
    args = parser.parse_args()
    if not 1 <= args.flows <= 4096 or not 1 <= args.packets <= 1000000 or not 1 <= args.rate <= 10000:
        parser.error("flows: 1..4096; packets: 1..1000000; rate: 1..10000")
    if args.vary_source_ip and int(args.source_ip) + args.flows - 1 > 0xffffffff:
        parser.error("source IPv4 range exceeds 255.255.255.255")
    try:
        socket.if_nametoindex(args.interface)
        with open(f"/sys/class/net/{args.interface}/address", encoding="ascii") as source:
            source_mac = mac_address(source.read().strip())
        source_ip, destination_ip = args.source_ip.packed, args.destination_ip.packed
        if args.dry_run:
            sample = frame(source_mac, args.destination_mac, source_ip, destination_ip, 0, 0, args.protocol)
            print(f"DRY_RUN interface={args.interface} flows={args.flows} packets={args.packets} frame_bytes={len(sample)}")
            return
        with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)) as sock:
            sock.bind((args.interface, 0))
            if not args.receiver_dpdk:
                if not probe(sock, source_mac, args.destination_mac, source_ip, destination_ip):
                    raise RuntimeError("data NIC did not answer directed ARP; check the VMware network")
                print("DATA_NIC_REACHABLE", flush=True)
            if args.probe_only:
                return
            start = time.monotonic()
            for sequence in range(args.packets):
                delay = start + sequence / args.rate - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                flow = sequence % args.flows
                packet_source = (ipaddress.IPv4Address(int(args.source_ip) + flow).packed
                                 if args.vary_source_ip else source_ip)
                packet = frame(source_mac, args.destination_mac, packet_source, destination_ip,
                               flow, sequence, args.protocol)
                if sock.send(packet) != len(packet):
                    raise RuntimeError(f"short send at sequence {sequence}")
            elapsed = time.monotonic() - start
            print(f"SENT packets={args.packets} flows={args.flows} elapsed={elapsed:.3f}s")
            print("SENT is local submission; verify receiver queue counts and loss separately")
    except (OSError, RuntimeError, argparse.ArgumentTypeError) as error:
        parser.exit(1, f"rss_sender: {error}\n")


if __name__ == "__main__":
    main()
