#!/usr/bin/env python3
"""使用 clang-format 14 检查或整理项目自有 C 代码，默认只检查"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


def main():
    """只处理源码目录里的 C 文件和头文件，构建产物与第三方补丁不参与格式整理"""
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--check", action="store_true", help="仅检查格式，默认行为")
    modes.add_argument("--write", action="store_true", help="把格式整理结果写回文件")
    parser.add_argument("--clang-format", default=os.environ.get("CLANG_FORMAT"), help="clang-format 14 可执行文件路径")
    args = parser.parse_args()
    formatter = args.clang_format or shutil.which("clang-format-14") or shutil.which("clang-format")
    if not formatter:
        parser.error("未找到 clang-format 14，请安装工具或通过 --clang-format 指定路径")
    # 先按调用者的当前目录解析工具路径，之后切换到仓库根目录也不会找错程序
    formatter = shutil.which(formatter) or str(Path(formatter).resolve())

    root = Path(__file__).resolve().parent.parent
    try:
        version = subprocess.run([formatter, "--version"], check=True, capture_output=True, text=True).stdout
        if re.search(r"\bversion 14\.", version) is None:
            parser.error(f"请使用 clang-format 14，避免不同版本产生反复变动，当前为 {version.strip()}")
        tracked = subprocess.run(
            ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z", "--", "app", "include", "lib", "tests"],
            cwd=root, check=True, capture_output=True,
        ).stdout.decode("utf-8").split("\0")
        sources = sorted({name for name in tracked if Path(name).suffix in (".c", ".h")})
        if not sources:
            parser.error("没有找到项目 C 源码，请在完整仓库中运行")
        flags = ["-i"] if args.write else ["--dry-run", "--Werror"]
        result = subprocess.run([formatter, "--style=file", *flags, *sources], cwd=root)
        if result.returncode != 0:
            return result.returncode
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"格式工具运行失败：{error}", file=sys.stderr)
        return 1
    print(f"{'已整理' if args.write else '格式检查通过'}：{len(sources)} 个 C 源码和头文件")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
