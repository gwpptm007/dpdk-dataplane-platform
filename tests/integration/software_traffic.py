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
    args = parser.parse_args()
    executable = args.build_dir.resolve() / "tests/integration/test_software_traffic"
    if not executable.is_file():
        parser.error("build with -Dtests=true and the rte_net_ring library installed")
    cpus = sorted(os.sched_getaffinity(0))
    if len(cpus) < 2:
        parser.error("two available CPUs are required")
    with tempfile.TemporaryDirectory(prefix="dppd-traffic-") as temporary:
        prefix = Path(temporary).name
        result = subprocess.run(
            [str(executable), f"--lcores=0@{cpus[0]},1@{cpus[1]}", "--main-lcore=0",
             "--no-huge", "--no-pci", "-m", "64", "--no-telemetry",
             f"--file-prefix={prefix}"],
            timeout=60, check=False,
        )
        if result.returncode != 0:
            raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
