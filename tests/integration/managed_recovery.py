#!/usr/bin/env python3
"""只在正式启动器创建的 TAP 内验证崩溃、清理日志、拒绝和快照重放"""

import argparse
import json
import os
from pathlib import Path
import pwd
import re
import select
import signal
import struct
import subprocess
import tempfile
import time
import zlib

from batch_update import check, fields
from health_readiness import clean_environment


def run_case(build, native, software=False, window=None, reject=None):
    environment = clean_environment()
    environment.pop("DPPD_SCOPE_FD", None)
    # 私有前缀安装的动态库必须允许服务账户读取，优先使用当前构建的非 root 所有者
    user = pwd.getpwuid(build.stat().st_uid) if build.stat().st_uid != 0 else pwd.getpwnam("nobody")
    before = set(Path("/sys/class/net").iterdir())
    with tempfile.TemporaryDirectory(prefix="dppd-managed-") as temporary:
        directory = Path(temporary)
        directory.chmod(0o711)
        storage = directory / "storage"
        storage.mkdir(mode=0o700)
        os.chown(storage, user.pw_uid, user.pw_gid)
        log = directory / "scope.log"
        command = [str(build / ("dppd-scope-test" if window else "dppd-scope")), "run", str(storage),
                   str(user.pw_uid), str(user.pw_gid)]
        if native:
            command.append("--native")
        with log.open("w") as output:
            supervisor = subprocess.Popen(command, env=environment, stdout=output, stderr=subprocess.STDOUT)
        namespace_fd = None
        pending_cli = None
        interfaces = ["dppdscope0", "dppdscope1"]

        def wait(predicate, label):
            deadline = time.monotonic() + 20
            while not predicate():
                check(supervisor.poll() is None, f"scope exited waiting for {label}\n{log.read_text()}")
                check(time.monotonic() < deadline, f"timeout waiting for {label}\n{log.read_text()}")
                time.sleep(0.03)

        def run(*arguments, failure=False):
            result = subprocess.run(list(map(str, arguments)), env=environment, capture_output=True, text=True, timeout=8)
            check((result.returncode != 0) == failure, result.stdout + result.stderr + "\n" + log.read_text())
            return result.stdout + result.stderr

        def ctl(*arguments):
            return run(build / "dppctl", "--socket", storage / "control", *arguments)

        def ready():
            if not (storage / "control").exists():
                return False
            result = subprocess.run([str(build / "dppctl"), "--socket", str(storage / "control"), "health"],
                                    env=environment, capture_output=True, text=True, timeout=3)
            return result.returncode == 0 and fields(result.stdout).get("ready") == "yes"

        def filters():
            objects = []
            for interface in interfaces:
                for parent in ("1:", "ffff:"):
                    output = run("nsenter", f"--net=/proc/{os.getpid()}/fd/{namespace_fd}", "tc", "-j", "filter", "show",
                                 "dev", interface, "parent", parent)
                    objects.extend(row for row in json.loads(output) if row.get("options", {}).get("handle"))
            return objects

        try:
            wait(ready, "initial business readiness")
            namespace_fd = os.open(f"/proc/{supervisor.pid}/ns/net", os.O_RDONLY | os.O_CLOEXEC)
            policy = "software" if software else "require"
            ctl("apply-drop", 1101, 0, 0, 9, policy)
            if software or reject == "unused-port":
                ctl("apply-drop", 1102, 1, 0, 10, "software")
            else:
                ctl("apply-filter", 1102, 1, 0, "udp", "any", "198.18.0.2/32", "any", 5678,
                    "queue:0", "priority:10", policy)
            saved = (storage / "snapshot").read_bytes()
            versions = [fields(ctl("get", identifier))["generation"] for identifier in (1101, 1102)]
            old_filters = filters()
            check(len(old_filters) == (0 if software else 1 if reject == "unused-port" else 2), old_filters)
            # 普通外部工具连恢复锁都不能取得，更不能凭磁盘会话发起删除
            protected = (storage / "recovery").read_bytes()
            revision = struct.unpack_from("<Q", protected, 16)[0]
            refused = run(build / "dppd-recovery", "clean-managed", storage / "recovery", revision,
                          next(storage.glob("session-*")), failure=True)
            check("requires a live supervisor scope" in refused, refused)
            check((storage / "recovery").read_bytes() == protected, "unauthorized command changed recovery state")
            businesses = re.findall(r"scope-actor role=2 pid=(\d+)", log.read_text())
            check(len(businesses) == 1, log.read_text())
            business = int(businesses[0])
            if window == "intent":
                (storage / "recovery.business-window").touch()
                pending_cli = subprocess.Popen([str(build / "dppctl"), "--socket", str(storage / "control"),
                                                "apply-drop", "1110", "0", "0", "11", "require"],
                                               env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                wait(lambda: "business-window phase=intent" in log.read_text(), "business create result window")
                original = (storage / "recovery").read_bytes()
                check(original[9280 + 2 * 176 + 26] == 1, "last creation result was already committed")
                check(len(filters()) == 3, "intent test did not create its real kernel object")
                check((storage / "snapshot").read_bytes() == saved, "pending transaction changed snapshot")
            elif window:
                (storage / "recovery.test-window").write_text(window)
            os.kill(business, signal.SIGKILL)
            if pending_cli is not None:
                pending_cli.communicate(timeout=5)
                check(pending_cli.returncode != 0, "interrupted transaction reported success")
            if window and window != "intent":
                wait(lambda: f"cleanup-window mode={window}" in log.read_text(), "cleanup fault window")
                if window in ("before-delete", "after-delete", "after-result", "after-ack"):
                    journal = next(storage.glob("recovery.cleanup.*")).read_bytes()
                    check(struct.unpack_from("<I", journal, 136)[0] ==
                          (3 if window in ("after-result", "after-ack") else 2), "wrong durable progress at pause")
                    count = 2 if window == "before-delete" else 0 if window == "after-ack" else 1
                    check(len(filters()) == count, "wrong kernel state at pause")
                    cleaner = int(re.findall(r"scope-actor role=3 pid=(\d+)", log.read_text())[-1])
                    os.kill(cleaner, signal.SIGKILL)
            if reject:
                def net(*arguments):
                    return run("nsenter", f"--net=/proc/{os.getpid()}/fd/{namespace_fd}", *arguments)

                cookie = old_filters[0]["options"]["actions"][0]["cookie"]
                first = old_filters[0]
                monitor = int(re.findall(r"scope-actor role=1 pid=(\d+)", log.read_text())[-1])
                cleaner = int(re.findall(r"scope-actor role=3 pid=(\d+)", log.read_text())[-1])
                actor_fds = [os.pidfd_open(pid) for pid in (monitor, cleaner)]
                coordinate = ["dev", interfaces[0], "parent", "1:", "protocol", first["protocol"],
                              "pref", first["pref"], "handle", first["options"]["handle"], "flower"]
                if reject in ("duplicate", "foreign", "shared-action", "unused-port"):
                    extra = ["tc", "filter", "add", "dev", interfaces[1], "parent", "1:", "protocol", "all",
                             "pref", 59999, "handle", "0xaba", "flower", "action"]
                    if reject == "shared-action":
                        extra += ["gact", "index", first["options"]["actions"][0]["index"]]
                    else:
                        extra += ["drop"]
                        if reject in ("duplicate", "unused-port"):
                            extra += ["cookie", cookie]
                    net(*extra)
                elif reject == "content":
                    net("tc", "filter", "replace", *coordinate, "action", "skbedit", "queue_mapping", 0, "cookie", cookie)
                elif reject == "scope":
                    net("tc", "filter", "delete", *coordinate)
                    coordinate[coordinate.index("pref") + 1] = 59998
                    net("tc", "filter", "add", *coordinate, "action", "drop", "cookie", cookie)
                elif reject == "interface":
                    net("ip", "link", "set", interfaces[0], "name", "dppdrenamed")
                    interfaces[0] = "dppdrenamed"
                elif reject == "journal":
                    journal_path = next(storage.glob("recovery.cleanup.*"))
                    with journal_path.open("r+b") as journal:
                        journal.seek(100)
                        journal.write(bytes([1]))
                        journal.flush()
                        os.fsync(journal.fileno())
                expected_count = len(filters())
                if reject in ("monitor-dead", "monitor-paused"):
                    os.kill(monitor, signal.SIGKILL if reject == "monitor-dead" else signal.SIGSTOP)
                if reject == "supervisor":
                    supervisor.kill()
                elif reject == "journal":
                    os.kill(cleaner, signal.SIGKILL)
                else:
                    try:
                        os.kill(cleaner, signal.SIGCONT)
                    except ProcessLookupError:
                        pass
                supervisor.wait(timeout=15)
                try:
                    poller = select.poll()
                    for fd in actor_fds:
                        poller.register(fd, select.POLLIN)
                    deadline = time.monotonic() + 5
                    remaining = set(actor_fds)
                    while remaining and time.monotonic() < deadline:
                        for fd, _ in poller.poll(100):
                            remaining.discard(fd)
                            poller.unregister(fd)
                    check(not remaining, "failed scope left a privileged actor alive")
                finally:
                    for fd in actor_fds:
                        os.close(fd)
                check(supervisor.returncode != 0 and "scope-replay" not in log.read_text(), log.read_text())
                check(len(filters()) == expected_count, "rejected cleanup deleted a kernel object")
                check((storage / "snapshot").read_bytes() == saved, "rejected cleanup changed snapshot")
                show = run(build / "dppd-recovery", "show", storage / "recovery")
                check("external-reconciliation-required" in show, "rejection cleared the recovery guard")
                if reject != "journal":
                    check(not list(storage.glob("recovery.cleanup.*")), "rejection started durable removal")
                reasons = {"duplicate": "token-ambiguous", "shared-action": "token-ambiguous", "unused-port": "token-ambiguous",
                           "foreign": "unassociated-filters", "content": "content-mismatch", "scope": "scope-mismatch"}
                if reject in reasons:
                    check(f"reason={reasons[reject]}" in log.read_text(), log.read_text())
                if reject == "supervisor":
                    protected = (storage / "recovery").read_bytes()
                    refused = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=8)
                    check(refused.returncode != 0 and "scope-actor" not in refused.stdout,
                          "new supervisor adopted an interrupted native recovery scope")
                    check((storage / "recovery").read_bytes() == protected, "new scope rewrote interrupted evidence")
                print(f"PASS managed recovery refused={reject}", flush=True)
            elif native or software:
                wait(lambda: len(re.findall(r"scope-actor role=2 pid=(\d+)", log.read_text())) == 2, "new business")
                wait(ready, "replayed business readiness")
                check((storage / "snapshot").read_bytes() == saved, "replay changed the durable rule snapshot")
                for index, identifier in enumerate((1101, 1102)):
                    check(fields(ctl("get", identifier))["generation"] == versions[index], "replay changed generation")
                    status = ctl("rule-status", identifier)
                    check(f"backend={'software' if software else 'rte_flow'}" in status, status)
                check(len(filters()) == (0 if software else 2), "replay left duplicates or omitted a rule")
                journals = list(storage.glob("recovery.cleanup.*"))
                check(len(journals) == (0 if software else 1), journals)
                if journals:
                    data = journals[0].read_bytes()
                    check(len(data) == 4224 and data[:8] == b"DPPCLN1\0", "invalid cleanup journal")
                    expected = struct.unpack_from("<I", data, 20)[0]
                    check(zlib.crc32(data[:20] + bytes(4) + data[24:]) == expected, "journal CRC mismatch")
                    count = struct.unpack_from("<I", data, 16)[0]
                    check(count == (3 if window == "intent" else 2) and
                          all(struct.unpack_from("<I", data, 136 + 16 * i)[0] == 3 for i in range(count)),
                          "journal did not commit every removal")
                if window and window not in ("intent", "after-ack"):
                    check("scope-cleanup-retry" in log.read_text(), "cleanup did not retry within the original scope")
                    if window == "delete-fail":
                        check("cleanup-window failure-saved error=-5" in log.read_text(), "delete failure was not durable")
                supervisor.send_signal(signal.SIGINT)
                supervisor.wait(timeout=10)
                check(supervisor.returncode == 0, log.read_text())
                show = run(build / "dppd-recovery", "show", storage / "recovery")
                check("state=clean" in show and "attempts=0" in show, show)
                check(not (storage / "control").exists(), "normal shutdown leaked the control socket")
                check(not filters(), "normal shutdown left a TC filter")
            else:
                supervisor.wait(timeout=15)
                check(supervisor.returncode != 0 and "scope-replay" not in log.read_text(), log.read_text())
                show = run(build / "dppd-recovery", "show", storage / "recovery")
                check("external-reconciliation-required" in show, show)
                check(not list(storage.glob("recovery.cleanup.*")), "unsupported cleanup created a progress journal")
                check("reason=missing-token" in log.read_text(), log.read_text())
                check(len(filters()) == len(old_filters), "unsupported cleanup removed a kernel object")
                refused = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=8)
                check(refused.returncode != 0 and "scope-actor" not in refused.stdout,
                      "new scope adopted a dirty recovery record")
            if not reject:
                print(f"PASS managed recovery native={native} software={software} window={window}", flush=True)
        except Exception:
            print(log.read_text(), flush=True)
            raise
        finally:
            if supervisor.poll() is None:
                supervisor.kill()
                supervisor.wait(timeout=5)
            if namespace_fd is not None:
                os.close(namespace_fd)
            if pending_cli is not None and pending_cli.poll() is None:
                pending_cli.kill()
                pending_cli.communicate(timeout=5)
        check(set(Path("/sys/class/net").iterdir()) == before, "managed recovery changed host interfaces")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--native", action="store_true")
    arguments = parser.parse_args()
    check(os.geteuid() == 0, "managed recovery integration requires root")
    build = arguments.build_dir.resolve()
    run_case(build, arguments.native)
    run_case(build, arguments.native, software=True)
    if arguments.native:
        for window in ("before-delete", "after-delete", "delete-fail", "commit-fail", "after-result", "after-ack", "intent"):
            run_case(build, True, window=window)
        for rejected in ("duplicate", "foreign", "shared-action", "unused-port", "content", "scope", "interface",
                         "monitor-dead", "monitor-paused", "supervisor"):
            run_case(build, True, window="before-inventory", reject=rejected)
        run_case(build, True, window="journal-corrupt", reject="journal")


if __name__ == "__main__":
    main()
