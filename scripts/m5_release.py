#!/usr/bin/env python3
"""生成可校验的发布包，并用两次独立构建检查正式程序是否可复现"""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile


PROGRAMS = ("dppd", "dppctl", "dppd-flow-state")
MAX_PACKAGE = 512 * 1024 * 1024
MAX_MEMBER = 128 * 1024 * 1024


def encoded(value):
    """固定键顺序和换行，同一份信息在不同主机也生成相同字节"""
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def command(arguments, cwd=None, environment=None):
    result = subprocess.run(arguments, cwd=cwd, env=environment, capture_output=True, text=True, check=True)
    return result.stdout.strip()


def source_files(root):
    """只收集明确的源码和文档目录，发布机无需携带 Git 元数据或私有运行配置"""
    names = {name for name in ("meson.build", "meson_options.txt", "README.md", "LICENSE") if (root / name).is_file()}
    for directory in ("app", "include", "lib", "scripts", "tests", "docs"):
        source_directory = root / directory
        if source_directory.is_symlink():
            raise ValueError("源码目录不能通过符号链接引入其他路径：" + directory)
        for path in source_directory.rglob("*"):
            if path.is_symlink():
                raise ValueError("源码目录不能包含符号链接：" + str(path))
            if path.is_file() and (path.suffix in (".c", ".h", ".py", ".sh", ".md", ".json") or path.name == "meson.build"):
                names.add(path.relative_to(root).as_posix())
    files = {}
    for name in sorted(names):
        path = root / name
        if path.is_symlink() or not path.is_file():
            raise ValueError("源码必须是普通文件：" + name)
        data = path.read_bytes()
        # Git 文本在 Windows 检出时可以带 CRLF，发布源统一成 Linux 构建使用的 LF
        if b"\0" not in data:
            data = data.replace(b"\r\n", b"\n")
        files[name] = (data, 0o755 if name.endswith(".sh") else 0o644)
    if "meson.build" not in files:
        raise ValueError("未找到项目源码")
    return files


def archive(files):
    """不记录操作者、构建目录和当前时间，避免无关环境改变发布包摘要"""
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.USTAR_FORMAT) as output:
        for name, (data, mode) in sorted(files.items()):
            entry = tarfile.TarInfo(name)
            entry.size, entry.mode, entry.mtime = len(data), mode, 0
            entry.uid = entry.gid = 0
            entry.uname = entry.gname = ""
            output.addfile(entry, io.BytesIO(data))
    return buffer.getvalue()


def build_metadata(build):
    metadata = {}
    for name in ("intro-buildoptions.json", "intro-dependencies.json"):
        path = build / "meson-info" / name
        if not path.is_file():
            raise ValueError("缺少 Meson 构建信息：" + str(path))
        information = json.loads(path.read_text(encoding="utf-8"))
        if name == "intro-buildoptions.json":
            # 只保留影响二进制的可移植选项，安装目录等绝对路径不进入可复现信息
            names = {"buildtype", "optimization", "debug", "b_ndebug", "b_lto", "b_sanitize", "werror", "tests", "c_std",
                     "c_args", "c_link_args"}
            metadata["options"] = {item["name"]: item["value"] for item in information if item["name"] in names}
            for key in ("c_args", "c_link_args"):
                metadata["options"][key] = [argument.split("=", 1)[0] + "=<build-root>=" + argument.rsplit("=", 1)[1]
                                            if argument.startswith(("-ffile-prefix-map=", "-fdebug-prefix-map=")) else argument
                                            for argument in metadata["options"].get(key, [])]
        elif name == "intro-dependencies.json":
            metadata["dependencies"] = [{"name": item["name"], "version": item["version"]}
                                        for item in information if item["name"] in ("libdpdk", "threads")]
    # Meson 0.61 没有 intro-compilers.json，直接保留本次配置日志记录的实际工具链身份
    log = (build / "meson-logs/meson-log.txt").read_text(encoding="utf-8")
    for key, expression in (("compiler", r"^C compiler for the host machine: (.+)$"),
                            ("linker", r"^C linker for the host machine: (.+)$"),
                            ("architecture", r"^Host machine cpu family: (.+)$"),
                            ("meson", r"^Version: (.+)$")):
        value = re.search(expression, log, re.MULTILINE)
        if value is None:
            raise ValueError("构建日志缺少实际工具链字段：" + key)
        metadata[key] = value[1]
    return metadata


def pack(root, build, output):
    sources = source_files(root)
    source_tar = archive(sources)
    metadata = build_metadata(build)
    payload = {"source.tar": (source_tar, 0o644), "build.json": (encoded(metadata), 0o644)}
    for name in PROGRAMS:
        path = build / name
        if path.is_symlink() or not path.is_file():
            raise ValueError("正式程序缺失或是链接：" + str(path))
        payload["bin/" + name] = (path.read_bytes(), 0o755)
    api = root / "app/dppd-api/main.py"
    if api.is_file():
        payload["libexec/dppd-api.py"] = (api.read_bytes().replace(b"\r\n", b"\n"), 0o755)
    for name in ("m5_release.py", "m5_deploy.py"):
        payload["libexec/" + name] = ((root / "scripts" / name).read_bytes().replace(b"\r\n", b"\n"), 0o755)
    header = (root / "include/dppd/management.h").read_text(encoding="utf-8")
    version = re.search(r"#define DPPD_MANAGEMENT_VERSION (\d+)U", header)
    if version is None:
        raise ValueError("无法识别管理协议版本")
    manifest = {"schema": 1, "management_version": int(version[1]), "source_sha256": digest(source_tar),
                "files": {name: {"sha256": digest(data), "size": len(data), "mode": mode}
                          for name, (data, mode) in sorted(payload.items())}}
    manifest_bytes = encoded(manifest)
    release_id = digest(manifest_bytes)
    payload["manifest.json"] = (manifest_bytes, 0o644)
    data = archive(payload)
    # 不覆盖已有文件，重复生成可另选路径并比较摘要
    with output.open("xb") as target:
        target.write(data)
        target.flush()
        os.fsync(target.fileno())
    return {"release_id": release_id, "package_sha256": digest(data), "source_sha256": digest(source_tar),
            "binary_sha256": {name: manifest["files"]["bin/" + name]["sha256"] for name in PROGRAMS}}


def verify_bundle(path, expected_sha256):
    """先验证调用者持有的摘要，再检查每个成员，禁止链接、路径越界和重复成员"""
    if not re.fullmatch(r"[0-9a-f]{64}", expected_sha256):
        raise ValueError("必须提供完整的小写 SHA256")
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_PACKAGE:
        raise ValueError("发布包不是普通文件或超过大小上限")
    data = path.read_bytes()
    if digest(data) != expected_sha256:
        raise ValueError("发布包 SHA256 与预期不符")
    payload, total = {}, 0
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:") as source:
        for member in source:
            name = member.name
            parts = PurePosixPath(name).parts
            if (not member.isfile() or member.issparse() or not parts or PurePosixPath(name).is_absolute() or
                    any(part in (".", "..") for part in parts) or "\\" in name or name != "/".join(parts) or
                    name in payload or member.size > MAX_MEMBER or member.size < 0 or len(payload) >= 256):
                raise ValueError("发布包含非法成员：" + name)
            total += member.size
            if total > MAX_PACKAGE:
                raise ValueError("发布包展开后超过大小上限")
            payload[name] = (source.extractfile(member).read(), member.mode)
    if "manifest.json" not in payload:
        raise ValueError("发布清单缺失")
    manifest_bytes = payload.pop("manifest.json")[0]
    if len(manifest_bytes) > 1024 * 1024:
        raise ValueError("发布清单超过大小上限")
    manifest = json.loads(manifest_bytes)
    if not isinstance(manifest, dict) or manifest.get("schema") != 1 or encoded(manifest) != manifest_bytes:
        raise ValueError("发布清单版本或编码不正确")
    expected = manifest.get("files", {})
    if (not isinstance(expected, dict) or set(expected) != set(payload) or
            not {"bin/" + name for name in PROGRAMS}.issubset(payload)):
        raise ValueError("发布清单成员不完整")
    for name, (body, mode) in payload.items():
        entry = expected[name]
        if entry != {"sha256": digest(body), "size": len(body), "mode": mode} or mode not in (0o644, 0o755):
            raise ValueError("发布内容校验失败：" + name)
    if "source.tar" not in payload or digest(payload["source.tar"][0]) != manifest.get("source_sha256"):
        raise ValueError("源码包校验失败")
    payload["manifest.json"] = (manifest_bytes, 0o644)
    return digest(manifest_bytes), manifest, payload


def reproduce(root, work, report, meson_arguments, jobs=None):
    """同一份源码在两个新目录各自编译，只有正式程序逐字节相同才通过"""
    # Ninja 默认可能按整机核数启动任务，显式并发避免突破本轮分配的 CPU 预算
    if jobs is None:
        jobs = len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else (os.cpu_count() or 1)
    if type(jobs) is not int or jobs < 1:
        raise ValueError("构建并发必须是正整数")
    files = source_files(root)
    source_sha256 = digest(archive(files))
    code_source = {name: digest(data.replace(b"\r\n", b"\n")) for name, (data, _) in files.items()
                   if name in ("meson.build", "meson_options.txt") or
                   (PurePosixPath(name).parts[0] in ("app", "include", "lib", "tests", "scripts") and
                    (PurePosixPath(name).suffix in (".c", ".h", ".py") or PurePosixPath(name).name == "meson.build"))}
    if work.exists():
        raise ValueError("复现工作目录必须尚不存在，避免覆盖已有产物")
    work.mkdir(parents=True)
    results = []
    for index in range(2):
        base = work / str(index)
        source, build = base / "source", base / "build"
        source.mkdir(parents=True)
        for name, (data, mode) in files.items():
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            path.chmod(mode)
        environment = dict(os.environ, SOURCE_DATE_EPOCH="0", TZ="UTC", LC_ALL="C", PYTHONHASHSEED="0")
        mapping = f"-ffile-prefix-map={base}=/dppd-build -fdebug-prefix-map={base}=/dppd-build"
        environment["CFLAGS"] = (environment.get("CFLAGS", "") + " " + mapping).strip()
        for command_line in (["meson", "setup", str(build), str(source), "--buildtype=debugoptimized", "-Dtests=true",
                              "-Dwerror=true", "-Db_ndebug=false", *meson_arguments],
                             ["meson", "compile", "-C", str(build), "-j", str(jobs), *PROGRAMS]):
            with (base / ("setup.log" if command_line[1] == "setup" else "compile.log")).open("w") as log:
                subprocess.run(command_line, env=environment, stdout=log, stderr=subprocess.STDOUT, check=True)
        results.append({name: digest((build / name).read_bytes()) for name in PROGRAMS})
        options = build_metadata(build)["options"]
        if (options.get("tests") is not True or options.get("werror") is not True or
                options.get("b_ndebug") != "false" or options.get("buildtype") != "debugoptimized"):
            raise ValueError("复现构建必须开启 tests、werror、断言并使用 debugoptimized")
    result = {"schema": 1, "success": results[0] == results[1] and digest(archive(source_files(root))) == source_sha256,
              "scope": "two_independent_build_directories", "code_source_sha256": code_source,
              "source_sha256": source_sha256, "builds": results, "build_jobs": jobs,
              "build_metadata": build_metadata(work / "0/build")}
    report.write_bytes(encoded(result))
    if not result["success"]:
        raise ValueError("两次构建的正式程序不同或验收期间源码改变，不能宣称二进制可复现")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="action", required=True)
    create = subcommands.add_parser("pack")
    create.add_argument("--source", type=Path, required=True)
    create.add_argument("--build-dir", type=Path, required=True)
    create.add_argument("--output", type=Path, required=True)
    verify = subcommands.add_parser("verify")
    verify.add_argument("--package", type=Path, required=True)
    verify.add_argument("--sha256", required=True)
    repeat = subcommands.add_parser("reproduce")
    repeat.add_argument("--source", type=Path, required=True)
    repeat.add_argument("--work-dir", type=Path, required=True)
    repeat.add_argument("--report", type=Path, required=True)
    repeat.add_argument("--jobs", type=int, help="构建并发，默认使用当前进程获准使用的 CPU 数量")
    repeat.add_argument("meson_arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    try:
        if args.action == "pack":
            result = pack(args.source.resolve(), args.build_dir.resolve(), args.output)
        elif args.action == "verify":
            identity, manifest, _ = verify_bundle(args.package, args.sha256)
            result = {"release_id": identity, "management_version": manifest["management_version"], "verified": True}
        else:
            arguments = args.meson_arguments[1:] if args.meson_arguments[:1] == ["--"] else args.meson_arguments
            result = reproduce(args.source.resolve(), args.work_dir.resolve(), args.report, arguments, args.jobs)
        print(json.dumps(result, ensure_ascii=False, sort_keys=True))
        return 0
    except (OSError, ValueError, KeyError, tarfile.TarError, subprocess.SubprocessError) as error:
        print(json.dumps({"error": str(error)}, ensure_ascii=False))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
