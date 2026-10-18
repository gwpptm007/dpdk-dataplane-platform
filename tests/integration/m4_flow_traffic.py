#!/usr/bin/env python3
"""通过真实 net_ring、mbuf 和正式 worker 验收 M4 批量发布，不接触实体端口"""

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
    destination = args.report_dir.resolve() if args.report_dir else build / "m4-flow-traffic"
    destination.mkdir(parents=True, exist_ok=True)
    executable = build / "tests/integration/test_m4_flow_traffic"
    report = {"schema_version": 1, "scope": "production_M4_batch_service_net_ring_worker_packets",
              "hardware_acceptance": "not_run", "complete_m4_validation": False, "success": False,
              "source_sha256": source_digest(root), "started_utc": datetime.now(timezone.utc).isoformat()}
    output = destination / "report.json"
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    try:
        cpus = sorted(os.sched_getaffinity(0))
        if len(cpus) < 2 or not executable.is_file():
            raise RuntimeError("需要两个可用 CPU 及开启 net_ring 的严格测试构建")
        report["binary_sha256"] = hashlib.sha256(executable.read_bytes()).hexdigest()
        mapping = f"0@{cpus[0]},1@{cpus[1]}"
        with tempfile.TemporaryDirectory(prefix="dppd-m4-traffic-") as directory:
            command = [str(executable), f"--lcores={mapping}", "--main-lcore=0", "--no-huge", "--no-pci",
                       "-m", "64", "--no-telemetry", f"--file-prefix={Path(directory).name}"]
            report["command"] = command
            with (destination / "traffic.log").open("w") as log:
                report["returncode"] = run_command(command, root, log, 60)
        text = (destination / "traffic.log").read_text()
        expected = "PASS M4 worker rx=272 forwarded=200 dropped=72"
        if report["returncode"] != 0 or expected not in text:
            raise RuntimeError("M4 真实报文、统计或进程清理未通过")
        if not all(token in text for token in ("atomic-create=passed", "atomic-delete=passed",
                   "atomic-update=passed", "update-count-reset=passed", "allocation-rollback=passed",
                   "MARK=42 COUNT=32/2048", "mbuf-leaks=0", "hardware=not_run")):
            raise RuntimeError("缺少批量发布、回滚、COUNT 或资源回收证据")
        if "native-rejection=-95" not in text and "native-rejection=-38" not in text:
            raise RuntimeError("真实 net_ring 未明确拒绝原生模板配置")
        report["packets"] = {"received": 272, "forwarded": 200, "dropped": 72}
        if source_digest(root) != report["source_sha256"]:
            raise RuntimeError("测试期间源码改变")
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
