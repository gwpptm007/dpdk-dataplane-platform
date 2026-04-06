#!/usr/bin/env bash
set -euo pipefail
cat <<'MSG'
[dppd] modern DPDK NIC binding checklist
1. confirm IOMMU / VFIO support on host
2. load vfio-pci if needed
3. inspect devices with dpdk-devbind.py --status
4. bind the target NIC to vfio-pci
5. verify hugepages before running the dataplane app

This repository does not auto-bind NICs by default.
MSG
