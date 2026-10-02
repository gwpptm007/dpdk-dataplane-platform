#!/usr/bin/env python3
"""Send and verify bounded simultaneous UDP/TCP traffic through a PMD port pair."""

import argparse
import ipaddress
from pathlib import Path
import socket
import struct
import threading
import time

from rss_sender import frame, mac_address


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interface", required=True)
    parser.add_argument("--tx-destination-mac", type=mac_address, required=True)
    parser.add_argument("--rx-source-mac", type=mac_address, required=True)
    parser.add_argument("--rx-destination-mac", type=mac_address, required=True)
    for name in ("tx-source-ip", "tx-destination-ip", "rx-source-ip", "rx-destination-ip"):
        parser.add_argument(f"--{name}", type=ipaddress.IPv4Address, required=True)
    parser.add_argument("--protocol", choices=("udp", "tcp"), default="udp")
    parser.add_argument("--packets", type=int, default=30000)
    parser.add_argument("--flows", type=int, default=64)
    parser.add_argument("--rate", type=int, default=1000)
    parser.add_argument("--start-delay", type=float, default=5)
    args = parser.parse_args()
    if (not 1 <= args.packets <= 1000000 or not 1 <= args.flows <= 4096 or
            not 1 <= args.rate <= 10000 or not 0 <= args.start_delay <= 30 or
            args.packets / args.rate > 120):
        parser.error("invalid traffic bounds (maximum sending time is 120 seconds)")

    try:
        source_mac = mac_address(Path(f"/sys/class/net/{args.interface}/address").read_text().strip())
        seen = set()
        stop, complete = threading.Event(), threading.Event()
        failures = []
        offset = 54 if args.protocol == "tcp" else 42
        protocol_number = 6 if args.protocol == "tcp" else 17
        deadline = time.monotonic() + args.start_delay + args.packets / args.rate + 15
        with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)) as capture, \
                socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)) as sender:
            capture.bind((args.interface, 0))
            capture.settimeout(0.1)
            sender.bind((args.interface, 0))

            def receive():
                try:
                    while not stop.is_set() and time.monotonic() < deadline:
                        try:
                            packet, address = capture.recvfrom(65535)
                        except socket.timeout:
                            continue
                        if (address[2] == socket.PACKET_OUTGOING or len(packet) < offset + 20 or
                                packet[12:14] != b"\x08\x00" or packet[23] != protocol_number or
                                packet[offset:offset + 8] != b"DPPRSS01"):
                            continue
                        flow, sequence = struct.unpack("!IQ", packet[offset + 8:offset + 20])
                        if sequence >= args.packets or flow != sequence % args.flows or sequence in seen:
                            raise RuntimeError(f"unexpected or duplicate sequence={sequence} flow={flow}")
                        expected = frame(args.rx_source_mac, args.rx_destination_mac,
                                         args.rx_source_ip.packed, args.rx_destination_ip.packed,
                                         flow, sequence, args.protocol)
                        if packet != expected:
                            raise RuntimeError(f"received frame changed: sequence={sequence}")
                        seen.add(sequence)
                        if len(seen) == args.packets:
                            complete.set()
                    if len(seen) != args.packets:
                        raise RuntimeError(f"expected={args.packets} captured={len(seen)}")
                except (OSError, RuntimeError) as error:
                    failures.append(error)
                    complete.set()

            receiver = threading.Thread(target=receive)
            receiver.start()
            print(f"DUPLEX_READY interface={args.interface} protocol={args.protocol}", flush=True)
            try:
                time.sleep(args.start_delay)
                started = time.monotonic()
                for sequence in range(args.packets):
                    if failures:
                        raise failures[0]
                    delay = started + sequence / args.rate - time.monotonic()
                    if delay > 0:
                        time.sleep(delay)
                    packet = frame(source_mac, args.tx_destination_mac, args.tx_source_ip.packed,
                                   args.tx_destination_ip.packed, sequence % args.flows,
                                   sequence, args.protocol)
                    if sender.send(packet) != len(packet):
                        raise RuntimeError(f"short send: sequence={sequence}")
                if not complete.wait(max(0, deadline - time.monotonic())):
                    raise RuntimeError("receive deadline expired")
                time.sleep(1)
            finally:
                stop.set()
                receiver.join()
            if failures:
                raise failures[0]
        print(f"DUPLEX_PASS protocol={args.protocol} sent={args.packets} captured={len(seen)} "
              "duplicate=0 changed=0", flush=True)
    except (OSError, RuntimeError, argparse.ArgumentTypeError) as error:
        parser.exit(1, f"pmd_duplex_traffic: {error}\n")


if __name__ == "__main__":
    main()
