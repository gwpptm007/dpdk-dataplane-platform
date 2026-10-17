#!/usr/bin/env python3
"""用真实 net_ring 和生产 worker 检查 IPv4 多出口、报文改写与在线路由更新"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    arguments = parser.parse_args()
    cpus = sorted(os.sched_getaffinity(0))
    if len(cpus) < 3:
        parser.error("IPv4 单双队列验收需要三个可用 CPU")
    mapping = ",".join(f"{index}@{cpu}" for index, cpu in enumerate(cpus[:3]))
    executable = arguments.build_dir.resolve() / "tests/integration/test_ipv4_traffic"
    with tempfile.TemporaryDirectory(prefix="dppd-ipv4-traffic-") as temporary:
        command = [str(executable), f"--lcores={mapping}", "--main-lcore=0", "--no-huge", "--no-pci", "-m", "64",
                   "--no-telemetry", f"--file-prefix={Path(temporary).name}"]
        result = subprocess.run(command, timeout=60, check=False)
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
