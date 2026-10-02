#!/usr/bin/env python3
"""Compare Linux RX queue counters using 64 ordinary TCP connections."""

import argparse
import json
import re
import socket
import struct
import subprocess
import time


def counters(interface):
    text = subprocess.check_output(["ethtool", "-S", interface], text=True)
    result, queue = {}, None
    for line in text.splitlines():
        match = re.search(r"Rx Queue#: (\d+)", line)
        if match:
            queue = int(match[1])
        elif queue is not None and "ucast pkts rx:" in line:
            result[queue] = int(line.rsplit(":", 1)[1])
    if not result:
        raise RuntimeError("vmxnet3 RX unicast counters were not found")
    return result


def records(flow):
    return b"".join(b"DPPRSS01" + struct.pack("!IQ", flow, flow * 64 + i)
                    for i in range(64))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("receive", "send"))
    parser.add_argument("--interface", default="ens192")
    parser.add_argument("--receiver-ip", default="192.168.100.1")
    parser.add_argument("--sender-ip", default="192.168.100.2")
    parser.add_argument("--port", type=int, default=10001)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be 1..65535")
    try:
        if args.mode == "send":
            for flow in range(64):
                with socket.socket() as stream:
                    stream.settimeout(10)
                    stream.bind((args.sender_ip, 30000 + flow))
                    stream.connect((args.receiver_ip, args.port))
                    stream.sendall(records(flow))
            print("KERNEL_TCP_SENT connections=64 records=4096")
            return
        before = counters(args.interface)
        with socket.socket() as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind((args.receiver_ip, args.port))
            listener.listen(64)
            deadline = time.monotonic() + 60
            print("KERNEL_TCP_READY", flush=True)
            for flow in range(64):
                listener.settimeout(max(0.001, deadline - time.monotonic()))
                stream, peer = listener.accept()
                with stream:
                    if peer != (args.sender_ip, 30000 + flow):
                        raise RuntimeError(f"unexpected peer for flow {flow}: {peer}")
                    data = b""
                    while len(data) < 1280:
                        stream.settimeout(max(0.001, deadline - time.monotonic()))
                        part = stream.recv(1280 - len(data))
                        if not part:
                            raise RuntimeError(f"short stream for flow {flow}")
                        data += part
                    if data != records(flow):
                        raise RuntimeError(f"invalid records for flow {flow}")
        after = counters(args.interface)
        if before.keys() != after.keys() or any(after[q] < before[q] for q in before):
            raise RuntimeError("RX counters reset or queue configuration changed")
        delta = {q: after[q] - before[q] for q in before}
        active = sum(value > 0 for value in delta.values())
        print(json.dumps({"connections": 64, "records": 4096, "content_valid": True,
                          "rx_unicast_delta": delta, "active_rx_queues": active}))
        if active < 2:
            parser.exit(1, "kernel RSS comparison failed: only one RX queue received traffic\n")
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"rss_kernel_probe: {error}\n")


if __name__ == "__main__":
    main()
