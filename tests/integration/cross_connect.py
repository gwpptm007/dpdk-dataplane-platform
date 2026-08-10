#!/usr/bin/env python3
import os
import secrets
import sys
import time

from scapy.all import AsyncSniffer, Ether, IP, Raw, UDP, sendp


def main() -> int:
    if os.geteuid() != 0 or len(sys.argv) != 3:
        print(f"usage: sudo {sys.argv[0]} <tx-iface> <rx-iface>", file=sys.stderr)
        return 2

    tx_iface, rx_iface = sys.argv[1:]
    marker = secrets.token_bytes(16)
    packet = (
        Ether(dst="02:00:00:00:00:02", src="02:00:00:00:00:01")
        / IP(src="192.0.2.1", dst="198.51.100.2")
        / UDP(sport=12345, dport=23456)
        / Raw(marker)
    )

    sniffer = AsyncSniffer(
        iface=rx_iface,
        store=True,
        lfilter=lambda pkt: Raw in pkt and bytes(pkt[Raw].load) == marker,
    )
    sniffer.start()
    time.sleep(0.2)
    sendp(packet, iface=tx_iface, count=1, verbose=False)
    time.sleep(1.0)
    captured = sniffer.stop()
    if len(captured) != 1:
        print(f"FAIL: expected one matching packet, captured {len(captured)}")
        return 1
    if bytes(captured[0]) != bytes(packet):
        print("FAIL: forwarded packet content changed")
        return 1
    print("PASS: one byte-identical packet crossed the dppd port pair")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
