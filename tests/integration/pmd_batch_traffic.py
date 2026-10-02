#!/usr/bin/env python3
"""Exercise full-capacity software batch updates during external real PMD RX."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import socket
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ctl", type=Path, required=True)
    parser.add_argument("--socket", required=True)
    parser.add_argument("--state-path", type=Path, required=True)
    parser.add_argument("--interface", default="dppdsqout")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--peer", type=int, default=1)
    parser.add_argument("--rules", type=int, choices=(2, 4), required=True)
    parser.add_argument("--packets", type=int, default=10000)
    args = parser.parse_args()
    if not 1 <= args.packets <= 1000000:
        parser.error("packets must be 1..1000000")

    def ctl(*tokens, expected_error=None):
        result = subprocess.run([str(args.ctl), "--socket", args.socket, *map(str, tokens)],
                                capture_output=True, text=True, timeout=5)
        if expected_error is not None:
            if result.returncode == 0 or f"({expected_error})" not in result.stderr:
                raise RuntimeError(f"expected error {expected_error}: {result.stdout}{result.stderr}")
        elif result.returncode:
            raise RuntimeError(f"CLI failed: {result.stdout}{result.stderr}")
        return result.stdout

    def stats(port):
        return {name: int(value) for name, value in
                re.findall(r"(\w+)=(\d+)", ctl("stats", port, 0))}

    try:
        ids = [12001 + i for i in range(args.rules)]
        if "total=0" not in ctl("list"):
            raise RuntimeError("start with an empty rule repository")
        generations = []
        for index, rule_id in enumerate(ids):
            output = ctl("apply-drop", rule_id, args.port, 0, index, "software")
            generations.append(int(re.search(r"generation=(\d+)", output)[1]))
        initial = generations.copy()
        capacity_list = ctl("list")
        capacity_hash = hashlib.sha256(args.state_path.read_bytes()).digest()
        ctl("apply-drop", 12000, args.port, 0, 0, "software", expected_error=-28)
        if ctl("list") != capacity_list or hashlib.sha256(args.state_path.read_bytes()).digest() != capacity_hash:
            raise RuntimeError("capacity rejection changed repository or snapshot")
        begin, peer_begin = stats(args.port), stats(args.peer)
        with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)) as capture:
            capture.bind((args.interface, 0))
            capture.setblocking(False)

            def drain():
                while True:
                    try:
                        packet = capture.recv(65535)
                    except BlockingIOError:
                        return
                    if len(packet) >= 62 and packet[42:50] == b"DPPRSS01":
                        raise RuntimeError("DROP traffic leaked to the TAP peer")

            print(f"BATCH_TRAFFIC_READY rules={args.rules} packets={args.packets}", flush=True)
            deadline = time.monotonic() + 45
            while stats(args.port)["rx_packets"] == begin["rx_packets"]:
                drain()
                if time.monotonic() >= deadline:
                    raise RuntimeError("no external traffic")
                time.sleep(0.05)
            overlap = 0
            for iteration in range(50):
                prior_rx = stats(args.port)["rx_packets"]
                pairs = [value for pair in zip(ids, generations) for value in pair]
                output = ctl("update-drop-batch", args.port, 10 + iteration, "software", *pairs)
                rows = re.findall(r"rule=(\d+) generation=(\d+) transaction=(\d+) backend=(\d+)", output)
                if (len(rows) != args.rules or [int(row[0]) for row in rows] != ids or
                        any(int(row[3]) != 0 for row in rows) or len({row[2] for row in rows}) != 1):
                    raise RuntimeError(f"invalid software batch results: {output}")
                next_generations = [int(row[1]) for row in rows]
                if next_generations != list(range(max(generations) + 1, max(generations) + args.rules + 1)):
                    raise RuntimeError("generations did not advance as one complete batch")
                generations = next_generations
                time.sleep(0.1)
                drain()
                current_rx = stats(args.port)["rx_packets"]
                overlap += prior_rx < begin["rx_packets"] + args.packets and current_rx > prior_rx
            while stats(args.port)["rx_packets"] < begin["rx_packets"] + args.packets:
                drain()
                if time.monotonic() >= deadline:
                    raise RuntimeError("missing ingress packets")
                time.sleep(0.05)
            settle = time.monotonic() + 1
            while time.monotonic() < settle:
                drain()
                time.sleep(0.01)
            end, peer_end = stats(args.port), stats(args.peer)
            for name in ("rx_packets", "policy_drops", "rule_drops"):
                if end[name] - begin[name] != args.packets:
                    raise RuntimeError(f"unexpected {name} delta")
            if peer_end["tx_packets"] != peer_begin["tx_packets"] or overlap == 0:
                raise RuntimeError("egress leak or updates did not overlap ingress")
        before_list = ctl("list")
        before_hash = hashlib.sha256(args.state_path.read_bytes()).digest()
        pairs = [value for pair in zip(ids, initial) for value in pair]
        ctl("update-drop-batch", args.port, 999, "software", *pairs, expected_error=-116)
        if ctl("list") != before_list or hashlib.sha256(args.state_path.read_bytes()).digest() != before_hash:
            raise RuntimeError("stale batch modified repository or snapshot")
        persistence = ctl("persistence-status")
        if "dirty=no" not in persistence or f"persisted-generation={max(generations)} " not in persistence:
            raise RuntimeError("snapshot is not clean at the final generation")
        pairs = [value for pair in zip(ids, generations) for value in pair]
        ctl("delete-batch", *pairs)
        if "total=0" not in ctl("list") or "dirty=no" not in ctl("persistence-status"):
            raise RuntimeError("final deletion did not leave an empty clean repository")
        print(json.dumps({"rules": args.rules, "successful_batches": 50,
                          "batches_overlapping_rx": overlap, "rx_packets": args.packets,
                          "rule_drops": args.packets, "tap_tx_delta": 0,
                          "final_update_generations": generations,
                          "full_capacity": "ENOSPC, unchanged snapshot/repository",
                          "stale_batch": "ESTALE, unchanged snapshot/repository",
                          "final_repository": "empty, clean"}), flush=True)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"pmd_batch_traffic: {error}\n")


if __name__ == "__main__":
    main()
