#!/usr/bin/env python3
"""用真实 TAP 检查原生标识、同坐标替换、复制标识和旧驱动明确拒绝"""

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile

from batch_update import check, fields
from health_readiness import clean_environment, daemon_command
from recovery_isolation import wait_for


def run_case(build, unsupported=False, remote=False, omit_owner=False):
    """只创建和删除测试自身的持久 TAP，所有退出路径都核对 ifindex 后清理"""
    environment = clean_environment()
    owned, daemon = {}, None

    def run(*args, failure=False):
        """每次调用有超时，预期失败也必须核对退出码"""
        result = subprocess.run(list(map(str, args)), env=environment,
                                capture_output=True, text=True, timeout=15)
        check(result.returncode != 0 if failure else result.returncode == 0,
              result.stdout + result.stderr)
        return result.stdout + result.stderr

    def remove_interfaces():
        """只清理成功创建且未被其他进程替换的测试资源"""
        for name, index in tuple(owned.items()):
            check(Path(f"/sys/class/net/{name}/ifindex").read_text() == index, "interface replaced")
            run("ip", "link", "delete", "dev", name)
            del owned[name]

    def filters(name):
        """tc JSON 独立核对内核实际动作标识，跳过没有 handle 的分类器标题"""
        return [row for row in json.loads(run("tc", "-j", "filter", "show", "dev", name, "parent", "1:"))
                if row.get("options", {}).get("handle")]

    with tempfile.TemporaryDirectory(prefix="dppd-owner-") as temporary:
        directory = Path(temporary)
        (directory / "storage").mkdir()
        interfaces = (f"do{os.getpid()}a", f"do{os.getpid()}b")
        remote_name = f"do{os.getpid()}r"
        guard, sock, log = directory / "recovery", directory / "ctl.sock", directory / "daemon.log"
        command = daemon_command(build, directory, 1, interfaces) + [
            "--recovery-path", str(guard), "--tap-owner-cookie"]
        if remote:
            index = next(i for i, token in enumerate(command) if token.startswith("--vdev=net_tap0,"))
            command[index] += f",remote={remote_name}"
        try:
            for name in (*interfaces, *((remote_name,) if remote else ())):
                check(not Path(f"/sys/class/net/{name}").exists(), "interface already exists")
                if name == remote_name:
                    # 自有 dummy 提供稳定的远端链路，避免未挂接队列的 TAP 一直报告链路断开
                    run("ip", "link", "add", "dev", name, "type", "dummy")
                else:
                    run("ip", "tuntap", "add", "dev", name, "mode", "tap", "multi_queue")
                owned[name] = Path(f"/sys/class/net/{name}/ifindex").read_text()
                if name == remote_name:
                    run("ip", "link", "set", "dev", name, "up")
            with log.open("w") as output:
                daemon_env = dict(environment)
                if omit_owner:
                    daemon_env.update(LD_PRELOAD=str(build / "tests/integration/libdppd_recovery_attempt_faults.so"),
                                      DPPD_TEST_RECOVERY_WINDOW="omit-owner")
                daemon = subprocess.Popen(command, env=daemon_env, stdout=output, stderr=subprocess.STDOUT)
            wait_for(daemon, sock.exists, "management socket")
            wait_for(daemon, lambda: fields(run(build / "dppctl", "--socket", sock, "health"))["ready"] == "yes",
                     "worker readiness")
            original_state = (directory / "storage/state.bin").read_bytes()
            ctl = [build / "dppctl", "--socket", sock]
            rejected = unsupported or remote or omit_owner
            result = run(*ctl, "apply-drop", 980, 0, 0, 9, "require", failure=rejected)
            if rejected:
                check("(-117)" in result if omit_owner else "(-95)" in result, result)
                if omit_owner:
                    check("TAP owner readback not unique: 0; rolling back" in log.read_text(), log.read_text())
                check(not filters(interfaces[0]), "rejected request created a local filter")
                check((directory / "storage/state.bin").read_bytes() == original_state, "rejection published state")
                daemon.send_signal(signal.SIGINT)
                daemon.wait(timeout=10)
                check(daemon.returncode == 0, log.read_text())
                show = run(build / "dppd-recovery", "show", guard)
                last_attempt = 1 if omit_owner else 0
                check("state=clean" in show and f"attempts=0 last-attempt={last_attempt}" in show, show)
                check("owner-token " not in show, show)
            else:
                run(*ctl, "apply-filter", 981, 1, 0, "ipv4", "any", "198.18.0.2/32", "any", "any",
                    "queue:0", "priority:10", "require")
                # 正常删除必须释放带标识的真实 handle，不影响前面两个对象
                run(*ctl, "apply-drop", 982, 0, 0, 11, "require")
                generation = fields(run(*ctl, "get", 982))["generation"]
                run(*ctl, "delete", 982, generation)
                check(len(filters(interfaces[0])) == 1 and len(filters(interfaces[1])) == 1,
                      "normal removal left a filter or removed another rule")
                daemon.kill()
                daemon.wait(timeout=10)
                check(daemon.returncode == -signal.SIGKILL, log.read_text())
                show = run(build / "dppd-recovery", "show", guard)
                revision = fields(show.splitlines()[0])["revision"]
                tokens = [fields(line) for line in show.splitlines() if line.startswith("owner-token ")]
                check(len(tokens) == 3 and len({row["cookie"] for row in tokens}) == 3, show)
                snapshot, saved_guard = (directory / "storage/state.bin").read_bytes(), guard.read_bytes()

                def inspect():
                    """每次观察都必须保留恢复文件和业务快照，不能顺带自动确认清理"""
                    output = run(build / "dppd-recovery", "inspect", guard, revision)
                    check(guard.read_bytes() == saved_guard and (directory / "storage/state.bin").read_bytes() == snapshot,
                          "inspection changed persisted files")
                    check("cleanup-confirmed=no" in output and "content=unchecked" in output, output)
                    return output

                observed = inspect()
                check(observed.count("status=token-present owner=token-match") == 2, observed)
                check("status=removed-record owner=unknown" in observed, observed)
                actual = [filters(name) for name in interfaces]
                for index in range(2):
                    action = actual[index][0]["options"]["actions"]
                    check(len(action) == 1 and action[0].get("cookie", "").lower() == tokens[index]["cookie"],
                          str(actual))
                check([filters(name) for name in interfaces] == actual, "inspection changed TC filters")
                # 同一随机标识被复制到另一个对象时必须判为有歧义
                cookie = tokens[0]["cookie"]
                run("tc", "filter", "add", "dev", interfaces[0], "parent", "1:", "protocol", "all",
                    "pref", 60000, "handle", "0xabc", "flower", "action", "drop", "cookie", cookie)
                check("status=token-ambiguous owner=unknown" in inspect(), "duplicate token accepted")
                run("tc", "filter", "delete", "dev", interfaces[0], "parent", "1:", "protocol", "all",
                    "pref", 60000, "handle", "0xabc", "flower")
                # 删掉原对象后在相同坐标创建无标识对象，坐标仍在也不能认为原对象仍在
                row = actual[0][0]
                coordinate = ["dev", interfaces[0], "parent", "1:", "protocol", row["protocol"],
                              "pref", row["pref"], "handle", row["options"]["handle"], "flower"]
                run("tc", "filter", "delete", *coordinate)
                run("tc", "filter", "add", *coordinate, "action", "drop")
                check("status=token-absent owner=unknown" in inspect(), "same-coordinate replacement misidentified")
                check("state=external-reconciliation-required" in run(build / "dppd-recovery", "show", guard),
                      "inspection cleared recovery guard")
                remove_interfaces()
                run(build / "dppd-recovery", "acknowledge-clean", guard, revision, "--external-cleanup-complete")
        except Exception as error:
            raise AssertionError(log.read_text() if log.exists() else "daemon not started") from error
        finally:
            if daemon is not None and daemon.poll() is None:
                daemon.kill()
                daemon.wait(timeout=10)
            remove_interfaces()
    print(f"PASS native TAP cookie unsupported={unsupported} remote={remote} omit-owner={omit_owner}: "
          "driver result, durable token, kernel inventory, read-only reconciliation boundaries", flush=True)


def main():
    """系统驱动用于拒绝测试，带补丁的独立构建用于真实标识及远端范围测试"""
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--expect-unsupported", action="store_true")
    modes.add_argument("--remote", action="store_true")
    modes.add_argument("--omit-owner", action="store_true")
    args = parser.parse_args()
    check(os.geteuid() == 0, "native TAP cookie tests need root")
    run_case(args.build_dir.resolve(), args.expect_unsupported, args.remote, args.omit_owner)


if __name__ == "__main__":
    main()
