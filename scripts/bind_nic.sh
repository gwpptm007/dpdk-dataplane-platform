#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
    printf 'usage: sudo %s <PCI-BDF> [PCI-BDF ...]\n' "$0" >&2
    exit 2
fi

DEV_BIND=${DPDK_DEVBIND:-dpdk-devbind.py}
if ! command -v "$DEV_BIND" >/dev/null 2>&1; then
    printf '[dppd] %s is not available in PATH\n' "$DEV_BIND" >&2
    exit 1
fi
if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
    printf '[dppd] NIC binding requires root\n' >&2
    exit 1
fi

modprobe vfio-pci
"$DEV_BIND" --status
printf '[dppd] binding to vfio-pci: %s\n' "$*"
"$DEV_BIND" --bind=vfio-pci "$@"
"$DEV_BIND" --status
