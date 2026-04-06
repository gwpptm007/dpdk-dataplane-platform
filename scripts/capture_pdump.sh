#!/usr/bin/env bash
set -euo pipefail
cat <<'MSG'
[dppd] capture guidance
- in real DPDK environments, use dpdk-pdump when supported
- pair packet capture with Scapy-generated test traffic
- validate parsed headers and rewrite behavior in Wireshark
MSG
