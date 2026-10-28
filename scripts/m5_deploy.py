#!/usr/bin/env python3
"""在私有目录部署软件 daemon，保留当前策略完成串行升级和失败回滚"""

import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time
import uuid

from m5_release import encoded, verify_bundle


def sync_directory(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def save(path, value):
    """替换前同步完整内容，替换后同步目录，崩溃恢复只读取完整记录"""
    descriptor, temporary = tempfile.mkstemp(prefix=".write-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as target:
            target.write(encoded(value))
            target.flush()
            os.fsync(target.fileno())
        os.replace(temporary, path)
        sync_directory(path.parent)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def read(path, default=None):
    if not path.exists():
        return default
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 1024 * 1024:
        raise ValueError("部署记录必须是有界普通文件：" + str(path))
    return json.loads(path.read_text(encoding="utf-8"))


def checked_root(path):
    """管理目录由当前用户独占，所有运行文件固定在目录内，不追随目录别名"""
    root = Path(os.path.abspath(path))
    if root != root.resolve() or root.is_symlink() or not root.is_dir():
        raise ValueError("部署目录必须是已存在的绝对普通目录")
    for name in ("", "releases", "var", "run", "etc"):
        directory = root / name
        metadata = directory.stat()
        if directory.is_symlink() or not directory.is_dir() or metadata.st_uid != os.geteuid() or metadata.st_mode & 0o077:
            raise ValueError("部署目录必须由当前用户所有且权限为 0700：" + str(directory))
    for name in ("deployment.json", "current.json", "transaction.json", "run/process.json", "run/exit.json", "run/launch.json",
                 "run/daemon.log", "var/flow.state"):
        item = root / name
        if item.exists() or item.is_symlink():
            metadata = item.lstat()
            if item.is_symlink() or not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1 or metadata.st_uid != os.geteuid():
                raise ValueError("受管文件必须是本用户的独立普通文件：" + str(item))
    return root


def identity(pid):
    """同时核对启动批次和进程开始时刻，PID 被复用也不会误杀别的进程"""
    try:
        entry = Path(f"/proc/{pid}")
        fields = (entry / "stat").read_text().rsplit(")", 1)[1].split()
        if fields[0] == "Z":
            return None
        return {"pid": pid, "start": fields[19], "uid": entry.stat().st_uid,
                "boot": Path("/proc/sys/kernel/random/boot_id").read_text().strip()}
    except (FileNotFoundError, ProcessLookupError):
        return None


def alive(record):
    return record is not None and identity(record["identity"]["pid"]) == record["identity"]


def terminate_owned(record):
    """用 pidfd 固定进程对象，核对身份之后发生 PID 复用也不会把信号发错"""
    try:
        descriptor = os.pidfd_open(record["identity"]["pid"], 0)
    except ProcessLookupError:
        return
    try:
        if alive(record):
            signal.pidfd_send_signal(descriptor, signal.SIGTERM)
    finally:
        os.close(descriptor)


@contextlib.contextmanager
def private_lock(path, blocking=False):
    """主事务与启动交接分别加锁，子进程无需等待父进程完成整个启动流程"""
    import fcntl
    descriptor = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1 or metadata.st_uid != os.geteuid():
            raise ValueError("部署锁不是本用户的独立普通文件")
        deadline = time.monotonic() + 5
        while True:
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if not blocking:
                    raise
                if time.monotonic() >= deadline:
                    raise TimeoutError("启动交接锁仍被占用，保留运行状态等待处理") from None
                time.sleep(0.01)
        yield
    finally:
        os.close(descriptor)


class Deployment:
    def __init__(self, path, timeout=20):
        self.root, self.timeout = checked_root(path), timeout
        self.supervisors = []
        self.config = read(self.root / "deployment.json")
        if not isinstance(self.config, dict) or self.config.get("schema") != 1:
            raise ValueError("部署配置缺失或版本不正确")
        self.state = self.root / "var/flow.state"
        self.socket = self.root / "run/control.sock"

    @contextlib.contextmanager
    def lock(self):
        with private_lock(self.root / "run/deploy.lock"):
            yield self

    def cancel_launch(self):
        """中断后先取消尚未登记的旧启动，迟到的子进程不能覆盖新实例的身份"""
        with private_lock(self.root / "run/launch.lock", blocking=True):
            intent = read(self.root / "run/launch.json")
            if intent is not None and intent.get("enabled") is True:
                intent["enabled"] = False
                save(self.root / "run/launch.json", intent)

    def release(self, release_id):
        if not isinstance(release_id, str) or not re.fullmatch(r"[0-9a-f]{64}", release_id):
            raise ValueError("发布版本必须是完整清单摘要")
        directory = self.root / "releases" / release_id
        manifest_path = directory / "manifest.json"
        if directory.is_symlink() or not directory.is_dir():
            raise ValueError("指定版本尚未安装")
        manifest = read(manifest_path)
        if hashlib.sha256(encoded(manifest)).hexdigest() != release_id:
            raise ValueError("已安装版本清单被修改")
        for name, detail in manifest["files"].items():
            item = directory / name
            if (item.is_symlink() or any(parent.is_symlink() for parent in item.parents) or
                    not item.is_file() or not item.resolve().is_relative_to(directory.resolve()) or
                    item.stat().st_size != detail["size"] or
                    item.stat().st_nlink != 1 or stat.S_IMODE(item.stat().st_mode) != detail["mode"] or
                    hashlib.sha256(item.read_bytes()).hexdigest() != detail["sha256"]):
                raise ValueError("已安装版本文件被修改：" + name)
        return directory

    def install(self, package, sha256):
        release_id, _, files = verify_bundle(package, sha256)
        final = self.root / "releases" / release_id
        if final.exists():
            self.release(release_id)
            return {"release_id": release_id, "installed": True, "already_present": True}
        temporary = Path(tempfile.mkdtemp(prefix=".install-", dir=final.parent))
        for name, (data, mode) in files.items():
            target = temporary / name
            target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            with target.open("xb") as output:
                output.write(data)
                output.flush()
                os.fsync(output.fileno())
            target.chmod(mode)
        # 所有文件先可靠保存，再一次发布完整版本目录，不修改已经安装的版本
        for directory in sorted((item for item in temporary.rglob("*") if item.is_dir()), key=lambda p: len(p.parts), reverse=True):
            sync_directory(directory)
        sync_directory(temporary)
        os.rename(temporary, final)
        sync_directory(final.parent)
        return {"release_id": release_id, "installed": True, "already_present": False}

    def cli(self, release_id, operation, timeout=3):
        executable = self.root / "releases" / release_id / "bin/dppctl"
        return subprocess.run([str(executable), "--socket", str(self.socket), operation], capture_output=True,
                              text=True, timeout=timeout)

    def status(self):
        record = read(self.root / "run/process.json")
        running = alive(record)
        ready = False
        if running:
            try:
                ready = self.cli(record["release"], "ready").returncode == 0
            except (OSError, subprocess.SubprocessError):
                pass
        return {"current": read(self.root / "current.json", {}), "process": record, "running": running, "ready": ready,
                "transaction": read(self.root / "transaction.json"), "hardware_acceptance": "not_run"}

    def preflight(self, release_id):
        directory = self.release(release_id)
        if self.state.is_symlink() or not self.state.is_file() or self.state.stat().st_nlink != 1:
            raise ValueError("策略文件必须是独立普通文件")
        # 候选程序只恢复一份私有副本，不能在预检时改动线上策略或抢占线上文件锁
        with tempfile.TemporaryDirectory(prefix=".preflight-", dir=self.root / "run") as scratch:
            copy = Path(scratch) / "flow.state"
            copy.write_bytes(self.state.read_bytes())
            copy.chmod(0o600)
            subprocess.run([str(directory / "bin/dppd-flow-state"), "restore-check", str(copy)],
                           capture_output=True, text=True, timeout=self.timeout, check=True)

    def cleanup_dead(self):
        record = read(self.root / "run/process.json")
        if alive(record):
            raise ValueError("受管进程仍在运行，不能清理入口")
        if self.socket.exists() or self.socket.is_symlink():
            metadata = self.socket.lstat()
            if (record is None or not stat.S_ISSOCK(metadata.st_mode) or
                    record.get("socket") != {"device": metadata.st_dev, "inode": metadata.st_ino}):
                raise ValueError("入口不是已退出受管进程的 socket，拒绝删除")
            # 即使路径和 inode 一致，也不删除仍有进程监听的入口
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                client.settimeout(1)
                try:
                    client.connect(str(self.socket))
                except ConnectionRefusedError:
                    pass
                else:
                    raise ValueError("入口仍被某个进程监听，拒绝删除")
            self.socket.unlink()
            sync_directory(self.socket.parent)

    def start_process(self, release_id):
        self.cancel_launch()
        self.release(release_id)
        self.cleanup_dead()
        attempt = uuid.uuid4().hex
        with private_lock(self.root / "run/launch.lock", blocking=True):
            save(self.root / "run/launch.json", {"attempt": attempt, "release": release_id, "enabled": True})
        # 启动器先保存自己的身份再 exec，控制程序崩溃也不会留下没有身份记录的 daemon
        launch = [sys.executable, str(Path(__file__).resolve()), "_supervise", str(self.root), release_id, attempt]
        with (self.root / "run/daemon.log").open("ab") as log:
            child = subprocess.Popen(launch, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                     start_new_session=True, close_fds=True)
        self.supervisors.append(child)
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            record = read(self.root / "run/process.json")
            if record is not None and record.get("attempt") == attempt:
                if not alive(record):
                    raise RuntimeError("候选 daemon 启动后退出")
                try:
                    if self.cli(release_id, "ready", timeout=1).returncode == 0:
                        metadata = self.socket.lstat()
                        if not stat.S_ISSOCK(metadata.st_mode):
                            raise ValueError("实际管理入口不是 socket")
                        record["socket"] = {"device": metadata.st_dev, "inode": metadata.st_ino}
                        save(self.root / "run/process.json", record)
                        return
                except (OSError, subprocess.SubprocessError):
                    pass
            if child.poll() is not None:
                raise RuntimeError("受管启动器提前退出")
            time.sleep(0.03)
        raise TimeoutError("候选 daemon 未在期限内 ready")

    def stop_process(self, require_clean=True):
        self.cancel_launch()
        record = read(self.root / "run/process.json")
        if record is None:
            self.cleanup_dead()
            return
        if alive(record):
            if require_clean:
                result = self.cli(record["release"], "flow-status")
                fields = dict(re.findall(r"([a-z-]+)=([^\s]+)", result.stdout))
                if result.returncode or any(fields.get(name) != "no" for name in ("dirty", "residual", "isolated")) or fields.get("jobs") != "0":
                    raise ValueError("策略未持久化、存在残留或未领取任务，先处理后再升级")
            # 发信号前再次核对身份，只有本目录记录的进程可以被停止
            terminate_owned(record)
            deadline = time.monotonic() + self.timeout
            while alive(record) and time.monotonic() < deadline:
                time.sleep(0.03)
            if alive(record):
                raise TimeoutError("旧 daemon 未正常退出，保留当前版本且不启动第二个写入者")
        if require_clean:
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                receipt = read(self.root / "run/exit.json")
                if receipt is not None and receipt.get("attempt") == record["attempt"]:
                    if receipt["returncode"] != 0:
                        raise RuntimeError("旧 daemon 退出失败，禁止自动升级")
                    break
                time.sleep(0.03)
            else:
                raise RuntimeError("缺少正常退出回执，使用 recover 核对持久策略后恢复")
        self.cleanup_dead()
        # 同一控制进程启动的监督者由这里领取退出码，不把它留成僵尸子进程
        for child in self.supervisors:
            child.wait(timeout=3)
        self.supervisors.clear()

    def select(self, release_id, previous):
        # current 链接供 API 每次选择同版本 CLI，状态记录用于中断后恢复
        current = self.root / "current"
        if current.exists() or current.is_symlink():
            if not current.is_symlink() or re.fullmatch(r"releases/[0-9a-f]{64}", os.readlink(current)) is None:
                raise ValueError("current 不是部署器管理的版本链接，拒绝覆盖")
        temporary = self.root / (".current-" + uuid.uuid4().hex)
        temporary.symlink_to("releases/" + release_id, target_is_directory=True)
        os.replace(temporary, self.root / "current")
        sync_directory(self.root)
        save(self.root / "current.json", {"release": release_id, "previous": previous})

    def transaction(self, record, phase, **extra):
        record.update(phase=phase, **extra)
        save(self.root / "transaction.json", record)

    def upgrade(self, target):
        pending = read(self.root / "transaction.json", {})
        if pending.get("phase") not in (None, "committed", "rolled_back", "failed"):
            raise ValueError("存在未收尾升级，请先执行 recover")
        old = read(self.root / "current.json", {}).get("release")
        running = read(self.root / "run/process.json")
        if alive(running) and running["release"] != old:
            raise ValueError("实际运行版本与已提交版本不一致，请先 recover")
        if old == target and self.status()["ready"]:
            return {"release_id": target, "ready": True, "already_current": True}
        self.preflight(target)
        record = {"id": uuid.uuid4().hex, "old": old, "target": target, "phase": "prepared"}
        self.transaction(record, "prepared")
        try:
            self.stop_process()
            self.transaction(record, "stopped")
            # 正常退出可能完成最后一批策略，再次按最终持久文件核对候选版本兼容性
            self.preflight(target)
            self.start_process(target)
            self.transaction(record, "candidate_ready")
            self.select(target, old)
            self.transaction(record, "committed")
            return {"release_id": target, "ready": True, "rolled_back": False}
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
            self.transaction(record, "rollback_pending", error=str(error))
            try:
                self.restore_old(record)
            except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as rollback_error:
                self.transaction(record, "blocked", rollback_error=str(rollback_error))
                raise RuntimeError("升级失败且回滚待处理，原始策略保留：" + str(rollback_error)) from error
            raise RuntimeError("升级未通过，已恢复原版本并保留当前策略：" + str(error)) from error

    def restore_old(self, record):
        running = read(self.root / "run/process.json")
        if running is not None and running["release"] not in (record["old"], record["target"]):
            raise ValueError("发现不属于本次升级的受管进程，拒绝停止")
        if record["old"] is not None and alive(running) and running["release"] == record["old"]:
            if self.cli(record["old"], "ready").returncode != 0:
                raise RuntimeError("原进程仍在运行但未就绪，保留它等待处理")
        else:
            self.stop_process(require_clean=alive(running))
            if record["old"] is None:
                self.transaction(record, "failed")
                return
            # 永远让旧二进制读取当前文件，格式不兼容则封闭失败，不能覆盖成历史策略
            self.preflight(record["old"])
            self.start_process(record["old"])
        self.select(record["old"], read(self.root / "current.json", {}).get("previous"))
        self.transaction(record, "rolled_back")

    def recover(self):
        self.cancel_launch()
        record = read(self.root / "transaction.json")
        if record is None or record["phase"] in ("committed", "rolled_back", "failed"):
            current = read(self.root / "current.json", {}).get("release")
            if current is None:
                raise ValueError("尚无已提交版本，先指定版本 start")
            actual = self.status()
            if actual["running"]:
                if actual["process"]["release"] != current:
                    raise ValueError("实际运行版本与当前版本不一致，保留进程等待核对")
                if not actual["ready"]:
                    raise RuntimeError("当前进程仍在运行但未就绪，先修复持久化或残留，不能用重启代替恢复")
                return actual
            self.stop_process(require_clean=False)
            self.preflight(current)
            self.start_process(current)
            return self.status()
        running = read(self.root / "run/process.json")
        # 已经 ready 的候选可完成原事务，其余情况恢复原版本，所有步骤均可再次执行
        if alive(running) and running["release"] == record["target"] and self.cli(record["target"], "ready").returncode == 0:
            self.select(record["target"], record["old"])
            self.transaction(record, "committed")
        else:
            self.restore_old(record)
        return self.status()


def initialize(path, source, arguments):
    if (os.name != "posix" or not Path("/proc/self/stat").exists() or
            not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal")):
        raise ValueError("部署器仅支持 Linux 的进程身份与文件锁接口")
    root = Path(os.path.abspath(path))
    if root != root.resolve() or root.exists():
        raise ValueError("初始化目录必须不存在且不能经过符号链接")
    if source.is_symlink() or not source.is_file():
        raise ValueError("初始策略必须是普通文件")
    if (not isinstance(arguments, list) or not arguments or "--" not in arguments or
            any(not isinstance(item, str) or "\0" in item for item in arguments) or
            any(item.split("=", 1)[0] in ("--flow-state", "--state-path", "--control-socket") for item in arguments)):
        raise ValueError("参数必须是含 EAL 分隔符的字符串数组，状态和 socket 由部署器统一指定")
    root.mkdir(mode=0o700)
    for name in ("releases", "var", "run", "etc"):
        (root / name).mkdir(mode=0o700)
    state = root / "var/flow.state"
    with state.open("xb") as target:
        os.fchmod(target.fileno(), 0o600)
        target.write(source.read_bytes())
        target.flush()
        os.fsync(target.fileno())
    sync_directory(state.parent)
    save(root / "deployment.json", {"schema": 1, "arguments": arguments})
    return {"root": str(root), "initialized": True}


def worker(root, release_id, attempt):
    deploy = Deployment(root)
    directory = deploy.release(release_id)
    arguments = [item.replace("{root}", str(deploy.root)).replace("{release}", str(directory))
                 for item in deploy.config["arguments"]]
    executable = str(directory / "bin/dppd")
    with private_lock(deploy.root / "run/launch.lock", blocking=True):
        intent = read(deploy.root / "run/launch.json")
        if intent != {"attempt": attempt, "release": release_id, "enabled": True}:
            raise ValueError("启动意图已经取消或被新事务替换，拒绝迟到的子进程")
        record = {"identity": identity(os.getpid()), "release": release_id, "attempt": attempt}
        save(deploy.root / "run/process.json", record)
        # 锁描述符带 CLOEXEC，身份登记与 exec 之间不会被另一个恢复命令插入
        os.execv(executable, [executable, *arguments, "--flow-state", str(deploy.state), "--control-socket", str(deploy.socket)])


def supervise(root, release_id, attempt):
    # 独立守护者记录实际退出码，后续命令无需猜测进程消失是否代表正常退出
    child = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "_worker", root, release_id, attempt])
    returncode = child.wait()
    with private_lock(Path(root) / "run/launch.lock", blocking=True):
        record = read(Path(root) / "run/process.json")
        if record is not None and record.get("attempt") == attempt:
            save(Path(root) / "run/exit.json", {"attempt": attempt, "returncode": returncode})
    return 0


def rolling(roots, target, minimum, timeout):
    ordered = sorted({str(Path(root).resolve()) for root in roots})
    if len(ordered) != len(roots) or len(roots) < 2 or not 1 <= minimum < len(roots):
        raise ValueError("滚动升级需要至少两个不同实例，最少就绪数必须介于 1 和实例数减一之间")
    deployments = {root: Deployment(root, timeout) for root in ordered}
    results = []
    # 固定顺序同时锁定参与实例，防止两个滚动任务分别停掉不同实例
    with contextlib.ExitStack() as locks:
        for deploy in deployments.values():
            locks.enter_context(deploy.lock())
            deploy.preflight(target)
        for root in roots:
            selected = str(Path(root).resolve())
            available = sum(deploy.status()["ready"] for key, deploy in deployments.items() if key != selected)
            if available < minimum:
                raise RuntimeError("其他实例的就绪数量不足，停止后续升级")
            try:
                results.append({"root": selected, "result": deployments[selected].upgrade(target)})
            except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
                raise RuntimeError("滚动升级在 " + selected + " 停止，已完成实例 " +
                                   json.dumps([item["root"] for item in results], ensure_ascii=False) + ": " + str(error)) from error
    return {"success": True, "instances": results, "traffic_failover": "external", "lossless_claim": False}


def main():
    if len(sys.argv) == 5 and sys.argv[1] in ("_worker", "_supervise"):
        return worker(*sys.argv[2:]) if sys.argv[1] == "_worker" else supervise(*sys.argv[2:])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path)
    parser.add_argument("--timeout", type=float, default=20)
    subcommands = parser.add_subparsers(dest="action", required=True)
    initial = subcommands.add_parser("init")
    initial.add_argument("--state", type=Path, required=True)
    initial.add_argument("--arguments", type=Path, required=True)
    install = subcommands.add_parser("install")
    install.add_argument("--package", type=Path, required=True)
    install.add_argument("--sha256", required=True)
    for name in ("start", "upgrade"):
        subcommands.add_parser(name).add_argument("--release", required=True)
    for name in ("status", "stop", "recover", "rollback"):
        subcommands.add_parser(name)
    sequence = subcommands.add_parser("rolling")
    sequence.add_argument("--roots", nargs="+", required=True)
    sequence.add_argument("--release", required=True)
    sequence.add_argument("--minimum-ready", type=int, required=True)
    args = parser.parse_args()
    try:
        if os.name != "posix" or not 0 < args.timeout <= 300:
            raise ValueError("需要 Linux 环境且超时须介于 0 和 300 秒之间")
        if args.action == "rolling":
            result = rolling(args.roots, args.release, args.minimum_ready, args.timeout)
        elif args.root is None:
            raise ValueError("必须指定 --root")
        elif args.action == "init":
            result = initialize(args.root, args.state, read(args.arguments))
        else:
            with Deployment(args.root, args.timeout).lock() as deploy:
                if args.action == "install":
                    result = deploy.install(args.package, args.sha256)
                elif args.action in ("start", "upgrade"):
                    result = deploy.upgrade(args.release)
                elif args.action == "stop":
                    deploy.stop_process()
                    result = deploy.status()
                elif args.action == "status":
                    result = deploy.status()
                elif args.action == "recover":
                    result = deploy.recover()
                else:
                    previous = read(deploy.root / "current.json", {}).get("previous")
                    if previous is None:
                        raise ValueError("没有可回滚的已安装版本")
                    result = deploy.upgrade(previous)
        print(json.dumps(result, ensure_ascii=False, sort_keys=True))
        return 0
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as error:
        print(json.dumps({"error": str(error)}, ensure_ascii=False))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
