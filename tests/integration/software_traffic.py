#!/usr/bin/env python3
"""Run real net_ring RX/TX through the production worker during software batch updates."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--queues", type=int, choices=(1, 2), default=2,
                        help="maximum queue count; 2 also runs the single-queue baseline")
    args = parser.parse_args()
    executable = args.build_dir.resolve() / "tests/integration/test_software_traffic"
    if not executable.is_file():
        parser.error("build with -Dtests=true and the rte_net_ring library installed")
    cpus = sorted(os.sched_getaffinity(0))
    if len(cpus) < args.queues + 1:
        parser.error(f"{args.queues + 1} available CPUs are required")
    mapping = ",".join(f"{index}@{cpu}" for index, cpu in enumerate(cpus[:args.queues + 1]))
    with tempfile.TemporaryDirectory(prefix="dppd-traffic-") as temporary:
        prefix = Path(temporary).name
        result = subprocess.run(
            [str(executable), f"--lcores={mapping}", "--main-lcore=0",
             "--no-huge", "--no-pci", "-m", "64", "--no-telemetry",
             f"--file-prefix={prefix}"],
            timeout=60, check=False,
        )
        if result.returncode != 0:
            raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
