#!/usr/bin/env python3
"""重复测量正式控制面的软件开销，基准不访问实体网卡，也不作为硬件安装速率"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import sys

from verify_m2 import run_command, source_digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--rules", type=int, default=4096)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--report-dir", type=Path)
    args = parser.parse_args()
    if args.rules < 64 or args.rules > 65536 or args.rules % 64 or not 3 <= args.repetitions <= 15:
        parser.error("规则数须为64的倍数且不超过65536，重复次数须为3到15")
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    destination = args.report_dir.resolve() if args.report_dir else build / "m4-benchmark"
    destination.mkdir(parents=True, exist_ok=True)
    report = {"schema_version": 1, "started_utc": datetime.now(timezone.utc).isoformat(),
              "scope": "production_control_with_memory_driver_and_software_backend",
              "hardware_acceptance": "not_run", "complete_m4_validation": False, "success": False,
              "source_sha256": source_digest(root), "rules": args.rules, "repetitions": args.repetitions,
              "runs": []}
    output = destination / "report.json"
    # 先撤销旧通过状态，中断或失败也不能留下看起来属于本轮的成功报告
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    try:
        metadata = json.loads((build / "meson-info/meson-info.json").read_text())
        options = {item["name"]: item["value"] for item in json.loads(
            (build / "meson-info/intro-buildoptions.json").read_text())}
        if Path(metadata["directories"]["source"]).resolve() != root:
            raise RuntimeError("构建来自其他源码目录")
        if (not options.get("tests") or not options.get("werror") or options.get("b_ndebug") != "false" or
                options.get("buildtype") != "debugoptimized"):
            raise RuntimeError("基准要求tests/werror=true、b_ndebug=false和debugoptimized，禁止关闭断言")
        report["build_options"] = options
        report["dpdk"] = [item for item in json.loads((build / "meson-info/intro-dependencies.json").read_text())
                          if item.get("name") == "libdpdk"]
        command = ["meson", "compile", "-C", str(build), "benchmark_control_scale"]
        report["compile_command"] = command
        with (destination / "compile.log").open("w") as log:
            if run_command(command, root, log, 180) != 0:
                raise RuntimeError("基准编译失败")
        executable = build / "tests/bench/benchmark_control_scale"
        report["binary_sha256"] = hashlib.sha256(executable.read_bytes()).hexdigest()
        command = [str(executable), str(args.rules)]
        report["command"] = command
        for index in range(args.repetitions):
            path = destination / f"run-{index}.log"
            with path.open("w") as log:
                if run_command(command, root, log, 180) != 0:
                    raise RuntimeError(f"第{index + 1}次基准失败，不能丢弃该样本")
            result = json.loads(path.read_text())
            cases = result["cases"]
            expected = [("memory_driver", args.rules, 1), ("memory_driver", args.rules, 64),
                        ("software", min(args.rules, 1024), 1)]
            if (result["hardware_acceptance"] != "not_run" or len(cases) != 3 or
                    [(case["path"], case["rules"], case["batch"]) for case in cases] != expected):
                raise RuntimeError("基准返回了不同规模或后端，不能与已固定契约比较")
            for case in cases:
                if any(case[key] <= 0 for key in ("install_ns", "rules_per_second", "request_p99_ns", "remove_ns")):
                    raise RuntimeError("基准包含无效计时")
            report["runs"].append(result)
        # 每轮都保留，汇总只取中位数，不从多次运行里挑最好的一次
        report["median_cases"] = []
        for index, case in enumerate(report["runs"][0]["cases"]):
            summary = {key: case[key] for key in ("path", "rules", "batch")}
            for key in ("install_ns", "rules_per_second", "request_p99_ns", "remove_ns"):
                summary[key] = statistics.median(run["cases"][index][key] for run in report["runs"])
            report["median_cases"].append(summary)
        report["median_residual_cleanup_ns"] = statistics.median(
            run["inprocess_residual_cleanup_ns"] for run in report["runs"])
        if source_digest(root) != report["source_sha256"]:
            raise RuntimeError("基准期间源码改变")
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
