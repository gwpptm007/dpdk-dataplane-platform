#!/usr/bin/env python3
"""用真实 TAP 检查原生标识、同坐标替换、复制标识和旧驱动明确拒绝"""

import argparse
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import tempfile
import zlib

from batch_update import check, fields
from health_readiness import clean_environment, daemon_command
from recovery_isolation import wait_for


def run_case(build, unsupported=False, remote=False, omit_owner=False, alter_content=False, ordinary_remote=False):
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
        if ordinary_remote:
            command.remove("--tap-owner-cookie")
            remote = True
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
                if omit_owner or alter_content:
                    daemon_env.update(LD_PRELOAD=str(build / "tests/integration/libdppd_recovery_attempt_faults.so"),
                                      DPPD_TEST_RECOVERY_WINDOW="omit-owner" if omit_owner else "alter-content")
                daemon = subprocess.Popen(command, env=daemon_env, stdout=output, stderr=subprocess.STDOUT)
            wait_for(daemon, sock.exists, "management socket")
            wait_for(daemon, lambda: fields(run(build / "dppctl", "--socket", sock, "health"))["ready"] == "yes",
                     "worker readiness")
            original_state = (directory / "storage/state.bin").read_bytes()
            ctl = [build / "dppctl", "--socket", sock]
            rejected = unsupported or (remote and not ordinary_remote) or omit_owner or alter_content
            result = run(*ctl, "apply-drop", 980, 0, 0, 9, "require", failure=rejected)
            if ordinary_remote:
                # 补丁不得让普通远端镜像的空动作列表崩溃，正常安装和删除都必须完成
                check(len(filters(interfaces[0])) == 1, "ordinary remote rule missing")
                generation = fields(run(*ctl, "get", 980))["generation"]
                run(*ctl, "delete", 980, generation)
                check(not filters(interfaces[0]), "ordinary remote removal failed")
                daemon.send_signal(signal.SIGINT)
                daemon.wait(timeout=10)
                check(daemon.returncode == 0, log.read_text())
                check("state=clean" in run(build / "dppd-recovery", "show", guard), "ordinary remote guard not clean")
            elif rejected:
                check("(-117)" in result if omit_owner or alter_content else "(-95)" in result, result)
                if omit_owner:
                    check("TAP owner readback not unique: 0; rolling back" in log.read_text(), log.read_text())
                if alter_content:
                    check("TAP rule readback failed: content=-116 scope=0; rolling back" in log.read_text(), log.read_text())
                check(not filters(interfaces[0]), "rejected request created a local filter")
                check((directory / "storage/state.bin").read_bytes() == original_state, "rejection published state")
                daemon.send_signal(signal.SIGINT)
                daemon.wait(timeout=10)
                check(daemon.returncode == 0, log.read_text())
                show = run(build / "dppd-recovery", "show", guard)
                last_attempt = 1 if omit_owner or alter_content else 0
                check("state=clean" in show and f"attempts=0 last-attempt={last_attempt}" in show, show)
                check("owner-token " not in show, show)
            else:
                run(*ctl, "apply-filter", 981, 1, 0, "ipv4", "any", "198.18.0.2/32", "any", "any",
                    "queue:0", "priority:10", "require")
                run(*ctl, "apply-filter", 983, 0, 0, "udp", "198.18.1.0/24", "198.18.2.3/32", 1234, 5678,
                    "drop", "priority:12", "require")
                run(*ctl, "apply-filter", 984, 1, 0, "tcp", "198.18.3.4/32", "198.18.4.0/24", "any", 443,
                    "queue:0", "priority:13", "require")
                # 正常删除必须释放带标识的真实 handle，不影响前面两个对象
                run(*ctl, "apply-drop", 982, 0, 0, 11, "require")
                generation = fields(run(*ctl, "get", 982))["generation"]
                run(*ctl, "delete", 982, generation)
                check(len(filters(interfaces[0])) == 2 and len(filters(interfaces[1])) == 2,
                      "normal removal left a filter or removed another rule")
                daemon.kill()
                daemon.wait(timeout=10)
                check(daemon.returncode == -signal.SIGKILL, log.read_text())
                show = run(build / "dppd-recovery", "show", guard)
                revision = fields(show.splitlines()[0])["revision"]
                tokens = [fields(line) for line in show.splitlines() if line.startswith("owner-token ")]
                check(len(tokens) == 5 and len({row["cookie"] for row in tokens}) == 5, show)
                snapshot, saved_guard = (directory / "storage/state.bin").read_bytes(), guard.read_bytes()

                def inspect():
                    """每次观察都必须保留恢复文件和业务快照，不能顺带自动确认清理"""
                    output = run(build / "dppd-recovery", "inspect", guard, revision)
                    check(guard.read_bytes() == saved_guard and (directory / "storage/state.bin").read_bytes() == snapshot,
                          "inspection changed persisted files")
                    check("cleanup-confirmed=no" in output and "content=unchecked" in output, output)
                    return output

                observed = inspect()
                check(observed.count("status=token-present owner=token-match content=match scope=match") == 4, observed)
                check("status=removed-record owner=unknown" in observed, observed)
                actual = [filters(name) for name in interfaces]
                for index in range(2):
                    action = actual[index][0]["options"]["actions"]
                    check(len(action) == 1 and action[0].get("cookie", "").lower() == tokens[index]["cookie"],
                          str(actual))
                check([filters(name) for name in interfaces] == actual, "inspection changed TC filters")
                # 旧 v4 仍可关联原生标识，但缺少预期内容，检查不能自动升级或补猜
                legacy = bytearray(saved_guard[:9280])
                for slot in range(256):
                    legacy.extend(saved_guard[9280 + slot * 176:9280 + slot * 176 + 112])
                struct.pack_into("<II", legacy, 8, 4, len(legacy))
                struct.pack_into("<I", legacy, 28, 0)
                struct.pack_into("<I", legacy, 28, zlib.crc32(legacy))
                legacy_path = directory / "legacy-v4"
                legacy_path.write_bytes(legacy)
                legacy_path.chmod(0o600)
                legacy_output = run(build / "dppd-recovery", "inspect", legacy_path, revision)
                check(legacy_output.count("owner=token-match content=unchecked scope=unchecked") == 4, legacy_output)
                check(legacy_path.read_bytes() == legacy, "inspection upgraded v4")

                def correlation(rule):
                    """按业务规则选中诊断行，避免另一条匹配记录掩盖本次修改"""
                    return next(line for line in inspect().splitlines() if line.startswith("correlation ") and
                                fields(line)["rule"] == str(rule))

                # tc 命令把 IP 规则挂在 protocol ip，先确认只有范围不同，再逐项修改内容
                udp_row = actual[0][1]
                udp_old = ["dev", interfaces[0], "parent", "1:", "protocol", udp_row["protocol"],
                           "pref", udp_row["pref"], "handle", udp_row["options"]["handle"], "flower"]
                run("tc", "filter", "delete", *udp_old)
                udp_new = list(udp_old)
                udp_new[udp_new.index("protocol") + 1] = "ip"
                original_match = ["ip_proto", "udp", "src_ip", "198.18.1.0/24", "dst_ip", "198.18.2.3/32",
                                  "src_port", 1234, "dst_port", 5678]
                variations = [(None, None), ("src_ip", "198.18.9.0/24"), ("dst_ip", "198.18.2.0/24"),
                              ("src_port", 1235), ("dst_port", 5679), ("ip_proto", "tcp")]
                for key, value in variations:
                    match = list(original_match)
                    if key:
                        match[match.index(key) + 1] = value
                    run("tc", "filter", "add", *udp_new, *match, "action", "drop", "cookie", tokens[2]["cookie"])
                    expected_content = "mismatch" if key else "match"
                    line = correlation(983)
                    check(f"owner=token-match content={expected_content} scope=mismatch" in line, line)
                    run("tc", "filter", "delete", *udp_new)
                run("tc", "filter", "add", *udp_new, *original_match, "action", "drop", "cookie", tokens[2]["cookie"])

                # 先保留原 IPv4 条件和 QUEUE 0，再仅改成 QUEUE 1，验证实际队列编号参与比较
                queue_row = actual[1][0]
                queue_coordinate = ["dev", interfaces[1], "parent", "1:", "protocol", queue_row["protocol"],
                                    "pref", queue_row["pref"], "handle", queue_row["options"]["handle"], "flower"]
                run("tc", "filter", "delete", *queue_coordinate)
                queue_coordinate[queue_coordinate.index("protocol") + 1] = "ip"
                for queue in (0, 1):
                    run("tc", "filter", "add", *queue_coordinate, "dst_ip", "198.18.0.2/32", "action", "skbedit",
                        "queue_mapping", queue, "cookie", tokens[1]["cookie"])
                    line = correlation(981)
                    check(f"content={'match' if queue == 0 else 'mismatch'} scope=mismatch" in line, line)
                    run("tc", "filter", "delete", *queue_coordinate)
                run("tc", "filter", "add", *queue_coordinate, "dst_ip", "198.18.0.2/32", "action", "skbedit",
                    "queue_mapping", 0, "cookie", tokens[1]["cookie"])
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
                # 同一标识和坐标上的动作改变，不得继续报告内容匹配
                run("tc", "filter", "replace", *coordinate, "action", "skbedit", "queue_mapping", 0, "cookie", cookie)
                check("owner=token-match content=mismatch scope=match" in correlation(980), "changed action accepted")
                run("tc", "filter", "replace", *coordinate, "action", "skbedit", "queue_mapping", 1, "cookie", cookie)
                check("owner=token-match content=mismatch scope=match" in correlation(980), "changed queue accepted")
                # 增加 MAC 条件超出允许列表，不能只比较已认识的字段
                run("tc", "filter", "delete", *coordinate)
                run("tc", "filter", "add", *coordinate, "dst_mac", "02:00:00:00:00:01", "action", "drop", "cookie", cookie)
                check("owner=token-match content=unsupported scope=unsupported" in correlation(980), "extra match ignored")
                run("tc", "filter", "delete", *coordinate)
                run("tc", "filter", "add", *coordinate, "action", "drop", "cookie", cookie)
                # 同一个动作再绑定到另一个规则，内容未改但作用范围已经扩大
                action_index = filters(interfaces[0])[0]["options"]["actions"][0]["index"]
                run("tc", "filter", "add", "dev", interfaces[1], "parent", "1:", "protocol", "all",
                    "pref", 60001, "handle", "0xabd", "flower", "action", "gact", "index", action_index)
                check("owner=token-match content=match scope=shared-action" in correlation(980), "shared action accepted")
                run("tc", "filter", "delete", "dev", interfaces[1], "parent", "1:", "protocol", "all",
                    "pref", 60001, "handle", "0xabd", "flower")
                # 移动优先级后保留同一标识，内容相同也必须报告范围不符
                run("tc", "filter", "delete", *coordinate)
                for field, value in (("pref", 60002), ("parent", "ffff:"), ("chain", 1)):
                    moved = list(coordinate)
                    if field == "chain":
                        moved[-1:-1] = [field, value]
                    else:
                        moved[moved.index(field) + 1] = value
                    run("tc", "filter", "add", *moved, "action", "drop", "cookie", cookie)
                    check("owner=token-match content=match scope=mismatch" in correlation(980), "changed scope accepted")
                    run("tc", "filter", "delete", *moved)
                run("tc", "filter", "add", *coordinate, "action", "drop", "cookie", cookie)
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
    print(f"PASS native TAP cookie unsupported={unsupported} remote={remote} omit-owner={omit_owner} "
          f"alter-content={alter_content} ordinary-remote={ordinary_remote}: "
          "driver result, durable token, kernel inventory, read-only reconciliation boundaries", flush=True)


def main():
    """系统驱动用于拒绝测试，带补丁的独立构建用于真实标识及远端范围测试"""
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--expect-unsupported", action="store_true")
    modes.add_argument("--remote", action="store_true")
    modes.add_argument("--omit-owner", action="store_true")
    modes.add_argument("--alter-content", action="store_true")
    modes.add_argument("--ordinary-remote", action="store_true")
    args = parser.parse_args()
    check(os.geteuid() == 0, "native TAP cookie tests need root")
    run_case(args.build_dir.resolve(), args.expect_unsupported, args.remote, args.omit_owner,
             args.alter_content, args.ordinary_remote)


if __name__ == "__main__":
    main()
