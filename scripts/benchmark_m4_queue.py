#!/usr/bin/env python3
"""核对正式 M4 批量服务的排队到完整发布延迟，内存驱动不作为 ASIC 速率"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys

from verify_m2 import run_command, source_digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--report-dir", type=Path, required=True)
    parser.add_argument("--rules", type=int, choices=(4096, 16384), default=4096)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build, destination = args.build_dir.resolve(), args.report_dir.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    contract_bytes = (root / "docs/m4_acceptance_contract.json").read_bytes().replace(b"\r\n", b"\n")
    contract = json.loads(contract_bytes)
    report = {"schema_version": 1, "success": False, "hardware_acceptance": "not_run",
              "complete_m4_validation": False, "started_utc": datetime.now(timezone.utc).isoformat(),
              "scope": "production_async_batch_service_with_memory_driver",
              "source_sha256": source_digest(root), "contract_sha256": hashlib.sha256(contract_bytes).hexdigest(),
              "rules": args.rules, "repetitions": 5, "cpu_affinity": sorted(os.sched_getaffinity(0)), "runs": []}
    output = destination / "report.json"
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    try:
        if contract.get("revision") != 1 or report["cpu_affinity"] != [0, 1, 2, 3]:
            raise RuntimeError("固定队列门禁要求 revision 1 契约与 CPU 0-3")
        metadata = json.loads((build / "meson-info/meson-info.json").read_text())
        options = {item["name"]: item["value"] for item in json.loads(
            (build / "meson-info/intro-buildoptions.json").read_text())}
        dependencies = json.loads((build / "meson-info/intro-dependencies.json").read_text())
        if Path(metadata["directories"]["source"]).resolve() != root:
            raise RuntimeError("构建来自其他源码目录")
        if (not options.get("tests") or not options.get("werror") or options.get("b_ndebug") != "false" or
                options.get("buildtype") != "debugoptimized"):
            raise RuntimeError("固定门禁要求严格优化构建并保留断言")
        report["build_options"] = options
        report["dpdk"] = [item for item in dependencies if item.get("name") == "libdpdk"]
        if [item.get("version") for item in report["dpdk"]] != ["21.11.9"]:
            raise RuntimeError("固定门禁须使用参考 DPDK 21.11.9")
        report["compiler"] = subprocess.check_output(["cc", "--version"], text=True).splitlines()[0]
        if subprocess.check_output(["cc", "-dumpversion"], text=True).strip() != "11":
            raise RuntimeError("固定门禁须使用参考 GCC 11")
        command = ["meson", "compile", "-C", str(build), "benchmark_flow_queue"]
        with (destination / "compile.log").open("w") as log:
            if run_command(command, root, log, 180) != 0:
                raise RuntimeError("队列基准编译失败")
        executable = build / "tests/bench/benchmark_flow_queue"
        report["binary_sha256"] = hashlib.sha256(executable.read_bytes()).hexdigest()
        report["command"] = [str(executable), str(args.rules)]
        # 每轮都必须保留，任一失败都会终止验收，不能挑选更好的五个样本
        for index in range(5):
            path = destination / f"run-{index}.log"
            with path.open("w") as log:
                if run_command(report["command"], root, log, 180) != 0:
                    raise RuntimeError(f"第{index + 1}轮失败，不能丢弃该轮")
            result = json.loads(path.read_text())
            expected = {"hardware_acceptance": "not_run", "path": "memory_driver_async_batch_service",
                        "rules": args.rules, "batch": 64, "queues": 8, "queue_depth": 256,
                        "window_rules": 2048, "samples": args.rules // 64}
            if any(result.get(key) != value for key, value in expected.items()):
                raise RuntimeError("队列、窗口、批量或完整发布范围与冻结条件不同")
            if result.get("completion_p99_ns", 0) <= 0 or result.get("install_ns", 0) <= 0:
                raise RuntimeError("队列基准返回无效计时")
            report["runs"].append(result)
        report["median_completion_p99_ns"] = statistics.median(run["completion_p99_ns"] for run in report["runs"])
        report["median_install_ns"] = statistics.median(run["install_ns"] for run in report["runs"])
        if args.rules == 4096:
            gate = "queue_4096_batch64_max_completion_p99_ns"
            report["performance_gates"] = {gate: report["median_completion_p99_ns"] <= contract["performance_gates"][gate]}
            if not all(report["performance_gates"].values()):
                raise RuntimeError("固定队列 P99 门槛未通过，须修复而不能降低标准")
        else:
            report["performance_gates"] = {}
            report["scale_acceptance"] = "16384_complete_batches_published_and_deleted"
        if (source_digest(root) != report["source_sha256"] or
                hashlib.sha256(executable.read_bytes()).hexdigest() != report["binary_sha256"]):
            raise RuntimeError("测量期间源码或二进制改变")
        report["success"] = True
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
        print(str(error), file=sys.stderr)
    finally:
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(str(output))
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
