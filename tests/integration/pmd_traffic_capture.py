#!/usr/bin/env python3
"""Verify unchanged UDP frames at the TAP peer of an external PMD ingress."""

import argparse
import socket
import struct
import time

from rss_sender import frame, mac_address


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interface", required=True)
    parser.add_argument("--source-mac", required=True, type=mac_address)
    parser.add_argument("--destination-mac", required=True, type=mac_address)
    parser.add_argument("--packets", type=int, default=256)
    parser.add_argument("--flows", type=int, default=64)
    parser.add_argument("--duration", type=float, default=6)
    parser.add_argument("--expect-drop", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.packets <= 1000000 or not 1 <= args.flows <= 4096 or not 1 <= args.duration <= 60:
        parser.error("invalid packet count, flow count or capture duration")
    seen = set()
    try:
        with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)) as capture:
            capture.bind((args.interface, 0))
            capture.settimeout(0.1)
            print("CAPTURE_READY", flush=True)
            deadline = time.monotonic() + args.duration
            while time.monotonic() < deadline:
                try:
                    packet = capture.recv(65535)
                except socket.timeout:
                    continue
                if len(packet) < 62 or packet[42:50] != b"DPPRSS01":
                    continue
                flow, sequence = struct.unpack("!IQ", packet[50:62])
                if args.expect_drop:
                    raise RuntimeError(f"dropped traffic leaked to TAP: sequence={sequence}")
                if sequence >= args.packets or flow != sequence % args.flows or sequence in seen:
                    raise RuntimeError(f"unexpected or duplicate sequence={sequence} flow={flow}")
                expected = frame(args.source_mac, args.destination_mac,
                                 socket.inet_aton("192.168.100.2"),
                                 socket.inet_aton("192.168.100.1"), flow, sequence)
                if packet != expected:
                    raise RuntimeError(f"frame changed: sequence={sequence}")
                seen.add(sequence)
        expected_count = 0 if args.expect_drop else args.packets
        if len(seen) != expected_count:
            raise RuntimeError(f"expected={expected_count} captured={len(seen)}")
        print(f"CAPTURE_PASS expected={expected_count} captured={len(seen)} duplicate=0 changed=0")
    except (OSError, RuntimeError) as error:
        parser.exit(1, f"pmd_traffic_capture: {error}\n")


if __name__ == "__main__":
    main()
