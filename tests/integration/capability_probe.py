#!/usr/bin/env python3
"""用正式进程验证能力画像、只校验不安装的探测、缓存失效和重启边界"""

import argparse
import errno
import os
from pathlib import Path
import subprocess
import tempfile
import time

from batch_update import check, fields
from health_readiness import clean_environment, daemon_command
from recovery_isolation import stop, wait_for


def run_case(build, tap=False):
    """普通场景只用 ring，TAP 场景使用独占临时接口，不改物理网卡或主机地址"""
    with tempfile.TemporaryDirectory(prefix="dppd-capability-") as temporary:
        directory = Path(temporary)
        (directory / "storage").mkdir()
        sock, state = directory / "ctl.sock", directory / "storage/state.bin"
        interfaces = (f"dc{os.getpid()}a", f"dc{os.getpid()}b") if tap else ()
        command = daemon_command(build, directory, 1, interfaces)
        environment = clean_environment()
        pool_capacity = 8191 if tap else 1024
        drop_id, filter_id, spare_id = 21000, 21010, 21999
        filter_tokens = ["tcp", "192.0.2.0/24", "any", "any", 54321,
                         "drop", "count", "mark:7"]
        logs = []

        def ctl(*tokens, error=None):
            """有效探测即使答复不支持也应成功返回，参数或服务状态错误则检查业务错误码"""
            result = subprocess.run([str(build / "dppctl"), "--socket", str(sock),
                                     *map(str, tokens)], env=environment,
                                    capture_output=True, text=True, timeout=5)
            check(result.returncode == (0 if error is None else 1), result.stdout + result.stderr)
            if error is not None:
                check(f"({-error})" in result.stderr, result.stderr)
            return result.stdout

        def profile(port=0):
            """画像字段来自启动快照和进程内记录，不能因读取而发起新的驱动校验"""
            return fields(ctl("capability-show", port))

        def probe(port=0, rule_id=drop_id, priority=10, refresh=False):
            """默认 DROP 在 ring 上不支持，在 TAP 上可以通过真实驱动校验"""
            tokens = ["probe-drop", rule_id, port, priority]
            if refresh:
                tokens.append("refresh")
            result = fields(ctl(*tokens))
            check(result["hardware"] == ("supported" if tap else "unsupported"), str(result))
            check(result["software"] == "yes" and result["age-ms"].isdigit(), str(result))
            return result

        def start(name):
            """启动独立正式进程，退出后保留日志以核对报文池、socket 和临时接口回收"""
            path = directory / name
            logs.append(path)
            with path.open("w") as output:
                process = subprocess.Popen(command, env=environment, stdout=output,
                                           stderr=subprocess.STDOUT)
            try:
                wait_for(process, sock.exists, "management socket")
                wait_for(process, lambda: fields(ctl("health"))["ready"] == "yes", "readiness")
            except Exception:
                stop(process)
                raise
            return process

        daemon = None
        try:
            daemon = start("run.log")
            initial = profile()
            raw_profile = ctl("capability-show", 0)
            check(initial["device"] == ("net_tap0" if tap else "net_ring0"), str(initial))
            check("DPDK " in raw_profile and initial["firmware"] == "unknown", raw_profile)
            check(int(initial["firmware-error"]) == -errno.ENOTSUP, str(initial))
            check(initial["queues"] == "1" and int(initial["rx-desc"]) > 0 and
                  int(initial["tx-desc"]) > 0, str(initial))
            check(initial["validations"] == "0" and initial["cache-entries"] == "0" and
                  initial["cache-capacity"] == "64" and initial["ttl-ms"] == "5000", str(initial))
            saved = state.read_bytes()
            for _ in range(3):
                check(ctl("capability-show", 0) == raw_profile, "profile query changed observations")
            ctl("capability-show", 9, error=errno.ENOENT)
            ctl("probe-drop", drop_id, 9, error=errno.ENOENT)

            first = probe()
            check(first["cached"] == "no", str(first))
            observed = profile()
            check(observed["validations"] == "1" and observed["misses"] == "1", str(observed))
            check(probe()["cached"] == "yes", "identical rule missed cache")
            hit = profile()
            check(hit["validations"] == "1" and hit["hits"] == "1", str(hit))
            for tokens in [(0, drop_id, 11), (0, drop_id + 1, 10), (1, drop_id, 10)]:
                check(probe(*tokens)["cached"] == "no", "rule or port key was ignored")

            # 逐项改变完整规则，掩码、协议、动作及 COUNT 都不能复用不同语义的结果
            for variant in range(6):
                tokens = list(filter_tokens)
                if variant == 1:
                    tokens[1] = "192.0.2.0/25"
                elif variant == 2:
                    tokens[0] = "udp"
                elif variant == 3:
                    tokens[4] = 54322
                elif variant == 4:
                    tokens[5] = "queue:0"
                elif variant == 5:
                    tokens.remove("count")
                result = fields(ctl("probe-filter", filter_id, 0, *tokens))
                check(result["cached"] == "no", "different filter borrowed cache: " + str(result))
                check(result["software"] == ("no" if variant == 4 else "yes"), str(result))

            before_refresh = profile()
            check(probe(refresh=True)["cached"] == "no", "refresh reused cache")
            after_refresh = profile()
            check(int(after_refresh["validations"]) == int(before_refresh["validations"]) + 1,
                  "refresh did not call driver")
            cleared = fields(ctl("probe-cache-clear", 0))
            check(cleared["cache-entries"] == "0" and
                  int(cleared["epoch"]) == int(after_refresh["epoch"]) + 1 and
                  cleared["validations"] == after_refresh["validations"], str(cleared))
            check(probe()["cached"] == "no", "explicit clear left cached result")

            # 真实单调时钟也做一次到期检查，五秒内的精确边界已由可控时钟单元测试覆盖
            time.sleep(5.05)
            stale = ctl("capability-show", 0)
            check(ctl("capability-show", 0) == stale, "read-only profile cleaned expired entries")
            check(probe()["cached"] == "no", "expired result reused")
            check(fields(ctl("list"))["total"] == "0" and
                  fields(ctl("health"))["current-generation"] == "0", "probe installed a rule")
            ctl("get", drop_id, error=errno.ENOENT)
            check(state.read_bytes() == saved, "probe wrote a snapshot")

            if not tap:
                # 负缓存只回答诊断，require 安装仍访问驱动，而不是直接套用旧的失败结果
                calls = int(profile()["validations"])
                code = abs(int(first["hardware-error"]))
                ctl("apply-drop", drop_id, 0, 0, 10, "require", error=code)
                check(int(profile()["validations"]) == calls + 1, "require skipped fresh validate")
                check(state.read_bytes() == saved, "failed require changed snapshot")
            check(probe(1, spare_id, 12)["cached"] == "no", "new shared-port probe was cached")
            other = profile(1)
            calls = int(profile()["validations"])
            ctl("apply-drop", drop_id, 0, 0, 10, "require" if tap else "prefer")
            check(int(profile()["validations"]) == calls + 1, "installation reused probe cache")
            installed = fields(ctl("rule-status", drop_id, 1))
            check(installed["backend"] == ("rte_flow" if tap else "software"), str(installed))
            if tap:
                changed = profile(1)
                check(changed["cache-entries"] == "0" and
                      int(changed["epoch"]) > int(other["epoch"]), "create kept shared-port cache")
                check(int(profile()["observed-actions"], 16) != 0, "success was not observed")
            else:
                check(probe(1, spare_id, 12)["cached"] == "yes", "software-only create invalidated PMD cache")

            # 已有 COUNT 规则的画像和探测同样不能改版本、保存文件或重置计数
            ctl("apply-filter", filter_id, 1, 0, *filter_tokens, "software")
            saved = state.read_bytes()
            count = ctl("count", filter_id, 2)
            check("hits=0 bytes=0" in count, count)
            for _ in range(3):
                profile()
                probe(1, spare_id, 12)
                check(ctl("count", filter_id, 2) == count, "diagnostic changed COUNT")
            check(state.read_bytes() == saved and
                  fields(ctl("health"))["current-generation"] == "2", "diagnostic changed desired state")
            generation = 1
            if tap:
                probe(0, spare_id, 12)
                epochs = [int(profile(port)["epoch"]) for port in (0, 1)]
                ctl("apply-drop", drop_id, 1, 1, 20, "require")
                generation = 3
                for port in (0, 1):
                    changed = profile(port)
                    check(changed["cache-entries"] == "0" and
                          int(changed["epoch"]) > epochs[port], "update kept shared cache")
                ctl("rule-status", drop_id, 1, error=errno.ESTALE)
            installed = fields(ctl("rule-status", drop_id, generation))
            saved = state.read_bytes()
            probe(0, spare_id, 12)
            stop(daemon)
            check(daemon.returncode == 0 and not sock.exists(), logs[-1].read_text())

            daemon = start("replayed.log")
            check(state.read_bytes() == saved, "replay changed snapshot")
            for port in (0, 1):
                restarted = profile(port)
                check(restarted["cache-entries"] == "0" and restarted["hits"] == "0" and
                      restarted["misses"] == "0", "replay inherited old diagnostic cache")
            check(profile()["device"] == initial["device"], "replay changed device identity")
            replayed = fields(ctl("rule-status", drop_id, generation))
            for key in ("rule", "generation", "port", "backend", "fallback"):
                check(replayed[key] == installed[key], "replay changed rule identity: " + str(replayed))
            check(probe(0, spare_id, 12)["cached"] == "no", "first replay probe was cached")
            check("hits=0 bytes=0" in ctl("count", filter_id, 2), "COUNT was not reset at replay")
            if tap:
                epochs = [int(profile(port)["epoch"]) for port in (0, 1)]
            ctl("delete", drop_id, generation)
            if tap:
                for port in (0, 1):
                    changed = profile(port)
                    check(changed["cache-entries"] == "0" and
                          int(changed["epoch"]) > epochs[port], "delete kept shared cache")
            ctl("delete", filter_id, 2)
            stop(daemon)
            check(daemon.returncode == 0 and not sock.exists(), logs[-1].read_text())
            for log in logs:
                check(f"available={pool_capacity} capacity={pool_capacity} in-use=0" in log.read_text(),
                      "process leaked mbufs: " + log.read_text())
            for interface in interfaces:
                check(not Path(f"/sys/class/net/{interface}").exists(), "temporary TAP remains")
        except Exception:
            for log in logs:
                print(log.read_text(), flush=True)
            raise
        finally:
            if daemon is not None:
                stop(daemon)
        print(f"PASS {'TAP' if tap else 'ring'} capability: identity, passive profile, complete keys, "
              "cache hit/refresh/clear/expiry, no installation or snapshot changes, fresh validate, "
              "COUNT unchanged, replay, cleanup" + (", shared-port invalidation" if tap else ""), flush=True)


def main():
    """默认验证普通用户的 ring 场景，--tap 切换到需 root 的真实 TAP 驱动场景"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--tap", action="store_true", help="verify real TAP flow validation as root")
    args = parser.parse_args()
    if args.tap:
        check(os.geteuid() == 0, "--tap requires root")
    run_case(args.build_dir.resolve(), args.tap)


if __name__ == "__main__":
    main()
