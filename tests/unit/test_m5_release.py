#!/usr/bin/env python3
"""发布包完整性、升级回滚、身份保护和中断恢复的边界测试"""

import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import m5_deploy as deploy
import m5_release as release


def bundle(path, tag="first", checker=b"#!/bin/sh\nexit 0\n", daemon=b"#!/bin/sh\nexit 0\n", cli=b"#!/bin/sh\nexit 0\n"):
    files = {"bin/dppd": (daemon, 0o755), "bin/dppctl": (cli, 0o755),
             "bin/dppd-flow-state": (checker, 0o755), "source.tar": (tag.encode(), 0o644)}
    manifest = {"schema": 1, "management_version": 29, "source_sha256": release.digest(files["source.tar"][0]),
                "files": {name: {"sha256": release.digest(data), "size": len(data), "mode": mode}
                          for name, (data, mode) in files.items()}}
    identity = release.digest(release.encoded(manifest))
    files["manifest.json"] = (release.encoded(manifest), 0o644)
    data = release.archive(files)
    path.write_bytes(data)
    return identity, release.digest(data)


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_same_payload_same_archive_bytes(self):
        first = release.archive({"b": (b"B", 0o755), "a": (b"A", 0o644)})
        second = release.archive({"a": (b"A", 0o644), "b": (b"B", 0o755)})
        self.assertEqual(first, second)
        with tarfile.open(fileobj=io.BytesIO(first)) as source:
            self.assertTrue(all(item.mtime == 0 and item.uid == 0 and item.uname == "" for item in source))

    def test_bundle_integrity_and_expected_digest(self):
        package = self.root / "release.tar"
        identity, sha = bundle(package)
        self.assertEqual(release.verify_bundle(package, sha)[0], identity)
        with self.assertRaises(ValueError):
            release.verify_bundle(package, "0" * 64)
        with self.assertRaises(ValueError):
            release.verify_bundle(package, "unknown")

    def test_paths_links_and_duplicate_members_are_rejected(self):
        for name, kind, repeated in (("../outside", tarfile.REGTYPE, False), ("/absolute", tarfile.REGTYPE, False),
                                     ("a\\b", tarfile.REGTYPE, False), ("./a", tarfile.REGTYPE, False),
                                     ("link", tarfile.SYMTYPE, False), ("same", tarfile.REGTYPE, True)):
            with self.subTest(name=name):
                memory = io.BytesIO()
                with tarfile.open(fileobj=memory, mode="w") as output:
                    for _ in range(2 if repeated else 1):
                        member = tarfile.TarInfo(name)
                        member.type, member.linkname = kind, "outside"
                        output.addfile(member, io.BytesIO())
                data = memory.getvalue()
                package = self.root / "invalid.tar"
                package.write_bytes(data)
                with self.assertRaises(ValueError):
                    release.verify_bundle(package, release.digest(data))

    def test_modified_member_is_not_covered_by_recomputed_package_digest(self):
        package = self.root / "release.tar"
        _, sha = bundle(package)
        _, _, files = release.verify_bundle(package, sha)
        files["bin/dppd"] = (b"different executable", 0o755)
        data = release.archive(files)
        package.write_bytes(data)
        with self.assertRaises(ValueError):
            release.verify_bundle(package, release.digest(data))

    def test_unlisted_file_and_unexpected_mode_rejected(self):
        for operation in ("extra", "mode"):
            with self.subTest(operation=operation):
                package = self.root / "release.tar"
                _, sha = bundle(package)
                _, _, files = release.verify_bundle(package, sha)
                if operation == "extra":
                    files["unlisted"] = (b"private", 0o644)
                else:
                    files["bin/dppd"] = (files["bin/dppd"][0], 0o777)
                data = release.archive(files)
                package.write_bytes(data)
                with self.assertRaises(ValueError):
                    release.verify_bundle(package, release.digest(data))

    def test_reproduce_keeps_two_roots_and_explicit_job_budget(self):
        """仅替换编译进程，检查复制、两次构建路径、环境与 CPU 并发限制的实际装配"""
        source, work, report = self.root / "source", self.root / "work", self.root / "report.json"
        source.mkdir()
        (source / "meson.build").write_text("project('example', 'c')\n")
        calls = []

        def compile_programs(command, **arguments):
            calls.append((command, arguments["env"]))
            if command[1] == "compile":
                build = Path(command[command.index("-C") + 1])
                build.mkdir()
                for name in release.PROGRAMS:
                    (build / name).write_bytes(name.encode())

        metadata = {"options": {"tests": True, "werror": True, "b_ndebug": "false", "buildtype": "debugoptimized"}}
        with patch.object(release.subprocess, "run", side_effect=compile_programs), \
                patch.object(release, "build_metadata", return_value=metadata):
            result = release.reproduce(source, work, report, [], jobs=4)
        self.assertTrue(result["success"])
        self.assertEqual(result["build_jobs"], 4)
        self.assertEqual(len(calls), 4)
        for index in range(2):
            setup, environment = calls[index * 2]
            command = calls[index * 2 + 1][0]
            self.assertEqual(Path(setup[2]), work / str(index) / "build")
            self.assertEqual(Path(setup[3]), work / str(index) / "source")
            self.assertEqual(command[command.index("-j") + 1], "4")
            self.assertEqual(environment["SOURCE_DATE_EPOCH"], "0")
            self.assertEqual(environment["LC_ALL"], "C")
            self.assertIn(f"-ffile-prefix-map={work / str(index)}=/dppd-build", environment["CFLAGS"])
            self.assertIn(f"-fdebug-prefix-map={work / str(index)}=/dppd-build", environment["CFLAGS"])
        with self.assertRaises(ValueError):
            release.reproduce(source, self.root / "invalid", report, [], jobs=0)
        self.assertFalse((self.root / "invalid").exists())


@unittest.skipUnless(os.name == "posix", "部署器要求 Linux 进程与文件锁接口")
class DeploymentTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        state = self.base / "source.state"
        state.write_bytes(b"declared policy revision 10")
        self.root = self.base / "instance"
        deploy.initialize(self.root, state, ["--no-pci", "--", "--ports", "0,1"])
        self.instance = deploy.Deployment(self.root)
        package = self.base / "release.tar"
        self.old, sha = bundle(package)
        self.instance.install(package, sha)
        self.new, sha = bundle(package, "second")
        self.instance.install(package, sha)
        self.instance.select(self.old, None)

    def test_init_existing_directory_keeps_all_data(self):
        before = self.instance.state.read_bytes()
        with self.assertRaises(ValueError):
            deploy.initialize(self.root, self.instance.state, ["--"])
        self.assertEqual(self.instance.state.read_bytes(), before)

    def test_alias_directory_and_foreign_socket_file_are_rejected(self):
        alias = self.base / "alias"
        alias.symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(ValueError):
            deploy.Deployment(alias)
        self.instance.socket.write_text("user file")
        with self.assertRaises(ValueError):
            self.instance.cleanup_dead()
        self.assertEqual(self.instance.socket.read_text(), "user file")

    def test_source_archive_cannot_follow_an_external_directory(self):
        source, outside = self.base / "source", self.base / "outside"
        source.mkdir()
        outside.mkdir()
        (source / "meson.build").write_text("project('example','c')")
        (outside / "private.py").write_text("private_configuration = True")
        (source / "app").symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "符号链接"):
            release.source_files(source)

    def test_install_is_idempotent_and_checks_existing_bytes(self):
        package = self.base / "release.tar"
        _, sha = bundle(package)
        self.assertTrue(self.instance.install(package, sha)["already_present"])
        (self.root / "releases" / self.old / "bin/dppd").write_bytes(b"tampered")
        with self.assertRaises(ValueError):
            self.instance.install(package, sha)

    def test_restore_preflight_does_not_touch_declared_state(self):
        before = self.instance.state.read_bytes()
        self.instance.preflight(self.new)
        self.assertEqual(self.instance.state.read_bytes(), before)
        package = self.base / "incompatible.tar"
        identity, sha = bundle(package, "incompatible", checker=b"#!/bin/sh\nexit 3\n")
        self.instance.install(package, sha)
        with self.assertRaises(subprocess.CalledProcessError):
            self.instance.preflight(identity)
        self.assertEqual(self.instance.state.read_bytes(), before)

    def test_start_failure_rolls_back_with_latest_policy(self):
        policy = b"new durable policy written before candidate failure"
        started = []

        def start(identity):
            started.append(identity)
            if identity == self.new:
                self.instance.state.write_bytes(policy)
                raise RuntimeError("candidate failed")

        with patch.object(self.instance, "stop_process"), patch.object(self.instance, "start_process", side_effect=start):
            with self.assertRaisesRegex(RuntimeError, "已恢复原版本"):
                self.instance.upgrade(self.new)
        self.assertEqual(started, [self.new, self.old])
        self.assertEqual(self.instance.state.read_bytes(), policy)
        self.assertEqual(deploy.read(self.root / "current.json")["release"], self.old)
        self.assertEqual(deploy.read(self.root / "transaction.json")["phase"], "rolled_back")

    def test_incompatible_rollback_keeps_state_and_blocks(self):
        policy = b"new format policy"

        def start(identity):
            self.instance.state.write_bytes(policy)
            raise RuntimeError("candidate failed")

        def preflight(identity):
            if identity == self.old and self.instance.state.read_bytes() == policy:
                raise ValueError("old binary cannot read new state")

        with patch.object(self.instance, "stop_process"), patch.object(self.instance, "start_process", side_effect=start), \
                patch.object(self.instance, "preflight", side_effect=preflight):
            with self.assertRaisesRegex(RuntimeError, "原始策略保留"):
                self.instance.upgrade(self.new)
        self.assertEqual(self.instance.state.read_bytes(), policy)
        self.assertEqual(deploy.read(self.root / "transaction.json")["phase"], "blocked")
        with self.assertRaisesRegex(ValueError, "recover"):
            self.instance.upgrade(self.new)

    def test_ready_candidate_after_interruption_can_commit(self):
        transaction = {"id": "interrupted", "old": self.old, "target": self.new, "phase": "candidate_ready"}
        deploy.save(self.root / "transaction.json", transaction)
        deploy.save(self.root / "run/process.json", {"release": self.new})
        with patch.object(deploy, "alive", return_value=True), \
                patch.object(self.instance, "cli", return_value=subprocess.CompletedProcess([], 0, "", "")):
            result = self.instance.recover()
        self.assertTrue(result["ready"])
        self.assertEqual(result["transaction"]["phase"], "committed")
        self.assertEqual(result["current"]["release"], self.new)

    def test_recovery_does_not_restart_a_live_isolated_instance(self):
        with patch.object(self.instance, "status", return_value={"running": True, "ready": False,
                          "process": {"release": self.old}}), patch.object(self.instance, "stop_process") as stop:
            with self.assertRaisesRegex(RuntimeError, "不能用重启代替恢复"):
                self.instance.recover()
            stop.assert_not_called()

    def test_recovery_does_not_accept_a_different_running_release(self):
        with patch.object(self.instance, "status", return_value={"running": True, "ready": True,
                          "process": {"release": self.new}}), patch.object(self.instance, "stop_process") as stop:
            with self.assertRaisesRegex(ValueError, "实际运行版本"):
                self.instance.recover()
            stop.assert_not_called()

    def test_late_worker_cannot_overwrite_identity_after_launch_cancel(self):
        record = {"identity": dict(deploy.identity(os.getpid()), start="earlier-process"), "release": self.old,
                  "attempt": "current-attempt"}
        deploy.save(self.root / "run/process.json", record)
        deploy.save(self.root / "run/launch.json", {"attempt": "late-attempt", "release": self.new, "enabled": True})
        self.instance.cancel_launch()
        result = subprocess.run([sys.executable, deploy.__file__, "_worker", str(self.root), self.new, "late-attempt"],
                                capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(deploy.read(self.root / "run/process.json"), record)
        self.assertFalse(self.instance.socket.exists())

    def test_late_supervisor_cannot_overwrite_new_exit_receipt(self):
        deploy.save(self.root / "run/process.json", {"release": self.old, "attempt": "current-attempt"})
        current = {"attempt": "current-attempt", "returncode": 0}
        deploy.save(self.root / "run/exit.json", current)
        deploy.save(self.root / "run/launch.json", {"attempt": "current-attempt", "release": self.old, "enabled": True})
        result = subprocess.run([sys.executable, deploy.__file__, "_supervise", str(self.root), self.new, "late-attempt"],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(deploy.read(self.root / "run/exit.json"), current)

    def test_dirty_or_uncollected_jobs_never_signal_daemon(self):
        deploy.save(self.root / "run/process.json", {"release": self.old})
        for status in ("dirty=yes residual=no isolated=no jobs=0", "dirty=no residual=no isolated=no jobs=1"):
            with self.subTest(status=status), patch.object(deploy, "alive", return_value=True), \
                    patch.object(deploy, "terminate_owned") as terminate, \
                    patch.object(self.instance, "cli", return_value=subprocess.CompletedProcess([], 0, status, "")):
                with self.assertRaises(ValueError):
                    self.instance.stop_process()
                terminate.assert_not_called()

    def test_unknown_transaction_process_is_not_stopped(self):
        deploy.save(self.root / "run/process.json", {"release": "f" * 64})
        with patch.object(self.instance, "stop_process") as stop:
            with self.assertRaises(ValueError):
                self.instance.restore_old({"old": self.old, "target": self.new})
            stop.assert_not_called()

    def test_reused_pid_identity_never_receives_signal(self):
        record = {"identity": dict(deploy.identity(os.getpid()), start="different-start")}
        with patch.object(signal, "pidfd_send_signal") as terminate:
            deploy.terminate_owned(record)
            terminate.assert_not_called()

    def test_live_socket_is_never_removed_even_with_recorded_inode(self):
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as listener:
            listener.bind(str(self.instance.socket))
            listener.listen(1)
            metadata = self.instance.socket.lstat()
            deploy.save(self.root / "run/process.json", {
                "identity": dict(deploy.identity(os.getpid()), start="old-start"),
                "socket": {"device": metadata.st_dev, "inode": metadata.st_ino}})
            with self.assertRaisesRegex(ValueError, "仍被某个进程监听"):
                self.instance.cleanup_dead()
            self.assertTrue(self.instance.socket.exists())

    def test_real_process_identity_exit_receipt_and_crash_restart(self):
        # 这里只模拟协议内容，进程、exec、pidfd、socket、退出码和文件持久化都经过真实 Linux 接口
        daemon = b'''#!/usr/bin/python3
import os,signal,socket,sys
path=sys.argv[sys.argv.index('--control-socket')+1]
running=True
def stop(*unused):
    global running
    running=False
signal.signal(signal.SIGTERM,stop)
listener=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET)
listener.bind(path)
listener.listen(4)
listener.settimeout(0.05)
while running:
    try:
        connection,_=listener.accept()
    except socket.timeout:
        continue
    with connection:
        operation=connection.recv(64)
        connection.send(b'dirty=no residual=no isolated=no jobs=0' if operation==b'flow-status' else b'ready=yes')
listener.close()
os.unlink(path)
'''
        cli = b'''#!/usr/bin/python3
import socket,sys
with socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET) as client:
    client.connect(sys.argv[2])
    client.send(sys.argv[3].encode())
    print(client.recv(512).decode())
'''
        package = self.base / "process.tar"
        identity, sha = bundle(package, "real-process", daemon=daemon, cli=cli)
        self.instance.install(package, sha)
        self.addCleanup(lambda: self.instance.stop_process(require_clean=False))
        result = self.instance.upgrade(identity)
        self.assertTrue(result["ready"])
        record = deploy.read(self.root / "run/process.json")
        self.assertTrue(deploy.alive(record))
        self.assertIn("socket", record)
        original_policy = self.instance.state.read_bytes()
        descriptor = os.pidfd_open(record["identity"]["pid"], 0)
        signal.pidfd_send_signal(descriptor, signal.SIGKILL)
        os.close(descriptor)
        deadline = time.monotonic() + 5
        while deploy.alive(record) and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertFalse(deploy.alive(record))
        self.assertTrue(self.instance.recover()["ready"])
        replacement = deploy.read(self.root / "run/process.json")
        self.assertNotEqual(replacement["attempt"], record["attempt"])
        self.assertEqual(self.instance.state.read_bytes(), original_policy)
        self.instance.stop_process()
        self.assertFalse(self.instance.status()["running"])
        receipt = deploy.read(self.root / "run/exit.json")
        self.assertEqual(receipt, {"attempt": replacement["attempt"], "returncode": 0})

    def test_deployment_lock_excludes_second_writer(self):
        with self.instance.lock():
            with self.assertRaises(BlockingIOError):
                with deploy.Deployment(self.root).lock():
                    self.fail("second writer acquired the lock")

    def test_rolling_stops_before_insufficient_ready_instances(self):
        second = self.base / "second"
        deploy.initialize(second, self.instance.state, ["--"])
        with patch.object(deploy.Deployment, "preflight"), \
                patch.object(deploy.Deployment, "status", return_value={"ready": False}), \
                patch.object(deploy.Deployment, "upgrade") as upgrade:
            with self.assertRaisesRegex(RuntimeError, "就绪数量不足"):
                deploy.rolling([str(self.root), str(second)], self.new, 1, 10)
            upgrade.assert_not_called()


if __name__ == "__main__":
    unittest.main(verbosity=2)
