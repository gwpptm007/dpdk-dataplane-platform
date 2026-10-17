#!/usr/bin/env python3
"""统一运行 M2 软件验收，保存命令、日志、源码摘要与明确的成功或失败结果"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def source_digest(root):
    result = {}
    for directory in ("app", "include", "lib", "tests", "scripts"):
        for path in sorted((root / directory).rglob("*")):
            if path.is_file() and (path.suffix in (".c", ".h", ".py") or path.name == "meson.build"):
                content = path.read_bytes().replace(b"\r\n", b"\n")
                result[path.relative_to(root).as_posix()] = hashlib.sha256(content).hexdigest()
    for name in ("meson.build", "meson_options.txt"):
        result[name] = hashlib.sha256((root / name).read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    return result


def run_command(command, root, output, timeout):
    # 每步独立进程组，超时也一并停止其 daemon，防止私有命名空间和 TAP 残留
    with subprocess.Popen(command, cwd=root, stdout=output, stderr=subprocess.STDOUT,
                          start_new_session=True) as process:
        try:
            return process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            for action, wait in ((signal.SIGTERM, 5), (signal.SIGKILL, 5)):
                try:
                    os.killpg(process.pid, action)
                except ProcessLookupError:
                    pass
                try:
                    process.wait(timeout=wait)
                except subprocess.TimeoutExpired:
                    continue
                # 组长退出不代表子进程退出，最后一次 KILL 仍尝试清理同组残留
                if action == signal.SIGKILL:
                    break
            raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--tap", action="store_true", help="增加完整私有网络命名空间 TAP 验收，需要 root")
    parser.add_argument("--report-dir", type=Path, help="默认保存到构建目录的 m2-acceptance 子目录")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    if args.tap and (not hasattr(os, "geteuid") or os.geteuid() != 0):
        parser.error("--tap 必须以 root 运行，不能跳过后仍报告完整验收成功")
    if not (build / "meson-info/intro-buildoptions.json").is_file():
        parser.error("请先配置开启 tests 的 Meson 构建目录")
    destination = args.report_dir.resolve() if args.report_dir else build / "m2-acceptance"
    destination.mkdir(parents=True, exist_ok=True)
    report_path = destination / "report.json"
    report = {"started_utc": datetime.now(timezone.utc).isoformat(), "build": str(build),
              "tap_requested": args.tap, "complete_m2_validation": False,
              "source_sha256": source_digest(root), "steps": [], "success": False}
    options = json.loads((build / "meson-info/intro-buildoptions.json").read_text())
    for required in ("tests", "werror"):
        if not any(option["name"] == required and option["value"] is True for option in options):
            parser.error(f"完整验收要求构建选项 {required}=true")
    commands = [("compile", ["meson", "compile", "-C", str(build)], 180),
                ("unit", ["meson", "test", "-C", str(build), "--print-errorlogs"], 180)]
    for name in ("software_traffic", "ipv4_traffic"):
        commands.append((name, [sys.executable, f"tests/integration/{name}.py", "--build-dir", str(build)], 180))
    if args.tap:
        for name in ("ipv4_routing", "acl_traffic", "conntrack_nat", "dnat_traffic", "ipv6_traffic",
                     "nat66_traffic", "nd6_traffic", "tunnel_traffic"):
            commands.append((name, [sys.executable, f"tests/integration/{name}.py", "--build-dir", str(build)], 300))
    report["dpdk"] = [dependency for dependency in json.loads((build / "meson-info/intro-dependencies.json").read_text())
                      if dependency.get("name") == "libdpdk"]
    try:
        for name, command, timeout in commands:
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
            if name == "unit":
                text = Path(step["log"]).read_text()
                for label in ("Skipped:", "Expected Fail:", "Fail:", "Unexpected Pass:", "Timeout:"):
                    # 匹配完整行首，不能把 Unexpected Pass 误当作 Pass 或 Fail 的一部分
                    values = [line.split(":", 1)[1].strip() for line in text.splitlines() if line.startswith(label)]
                    if values != ["0"]:
                        raise RuntimeError(f"单元验收存在跳过或异常结果：{label} {values}")
            print(f"PASS {name} {step['seconds']}s", flush=True)
        if source_digest(root) != report["source_sha256"]:
            raise RuntimeError("验收期间源码发生变化，不能把混合版本报告为通过")
        report["success"] = True
        report["complete_m2_validation"] = args.tap
    except RuntimeError as error:
        report["error"] = str(error)
        print(f"FAIL {error}", file=sys.stderr, flush=True)
    finally:
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"REPORT {report_path}", flush=True)
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
