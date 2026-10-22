#!/usr/bin/env python3
"""验收 M5 配置与持久请求闭环，完整生产化仍需额外联合验收"""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import sys

from verify_m2 import run_command, source_digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--report-dir", type=Path)
    args = parser.parse_args()
    root, build = Path(__file__).resolve().parents[1], args.build_dir.resolve()
    destination = args.report_dir.resolve() if args.report_dir else build / "m5-state-acceptance"
    destination.mkdir(parents=True, exist_ok=True)
    report = {"success": False, "scope": "M5_M4_configuration_persistence_and_process_restart",
              "m4_configuration_recovery_validated": False, "complete_m5_validation": False,
              "m4_daemon_protocol_validated": False,
              "online_definitions_validated": False,
              "durable_requests_validated": False,
              "hardware_acceptance": "not_run", "source_sha256": source_digest(root), "steps": [],
              "started_utc": datetime.now(timezone.utc).isoformat()}
    output = destination / "report.json"
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    try:
        commands = (
            ("m4_regression", [sys.executable, "scripts/verify_m4.py", "--build-dir", str(build),
                               "--report-dir", str(destination / "m4")], None, destination / "m4/report.json"),
            ("fresh_process_worker", [sys.executable, "tests/integration/m5_flow_restart.py", "--build-dir", str(build),
                                      "--report-dir", str(destination / "restart")], 180, destination / "restart/report.json"),
            ("formal_daemon", [sys.executable, "tests/integration/m5_daemon.py", "--build-dir", str(build),
                               "--report-dir", str(destination / "daemon")], 180, destination / "daemon/report.json"),
            ("durable_requests", [sys.executable, "tests/integration/m5_requests.py", "--build-dir", str(build),
                                  "--report-dir", str(destination / "requests")], 180, destination / "requests/report.json"),
        )
        for name, command, timeout, path in commands:
            path.unlink(missing_ok=True)
            with (destination / (name + ".log")).open("w") as log:
                rc = run_command(command, root, log, timeout)
            report["steps"].append({"name": name, "command": command, "returncode": rc})
            if rc != 0:
                raise ValueError(name + " 未通过")
            result = json.loads(path.read_text())
            if result.get("success") is not True or result.get("source_sha256") != report["source_sha256"]:
                raise ValueError(name + " 报告与本轮源码不一致")
            if name == "m4_regression":
                options = result["build_options"]
                if options.get("tests") is not True or options.get("werror") is not True or options.get("b_ndebug") != "false":
                    raise ValueError("要求严格构建并开启断言")
                report["unit_passed"] = result["unit_passed"]
                report["dpdk"] = result["dpdk"]
            elif name == "fresh_process_worker":
                if [item["name"] for item in result["stages"]] != ["before", "after"] or result["packets"] != {
                        "received": 96, "forwarded": 48, "dropped": 48}:
                    raise ValueError("缺少两个全新进程的实际报文及统计")
                report["restart_packets"] = result["packets"]
            elif len(result["steps"]) != 5:
                raise ValueError("缺少正式 daemon 配置与管理闭环")
        tests = [json.loads(line) for line in (build / "meson-logs/testlog.json").read_text().splitlines()]
        required = ("M5 M4 configuration codec integrity and atomic persistence",
                    "M5 M4 durable receipts crash replay and residual cleanup",
                    "M5 16384 rule configuration replay and exact teardown",
                    "M5 configuration CLI create check and software replay")
        required += ("M5 managed protocol durable receipts and session boundaries",)
        required += ("M5 online definition transactions shared counting and durable recovery",)
        required += ("M5 persistent request idempotency crash receipts and stale key protection",)
        for name in required:
            if not any(test["name"] == name and test["result"] == "OK" and test["returncode"] == 0 for test in tests):
                raise ValueError("缺少配置恢复验收：" + name)
        if source_digest(root) != report["source_sha256"]:
            raise ValueError("验收期间源码改变")
        report["success"] = report["m4_configuration_recovery_validated"] = True
        report["m4_daemon_protocol_validated"] = True
        report["online_definitions_validated"] = True
        report["durable_requests_validated"] = True
    except (OSError, ValueError, TypeError, KeyError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
        print(str(error), file=sys.stderr)
    finally:
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(str(output))
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
