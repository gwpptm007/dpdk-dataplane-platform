#!/usr/bin/env python3
"""验收当前可在虚拟环境落地的 M3 功能，硬件卸载始终单独登记为未验收"""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from verify_m2 import run_command, source_digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--tap", action="store_true", help="同时执行完整 M2 TAP 回归，需要 root")
    parser.add_argument("--report-dir", type=Path, help="默认保存到构建目录的 m3-acceptance 子目录")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    destination = args.report_dir.resolve() if args.report_dir else build / "m3-acceptance"
    destination.mkdir(parents=True, exist_ok=True)
    report = {"started_utc": datetime.now(timezone.utc).isoformat(), "build": str(build),
              "scope": "virtual_control_and_software_regression", "hardware_acceptance": "not_run",
              "complete_m3_validation": False, "tap_requested": args.tap,
              "source_sha256": source_digest(root), "steps": [], "success": False}
    report_path = destination / "report.json"
    # 先撤销上次成功状态，参数检查失败或进程中断也不会留下貌似本轮通过的旧报告
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    regression = destination / "software-regression"
    command = [sys.executable, "scripts/verify_m2.py", "--build-dir", str(build), "--report-dir", str(regression)]
    if args.tap:
        command.append("--tap")
    # 复用已有验收门禁，包含全量 M3 单测；不复制一套容易遗漏跳过或源码变化检查的流程
    # 回归子入口已经逐步计时并清理自己的进程组，外层再限时会先杀掉负责清理的父进程
    commands = [("inventory", [sys.executable, "scripts/inspect_m3_environment.py"], 60),
                ("regression", command, None),
                ("transfer_process", [sys.executable, "tests/integration/m3_transfer.py",
                                      "--build-dir", str(build)], 180),
                ("shared_recovery", [sys.executable, "tests/integration/m3_shared_recovery.py",
                                     "--build-dir", str(build)], 180),
                ("transfer_simulation", [sys.executable, "scripts/simulate_m3.py", "demo",
                                         "--build-dir", str(build)], 180)]
    try:
        if args.tap and (not hasattr(os, "geteuid") or os.geteuid() != 0):
            raise RuntimeError("--tap 必须以 root 运行")
        if not (build / "meson-info/intro-buildoptions.json").is_file():
            raise RuntimeError("请先配置开启 tests 和 werror 的 Meson 构建目录")
        metadata = json.loads((build / "meson-info/meson-info.json").read_text())
        if Path(metadata["directories"]["source"]).resolve() != root:
            raise RuntimeError("构建目录对应另一份源码，不能为当前工作区验收")
        for name, command, timeout in commands:
            if name == "regression":
                # 清除旧成功标记，子进程本轮没有生成报告时必须失败
                (regression / "report.json").unlink(missing_ok=True)
            step = {"name": name, "command": command, "log": str(destination / f"{name}.log")}
            report["steps"].append(step)
            started = time.monotonic()
            print(f"START {name}", flush=True)
            with Path(step["log"]).open("w") as output:
                try:
                    step["returncode"] = run_command(command, root, output, timeout)
                except (OSError, subprocess.TimeoutExpired) as error:
                    step["error"] = str(error)
                    step["returncode"] = -1
            step["seconds"] = round(time.monotonic() - started, 3)
            if step["returncode"] != 0:
                raise RuntimeError(f"{name} 失败，日志：{step['log']}")
            if name == "regression":
                # 子报告还必须和本轮源码一致，旧成功报告不能补充为当前结果
                result = json.loads((regression / "report.json").read_text())
                if (not result["success"] or result["source_sha256"] != report["source_sha256"] or
                        result["tap_requested"] != args.tap or result["complete_m2_validation"] != args.tap):
                    raise RuntimeError("软件回归报告没有通过或源码版本不一致")
                report["software_regression"] = {"report": str(regression / "report.json"),
                                                 "complete_m2_validation": result["complete_m2_validation"]}
            print(f"PASS {name} {step['seconds']}s", flush=True)
        if source_digest(root) != report["source_sha256"]:
            raise RuntimeError("验收期间源码发生变化，不能报告同一版本通过")
        report["success"] = True
    except (RuntimeError, OSError, ValueError, KeyError) as error:
        report["error"] = str(error)
        print(f"FAIL {error}", file=sys.stderr, flush=True)
    finally:
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    # success 只表示所列虚拟验收通过，不改变 complete_m3_validation 和硬件状态
    print(f"REPORT {report_path}", flush=True)
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
