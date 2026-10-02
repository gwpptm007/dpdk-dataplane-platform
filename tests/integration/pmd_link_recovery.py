#!/usr/bin/env python3
"""Verify TAP link recovery while receiving external traffic through a real PMD."""

import argparse
import errno
import hashlib
import json
from pathlib import Path
import re
import socket
import struct
import subprocess
import time

from rss_sender import frame, mac_address


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ctl", type=Path, required=True)
    parser.add_argument("--socket", required=True)
    parser.add_argument("--state-path", type=Path, required=True)
    parser.add_argument("--interface", default="dppdsqout")
    parser.add_argument("--source-mac", type=mac_address, required=True)
    parser.add_argument("--destination-mac", type=mac_address, required=True)
    parser.add_argument("--packets", type=int, default=64)
    args = parser.parse_args()
    if not 1 <= args.packets <= 4096:
        parser.error("packets must be 1..4096")
    flap_started = False

    def ctl(*tokens):
        result = subprocess.run([str(args.ctl), "--socket", args.socket, *map(str, tokens)],
                                capture_output=True, text=True, timeout=5, check=True)
        return result.stdout

    def stats(port):
        return {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", ctl("stats", port, 0))}

    def link(up):
        nonlocal flap_started
        subprocess.run(["ip", "link", "set", "dev", args.interface, "up" if up else "down"], check=True)
        flap_started = True
        deadline = time.monotonic() + 5
        expected = f"link={'up' if up else 'down'} "
        while expected not in ctl("port-show", 1):
            if time.monotonic() >= deadline:
                raise RuntimeError(f"daemon did not observe {expected}")
            time.sleep(0.02)

    try:
        if "total=0" not in ctl("list") or "driver=net_tap " not in ctl("port-show", 1):
            raise RuntimeError("requires an empty daemon with TAP port 1")
        if "link=up " not in ctl("port-show", 0) or "link=up " not in ctl("port-show", 1):
            raise RuntimeError("both ports must initially be up")
        output = ctl("apply-filter", 14001, 0, 0, "tcp", "192.168.100.2/32",
                     "192.168.100.1/32", "any", 10000, "drop", "count", "software")
        generation = int(re.search(r"generation=(\d+)", output)[1])
        saved_rule = ctl("get", 14001)
        saved_hash = hashlib.sha256(args.state_path.read_bytes()).digest()
        with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)) as capture:
            capture.bind((args.interface, 0))
            capture.setblocking(False)

            def phase(name, protocol="udp", reason=None):
                before, peer_before = stats(0), stats(1)
                seen = set()
                offset = 54 if protocol == "tcp" else 42
                protocol_number = 6 if protocol == "tcp" else 17

                def drain():
                    while True:
                        try:
                            packet = capture.recv(65535)
                        except BlockingIOError:
                            return
                        except OSError as error:
                            if reason == "egress" and error.errno == errno.ENETDOWN:
                                return
                            raise
                        if (len(packet) < offset + 20 or packet[12:14] != b"\x08\x00" or
                                packet[23] != protocol_number or packet[offset:offset + 8] != b"DPPRSS01"):
                            continue
                        flow, sequence = struct.unpack("!IQ", packet[offset + 8:offset + 20])
                        if reason is not None:
                            raise RuntimeError(f"{name}: dropped packet leaked: sequence={sequence}")
                        if sequence >= args.packets or flow != sequence % 64 or sequence in seen:
                            raise RuntimeError(f"{name}: invalid or duplicate sequence={sequence}")
                        expected = frame(args.source_mac, args.destination_mac,
                                         socket.inet_aton("192.168.100.2"), socket.inet_aton("192.168.100.1"),
                                         flow, sequence, protocol)
                        if packet != expected:
                            raise RuntimeError(f"{name}: frame changed: sequence={sequence}")
                        seen.add(sequence)

                print(f"LINK_READY phase={name} protocol={protocol}", flush=True)
                deadline = time.monotonic() + 45
                while stats(0)["rx_packets"] < before["rx_packets"] + args.packets:
                    drain()
                    if time.monotonic() >= deadline:
                        raise RuntimeError(f"{name}: missing ingress packets")
                    time.sleep(0.02)
                settle = time.monotonic() + 1
                while time.monotonic() < settle:
                    drain()
                    time.sleep(0.01)
                after, peer_after = stats(0), stats(1)
                dropped = args.packets if reason is not None else 0
                expected = {"rx_packets": args.packets,
                            "rx_bytes": args.packets * (74 if protocol == "tcp" else 62),
                            "policy_drops": dropped, "rule_drops": args.packets if reason == "rule" else 0,
                            "egress_drops": args.packets if reason == "egress" else 0}
                for key, value in expected.items():
                    if after[key] - before[key] != value:
                        raise RuntimeError(f"{name}: wrong {key} delta")
                if (peer_after["tx_packets"] - peer_before["tx_packets"] != args.packets - dropped or
                        peer_after["tx_bytes"] - peer_before["tx_bytes"] !=
                        (args.packets - dropped) * (74 if protocol == "tcp" else 62) or
                        peer_after["tx_drops"] != peer_before["tx_drops"] or
                        len(seen) != args.packets - dropped):
                    raise RuntimeError(f"{name}: output statistics/capture mismatch")
                print(f"LINK_PASS phase={name} rx={args.packets} forwarded={len(seen)} dropped={dropped}", flush=True)

            phase("initial-policy", "tcp", "rule")
            phase("baseline")
            for iteration in (1, 2):
                link(False)
                phase(f"down{iteration}", reason="egress")
                link(True)
                phase(f"up{iteration}")
                if ctl("get", 14001) != saved_rule or hashlib.sha256(args.state_path.read_bytes()).digest() != saved_hash:
                    raise RuntimeError("link flap changed rule or snapshot")
                if f"hits={args.packets} bytes={args.packets * 74}" not in ctl("count", 14001, generation):
                    raise RuntimeError("link flap changed existing TCP count")
            phase("policy", "tcp", "rule")
        if f"hits={args.packets * 2} bytes={args.packets * 148}" not in ctl("count", 14001, generation):
            raise RuntimeError("TCP count is incorrect after recovery")
        if ctl("get", 14001) != saved_rule or hashlib.sha256(args.state_path.read_bytes()).digest() != saved_hash:
            raise RuntimeError("recovered policy changed rule or snapshot")
        ctl("delete", 14001, generation)
        if "total=0" not in ctl("list") or "dirty=no" not in ctl("persistence-status"):
            raise RuntimeError("final repository is not empty and clean")
        print(json.dumps({"link_cycles": 2, "rx_packets": args.packets * 7,
                          "forwarded": args.packets * 3, "egress_drops": args.packets * 2,
                          "rule_drops": args.packets * 2, "existing_count_preserved": True,
                          "rule_and_snapshot_preserved": True,
                          "final_repository": "empty, clean"}), flush=True)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"pmd_link_recovery: {error}\n")
    finally:
        if flap_started:
            subprocess.run(["ip", "link", "set", "dev", args.interface, "up"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)


if __name__ == "__main__":
    main()
