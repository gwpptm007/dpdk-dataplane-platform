#!/usr/bin/env python3
"""正式业务退出后的受控交接，验证争抢、中断和全部只读边界"""

import argparse
import os
from pathlib import Path

from batch_update import check
from monitor_handshake import run_case


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--native", action="store_true")
    arguments = parser.parse_args()
    check(os.geteuid() == 0, "handoff tests require root for test-owned TAPs")
    for mode in ("live", "wrong-session", "paused", "lost", "stopped", "replaced", "renamed", "contention", "interrupted"):
        run_case(arguments.build_dir.resolve(), "handoff-" + mode, arguments.native)


if __name__ == "__main__":
    main()
