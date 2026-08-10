#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    printf 'usage: %s <ethdev-port-id> [output.pcapng]\n' "$0" >&2
    exit 2
fi

PORT_ID=$1
OUTPUT=${2:-"dppd-port-${PORT_ID}.pcapng"}

if ! command -v dpdk-dumpcap >/dev/null 2>&1; then
    printf '[dppd] dpdk-dumpcap is not available in PATH\n' >&2
    exit 1
fi

printf '[dppd] dppd must be running with --enable-pdump\n' >&2
exec dpdk-dumpcap -i "$PORT_ID" -w "$OUTPUT"
