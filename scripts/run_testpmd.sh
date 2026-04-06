#!/usr/bin/env bash
set -euo pipefail
cat <<'MSG'
[dppd] example modern baseline steps
- verify DPDK installation
- run dpdk-testpmd with explicit lcores/queues
- record RSS, queue, and stats behavior
- compare with this repository's userspace dataplane skeleton
MSG
