#!/usr/bin/env python3
"""两个全新 EAL 进程在正式 worker 上收发，恢复只使用磁盘配置"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from verify_m2 import run_command, source_digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--report-dir", type=Path)
    args = parser.parse_args()
    root, build = Path(__file__).resolve().parents[2], args.build_dir.resolve()
    destination = args.report_dir.resolve() if args.report_dir else build / "m5-flow-restart"
    destination.mkdir(parents=True, exist_ok=True)
    executable = build / "tests/integration/test_m5_flow_restart"
    report = {"success": False, "scope": "M5_configuration_fresh_process_real_net_ring_worker",
              "hardware_acceptance": "not_run", "source_sha256": source_digest(root), "stages": [],
              "started_utc": datetime.now(timezone.utc).isoformat()}
    output = destination / "report.json"
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    try:
        cpus = sorted(os.sched_getaffinity(0))
        if len(cpus) < 2:
            raise RuntimeError("需要两个可用 CPU")
        report["binary_sha256"] = hashlib.sha256(executable.read_bytes()).hexdigest()
        with tempfile.TemporaryDirectory(prefix="dppd-m5-restart-") as directory:
            state = Path(directory) / "state"
            digest = None
            for stage in ("before", "after"):
                command = [str(executable), f"--lcores=0@{cpus[0]},1@{cpus[1]}", "--main-lcore=0", "--no-huge",
                           "--no-pci", "--no-shconf", "-m", "64", "--no-telemetry",
                           f"--file-prefix={Path(directory).name}-{stage}", "--", str(state), stage]
                log = destination / (stage + ".log")
                with log.open("w") as stream:
                    rc = run_command(command, root, stream, 90)
                text = log.read_text()
                if rc != 0 or f"PASS M5 {stage}" not in text or "mbuf-leaks=0" not in text:
                    raise RuntimeError(f"{stage} 真实收发或退出失败")
                if stage == "after" and "COUNT-reset=0" not in text:
                    raise RuntimeError("新进程未验证 COUNT 重置")
                current = hashlib.sha256(state.read_bytes()).hexdigest()
                if digest is not None and current != digest:
                    raise RuntimeError("退出清理错误覆盖了持久期望状态")
                digest = current
                report["stages"].append({"name": stage, "command": command, "returncode": rc,
                                         "state_sha256": current})
        if source_digest(root) != report["source_sha256"] or hashlib.sha256(executable.read_bytes()).hexdigest() != report["binary_sha256"]:
            raise RuntimeError("验收期间源码或二进制改变")
        report["packets"] = {"received": 96, "forwarded": 48, "dropped": 48}
        report["success"] = True
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
        print(str(error), file=sys.stderr)
    finally:
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(str(output))
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
