#!/usr/bin/env python3
"""统一验收 M4 软件交付，物理卸载及设备复位始终单列为未验收"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from benchmark_m4 import evaluate_gates
from verify_m2 import run_command, source_digest


CONTRACT_SHA256 = "39504708d1d46102a4bf655fed56d5da41db0bafffe7a64ea7bac7d82a383aef"
SCOPE = "m4_current_environment_software"
FEATURE_TESTS = (
    "atomic global and per-port resource admission",
    "stable slot index ordering reuse and allocation failure",
    "template masks leases quotas and partial cleanup",
    "native DPDK template API and version contract",
    "bounded asynchronous flow lifecycle and completion identity",
    "native asynchronous enqueue push pull and retained storage",
    "atomic software create replace delete and allocation failures",
    "M4 asynchronous batch publication and exact restoration",
    "M4 queue AGE identity quotas maintenance and retry",
    "M4 shared COUNT quota leases concurrency and residual cleanup",
    "native shared COUNT template queue AGE and compensation contract",
)


def matching(report, source):
    if report.get("success") is not True or report.get("source_sha256") != source:
        raise ValueError("证据未通过或与本轮源码不同")


def complete_software(source, contract, profiles, tap, benchmarks):
    """逐项核对完整证据，单个功能成功不能提升为整个 M4 已完成"""
    modern = sanitizer = reference = False
    for profile in profiles:
        matching(profile, source)
        options = profile["build_options"]
        if (profile.get("scope") != SCOPE or profile.get("hardware_acceptance") != "not_run" or
                options.get("tests") is not True or options.get("werror") is not True or
                options.get("b_ndebug") != "false" or set(profile["coverage"]) != set(FEATURE_TESTS)):
            raise ValueError("构建范围、断言或 M4 功能覆盖不完整")
        modern |= profile["dpdk"] == "24.11.8" and options.get("buildtype") == "debugoptimized"
        sanitizer |= profile["dpdk"] == "21.11.9" and options.get("b_sanitize") == "address,undefined"
        reference |= (profile["dpdk"] == "21.11.9" and options.get("buildtype") == "debugoptimized" and
                      options.get("b_sanitize") == "none")
    if not (modern and sanitizer and reference):
        raise ValueError("需要参考 21.11、原生 24.11 与 ASan/UBSan 三套同源证据")
    matching(tap, source)
    expected_tap = ["compile", "unit", "software_traffic", "ipv4_traffic", "ipv4_routing", "acl_traffic",
                    "conntrack_nat", "dnat_traffic", "ipv6_traffic", "nat66_traffic", "nd6_traffic", "tunnel_traffic"]
    if (tap.get("tap_requested") is not True or tap.get("complete_m2_validation") is not True or
            [step["name"] for step in tap["steps"]] != expected_tap or
            any(step.get("returncode") != 0 or "error" in step for step in tap["steps"])):
        raise ValueError("缺少完整十二步 TAP 回归")
    passed, kinds = {}, set()
    for benchmark in benchmarks:
        matching(benchmark, source)
        if (benchmark.get("contract_sha256") != CONTRACT_SHA256 or benchmark.get("repetitions") != 5 or
                len(benchmark.get("runs", [])) != 5 or benchmark.get("cpu_affinity") != [0, 1, 2, 3]):
            raise ValueError("性能证据改变了固定契约、参考 CPU 或五轮样本")
        rules = benchmark["rules"]
        queue = benchmark.get("scope") == "production_async_batch_service_with_memory_driver"
        kind = ("queue" if queue else "control", rules)
        if kind in kinds or rules not in (4096, 16384):
            raise ValueError("性能证据重复或规模不符")
        kinds.add(kind)
        if queue:
            if any(run.get("rules") != rules or run.get("batch") != 64 or run.get("queues") != 8 or
                   run.get("queue_depth") != 256 or run.get("window_rules") != 2048
                   for run in benchmark["runs"]):
                raise ValueError("队列规模、批量或背压窗口不符")
            if rules == 4096:
                key = "queue_4096_batch64_max_completion_p99_ns"
                passed[key] = 0 < benchmark["median_completion_p99_ns"] <= contract["performance_gates"][key]
        else:
            if benchmark.get("scope") != "production_control_with_memory_driver_and_software_backend":
                raise ValueError("控制面测量范围不符")
            passed.update(evaluate_gates(benchmark, contract))
        if not all(value is True for value in benchmark["performance_gates"].values()):
            raise ValueError("原始性能报告存在未通过门禁")
    if kinds != {("control", 4096), ("control", 16384), ("queue", 4096), ("queue", 16384)}:
        raise ValueError("缺少控制面或队列的 4096/16384 同源规模证据")
    if set(passed) != set(contract["performance_gates"]) or not all(passed.values()):
        raise ValueError("八项冻结性能指标未全部通过")
    return passed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--report-dir", type=Path)
    parser.add_argument("--complete-software", action="store_true")
    parser.add_argument("--peer-report", type=Path, action="append", default=[])
    parser.add_argument("--tap-report", type=Path)
    parser.add_argument("--benchmark-report", type=Path, action="append", default=[])
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    destination = args.report_dir.resolve() if args.report_dir else build / "m4-acceptance"
    destination.mkdir(parents=True, exist_ok=True)
    report = {"schema_version": 1, "scope": SCOPE, "hardware_acceptance": "not_run", "success": False,
              "complete_m4_validation": False, "complete_m4_software_validation": False,
              "source_sha256": source_digest(root), "started_utc": datetime.now(timezone.utc).isoformat(),
              "build": str(build), "steps": [], "coverage": [], "inputs": []}
    output = destination / "report.json"
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")

    def load(path):
        content = path.read_bytes()
        result = json.loads(content)
        report["inputs"].append({"path": str(path.resolve()), "sha256": hashlib.sha256(content).hexdigest()})
        return result

    try:
        contract_bytes = (root / "docs/m4_acceptance_contract.json").read_bytes().replace(b"\r\n", b"\n")
        if hashlib.sha256(contract_bytes).hexdigest() != CONTRACT_SHA256:
            raise ValueError("冻结契约发生变化，不能作为原门槛验收")
        contract = json.loads(contract_bytes)
        report["build_options"] = {item["name"]: item["value"] for item in json.loads(
            (build / "meson-info/intro-buildoptions.json").read_text())}
        dependencies = json.loads((build / "meson-info/intro-dependencies.json").read_text())
        report["dpdk"] = next(item["version"] for item in dependencies if item["name"] == "libdpdk")
        commands = (
            ("m3_regression", [sys.executable, "scripts/verify_m3.py", "--build-dir", str(build),
                               "--report-dir", str(destination / "m3")], None, destination / "m3/report.json"),
            ("m4_worker", [sys.executable, "tests/integration/m4_flow_traffic.py", "--build-dir", str(build),
                           "--report-dir", str(destination / "worker")], 90, destination / "worker/report.json"),
        )
        for name, command, timeout, path in commands:
            path.unlink(missing_ok=True)
            with (destination / f"{name}.log").open("w") as log:
                rc = run_command(command, root, log, timeout)
            report["steps"].append({"name": name, "command": command, "returncode": rc})
            if rc != 0:
                raise ValueError(f"{name} 未通过")
            matching(load(path), report["source_sha256"])
        tests = [json.loads(line) for line in (build / "meson-logs/testlog.json").read_text().splitlines()]
        if not tests or any(test["result"] != "OK" or test["returncode"] != 0 for test in tests):
            raise ValueError("单测存在失败或跳过")
        for name in FEATURE_TESTS:
            if not any(test["name"] == name or test["name"].endswith(":" + name) for test in tests):
                raise ValueError(f"缺少功能验收：{name}")
        report["coverage"] = list(FEATURE_TESTS)
        report["unit_passed"] = len(tests)
        if args.complete_software:
            if args.tap_report is None:
                raise ValueError("完整软件验收需要同源完整 TAP 报告")
            report["performance_gates"] = complete_software(
                report["source_sha256"], contract, [dict(report, success=True)] + [load(path) for path in args.peer_report],
                load(args.tap_report), [load(path) for path in args.benchmark_report])
            report["complete_m4_software_validation"] = True
        if source_digest(root) != report["source_sha256"]:
            raise ValueError("验收期间源码改变")
        report["success"] = True
    except (OSError, ValueError, TypeError, IndexError, KeyError, StopIteration, subprocess.SubprocessError) as error:
        report["success"] = report["complete_m4_software_validation"] = False
        report["error"] = str(error)
        print(str(error), file=sys.stderr)
    finally:
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(str(output))
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
