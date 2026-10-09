#!/usr/bin/env python3
"""正式进程验证失败分类、耗时历史、只读 telemetry、分页、并发发布、保存故障与重启"""

import argparse
import errno
import os
from pathlib import Path
import subprocess
import tempfile
import threading

from batch_update import check, fields
from health_readiness import clean_environment, daemon_command
from recovery_isolation import stop, wait_for
from telemetry_client import (Telemetry, LATENCY_SCOPES, check_latency, check_latency_commits,
                              read_latency, read_history, check_history, history_events)


def run_case(build, tap=False):
    """独立 ring 或 TAP 实例，不改物理网卡或主机地址，结束时回收临时接口"""
    with tempfile.TemporaryDirectory(prefix="dppd-rule-telemetry-") as temporary:
        directory = Path(temporary)
        storage = directory / "storage"
        storage.mkdir()
        state, sock, log = storage / "state.bin", directory / "ctl.sock", directory / "run.log"
        interfaces = (f"dt{os.getpid()}a", f"dt{os.getpid()}b") if tap else ()
        command = daemon_command(build, directory, 1, interfaces)
        command.remove("--no-telemetry")
        command.extend(["--rule-capacity", "80"])
        environment = clean_environment()
        telemetry = None

        def ctl(*tokens, error=None):
            """用真实管理客户端，不把失败回应的剩余内容解释成部分成功"""
            result = subprocess.run([str(build / "dppctl"), "--socket", str(sock),
                                     *map(str, tokens)], env=environment,
                                    capture_output=True, text=True, timeout=5)
            check(result.returncode == (0 if error is None else 1), result.stdout + result.stderr)
            if error is not None:
                check(f"({-error})" in result.stderr, result.stderr)
            return result.stdout

        def metrics():
            """CLI 提供当前完成态，telemetry 应在下一轮管理发布后达到同一个操作计数"""
            values = fields(ctl("rule-metrics"))
            telemetry.wait(lambda: telemetry.query("/dppd/rule_failures")["operations"] ==
                           int(values["operations"]))
            report = telemetry.query("/dppd/rule_failures")
            for name in ("operations", "succeeded", "failed", "unchanged", "applied",
                         "fallback_rules", "compensation_failures", "failed_after_apply"):
                check(report[name] == int(values[name]), str(report))
            check(report["operations"] == report["succeeded"] + report["failed"], str(report))
            return report

        def start():
            """每次重启覆盖本测试日志，EAL 日志对应当前进程的运行目录"""
            with log.open("w") as output:
                return subprocess.Popen(command, env=environment, stdout=output,
                                        stderr=subprocess.STDOUT)

        daemon = start()
        try:
            wait_for(daemon, sock.exists, "management socket")
            wait_for(daemon, lambda: fields(ctl("health"))["ready"] == "yes", "worker readiness")
            telemetry = Telemetry(daemon, log)
            report = metrics()
            check(report["operations"] == 1 and report["failed"] == 0, str(report))
            check_latency_commits(read_latency(ctl, telemetry), 0, 0, 0)
            check(read_history(ctl, telemetry)["revision"] == 0, "empty restore produced a failure")
            ctl("apply-filter", 100, 0, 0, "tcp", "any", "any", "any", 10000,
                "drop", "count", "software")
            if not tap:
                code = abs(int(fields(ctl("probe-drop", 101, 0))["hardware-error"]))
                check(code in (errno.ENOTSUP, errno.ENOSYS), "ring unexpectedly supports flow")
                ctl("apply-drop", 101, 0, 0, 1, "require", error=code)
                report = metrics()
                check(report["stage"] == "validate" and report["kind"] == "unsupported", str(report))
                check(report["rule_id"] == 101 and report["cause_error"] == -code, str(report))
            ctl("apply-drop", 101, 0, 0, 1, "require" if tap else "prefer")
            report = metrics()
            check(report["fallback_rules"] == (0 if tap else 1), str(report))
            actual = telemetry.query("/dppd/rule", 101)
            check(actual["backend"] == ("rte_flow" if tap else "software"), str(actual))
            ctl("apply-drop", 101, 0, 999, 2, "software", error=errno.ESTALE)
            report = metrics()
            check(report["stage"] == "preflight" and report["kind"] == "conflict", str(report))
            check(report["generation"] == 999 and report["cause_error"] == -errno.ESTALE, str(report))
            ctl("apply-filter", 102, 0, 0, "tcp", "any", "any", "any", "any", "queue:0",
                "software", error=errno.ENOTSUP)
            check(metrics()["stage"] == "plan", "unsupported software action stage changed")

            # 相同规则重放只增加成功请求和 unchanged，不增加安装数量或改写最后失败
            previous = metrics()
            ctl("apply-drop", 101, 0, 0, 1, "require" if tap else "prefer")
            report = metrics()
            check(report["unchanged"] == previous["unchanged"] + 1 and
                  report["applied"] == previous["applied"] and
                  report["last_sequence"] == previous["last_sequence"], str(report))
            saved, profile = state.read_bytes(), ctl("capability-show", 0)
            counted = telemetry.query("/dppd/rule", 100)
            history = read_latency(ctl, telemetry)
            check_latency_commits(history, 1 if tap else 2, 0, 1 if tap else 0)
            failures = read_history(ctl, telemetry)
            check(failures["revision"] == (2 if tap else 3), str(failures))
            for _ in range(5):
                check(telemetry.query("/dppd/rule", 100) == counted, "read changed installation status")
                telemetry.query("/dppd/rules")
                telemetry.query("/dppd/stats")
                check(telemetry.query("/dppd/rule_failures") == report, "read changed failure history")
                check(read_latency(ctl, telemetry) == history, "read changed latency history")
                check(read_history(ctl, telemetry) == failures, "read changed failure history")
            for scope in LATENCY_SCOPES:
                selected = telemetry.query("/dppd/rule_latency", scope)
                check_latency(selected)
                check(selected["scope"] == scope and
                      all(selected[name] == value for name, value in history.items()
                          if name.startswith(scope + "_") and
                          not (scope == "software" and name.startswith("software_batch_"))), str(selected))
                check(all(f"{other}_samples" not in selected for other in LATENCY_SCOPES if other != scope),
                      "group filter leaked another scope")
            check(state.read_bytes() == saved and ctl("capability-show", 0) == profile,
                  "telemetry touched storage or driver validation")
            check("hits=0 bytes=0" in ctl("count", 100, 1), "telemetry touched COUNT")
            for request in [("/dppd/rules", -1), ("/dppd/rules", "0,"),
                            ("/dppd/rules", 0, 18446744073709551615),
                            ("/dppd/rule", 0), ("/dppd/rule", 999999),
                            ("/dppd/rule_failures", 1), ("/dppd/rule_latency", "unknown"),
                            ("/dppd/rule_latency", "software,rte_flow"),
                            ("/dppd/rule_latency", "SOFTWARE"), ("/dppd/rule_latency", "software,"),
                            ("/dppd/rule_history", -1), ("/dppd/rule_history", " 1"),
                            ("/dppd/rule_history", "1,"), ("/dppd/rule_history", "1,2,3"),
                            ("/dppd/rule_history", "18446744073709551616"),
                            ("/dppd/rule_history", 0, 18446744073709551615)]:
                check(telemetry.query(*request) is None, "invalid request did not return null")
            check(metrics() == report, "bad telemetry parameters changed control metrics")
            check(read_latency(ctl, telemetry) == history, "bad parameters changed installation history")
            check(read_history(ctl, telemetry) == failures, "bad parameters created a failure event")

            # 规则多于一页并包含最大 uint64 ID，验证完整规则镜像和稳定版本分页
            for rule_id in [*range(1000, 1069), 18446744073709551615]:
                ctl("apply-drop", rule_id, 1, 0, 5, "software")
            metrics()
            check_latency_commits(read_latency(ctl, telemetry), 71 if tap else 72, 0, 1 if tap else 0)
            first = telemetry.query("/dppd/rules")
            check(first["returned"] == 64 and first["has_more"] and first["total"] == 72, str(first))
            second = telemetry.query("/dppd/rules", first["next_after"], first["repository_generation"])
            ids = first["rule_ids"] + second["rule_ids"]
            check(len(ids) == len(set(ids)) == 72 and ids[-1] == 18446744073709551615, str(ids))
            check(second["returned"] == 8 and not second["has_more"], str(second))
            check(telemetry.query("/dppd/rule", ids[-1])["rule_id"] == ids[-1], "uint64 ID lost precision")

            # 独立 telemetry 连接与正式软件整批更新并发，检查每次 JSON 的完成态关系
            failed, sample_count, running = [], [0], threading.Event()
            running.set()
            reader = Telemetry(daemon, log)

            def read_concurrently():
                """跨次查询允许不同发布号，单次回应内的规则版本和汇总必须完整一致"""
                try:
                    while running.is_set():
                        value = reader.query("/dppd/rule_failures")
                        check(value["operations"] == value["succeeded"] + value["failed"], str(value))
                        row = reader.query("/dppd/rule", 1000)
                        check(row["generation"] <= row["repository_generation"], str(row))
                        page = reader.query("/dppd/rules")
                        check(page["total"] == 72 and page["returned"] == 64, str(page))
                        latency = reader.query("/dppd/rule_latency")
                        check_latency(latency)
                        batch_commits = latency["software_batch_samples"] + latency["software_batch_unavailable"]
                        check(latency["software_batch_rules"] == batch_commits * 2, str(latency))
                        check_history(reader.query("/dppd/rule_history"))
                        sample_count[0] += 1
                except Exception as error:
                    failed.append(error)

            thread = threading.Thread(target=read_concurrently)
            thread.start()
            try:
                for priority in range(20, 40):
                    pairs = [token for rule_id in (1000, 1001)
                             for token in (rule_id, fields(ctl("get", rule_id))["generation"])]
                    ctl("update-drop-batch", 1, priority, "software", *pairs)
                metrics()
                saved_before_failures = state.read_bytes()
                # 一百三十次真正的预检失败覆盖两轮窗口，保留最大 uint64 规则身份
                for _ in range(130):
                    ctl("apply-drop", 18446744073709551615, 1, 9999, 99, "software", error=errno.ESTALE)
                metrics()
                check(state.read_bytes() == saved_before_failures, "preflight failure saved a snapshot")
            finally:
                running.clear()
                thread.join(timeout=5)
                reader.close()
            check(not thread.is_alive() and not failed and sample_count[0] > 0, str(failed))
            history = read_latency(ctl, telemetry)
            check_latency_commits(history, 71 if tap else 72, 20, 1 if tap else 0, batch_rules=40)
            retained = read_history(ctl, telemetry)
            check(retained["total"] == 64 and retained["gap"] and
                  retained["revision"] == failures["revision"] + 130, str(retained))
            ctl("rule-history", 0, failures["revision"], error=errno.ESTALE)
            check(telemetry.query("/dppd/rule_history", 0, failures["revision"]) is None,
                  "stale failure revision was accepted")
            events, page = [], retained
            while True:
                events.extend(history_events(page))
                if not page["has_more"]:
                    break
                page = read_history(ctl, telemetry, page["next_after"], retained["revision"])
                check(not page["gap"], "a retained cursor unexpectedly lost failures")
            check(len(events) == 64 and all(event["rule_id"] == 18446744073709551615 and
                  event["generation"] == 9999 and event["cause_error"] == -errno.ESTALE and
                  event["response_error"] == -errno.ESTALE and event["compensation_error"] == 0 and
                  not event["applied"] for event in events), str(events))
            check(read_history(ctl, telemetry, retained["oldest_event_id"] - 1,
                               retained["revision"])["gap"] == 0, "gap boundary is off by one")
            check(read_history(ctl, telemetry, 18446744073709551615)["returned"] == 0,
                  "maximum cursor did not return an empty page")
            check(telemetry.query("/dppd/rules", first["next_after"], first["repository_generation"]) is None,
                  "stale pagination accepted a different repository")

            # 保存目录替换为普通文件，规则已生效但原始保存错误和最终错误都要保留
            saved = state.read_bytes()
            backup = directory / "saved-storage"
            storage.rename(backup)
            storage.write_text("storage unavailable\n")
            ctl("apply-drop", 1100, 1, 0, 50, "software", error=errno.EUCLEAN)
            report = metrics()
            check(report["stage"] == "persist" and report["kind"] == "persistence", str(report))
            check(report["cause_error"] == -errno.ENOTDIR and report["response_error"] == -errno.EUCLEAN
                  and report["last_applied"] == 1 and report["failed_after_apply"] == 1, str(report))
            page = telemetry.query("/dppd/rules")
            check(page["dirty"] == 1 and page["total"] == 73, str(page))
            history = read_latency(ctl, telemetry)
            check_latency_commits(history, 72 if tap else 73, 20, 1 if tap else 0, batch_rules=40)
            failed_after_apply = read_history(ctl, telemetry)
            event = history_events(read_history(ctl, telemetry, failed_after_apply["newest_event_id"] - 1,
                                               failed_after_apply["revision"]))[0]
            check(event["rule_id"] == 1100 and event["stage"] == "persist" and event["applied"] and
                  event["cause_error"] == -errno.ENOTDIR and event["response_error"] == -errno.EUCLEAN,
                  str(event))
            for _ in range(3):
                telemetry.query("/dppd/rule", 1100)
                check(telemetry.query("/dppd/rule_failures") == report, "dirty read changed failure")
            check((backup / "state.bin").read_bytes() == saved and
                  storage.read_text() == "storage unavailable\n", "read repaired persistence")
            ctl("apply-drop", 1101, 1, 0, 50, "software", error=errno.EUCLEAN)
            check(metrics()["last_applied"] == 0, "blocked request reported new publication")
            ctl("persistence-flush", error=errno.ENOTDIR)
            report = metrics()
            check(report["operation"] == "flush" and report["response_error"] == -errno.ENOTDIR, str(report))
            flushed_failure = read_history(ctl, telemetry)
            storage.unlink()
            backup.rename(storage)
            ctl("persistence-flush")
            check(metrics()["last_sequence"] == report["last_sequence"], "success removed last failure")
            check(telemetry.query("/dppd/rules")["dirty"] == 0, "flush not published")
            check(read_latency(ctl, telemetry) == history, "blocked write or flush changed installation history")
            check(read_history(ctl, telemetry) == flushed_failure, "successful flush changed failure history")
            # 删除已经成功安装的规则，历史仍完整保留，下一次启动只统计仍需重放的规则
            ctl("delete", 1100, fields(ctl("get", 1100))["generation"])
            metrics()
            check(read_latency(ctl, telemetry) == history, "delete removed a historical commit")
            check(read_history(ctl, telemetry) == flushed_failure, "successful delete changed failure history")
            check(telemetry.query("/dppd/rules")["total"] == 72, "delete did not update desired rules")
            saved = state.read_bytes()
            telemetry.close()
        except Exception:
            print(log.read_text(), flush=True)
            raise
        finally:
            if telemetry is not None:
                telemetry.close()
            stop(daemon)
        check(daemon.returncode == 0 and not sock.exists(), log.read_text())
        check(f"available={8191 if tap else 1024} capacity={8191 if tap else 1024} in-use=0" in log.read_text(),
              "daemon leaked mbufs")

        # 规则与快照跨重启保留，累计指标和最后失败从新进程的一笔 restore 重新开始
        daemon = start()
        try:
            wait_for(daemon, sock.exists, "replayed management socket")
            wait_for(daemon, lambda: fields(ctl("health"))["ready"] == "yes", "replayed readiness")
            telemetry = Telemetry(daemon, log)
            report = metrics()
            check(report["operations"] == 1 and report["failed"] == 0 and report["last_sequence"] == 0,
                  "metrics reused a previous process epoch: " + str(report))
            check(telemetry.query("/dppd/rules")["total"] == 72 and state.read_bytes() == saved,
                  "replay changed persisted rules")
            check_latency_commits(read_latency(ctl, telemetry), 71 if tap else 72, 0, 1 if tap else 0)
            check(read_history(ctl, telemetry)["revision"] == 0, "restart reused old failure history")
        finally:
            telemetry.close()
            stop(daemon)
        check(daemon.returncode == 0 and not sock.exists(), log.read_text())
        for interface in interfaces:
            check(not Path(f"/sys/class/net/{interface}").exists(), "temporary TAP interface remains")
        print(f"PASS {'TAP' if tap else 'ring'} rule telemetry: classified failures, read-only snapshots, "
              "64-ID pagination, uint64 IDs, historical latency, concurrent updates, persistence failure, "
              "130 failures, retained window, stable history pagination, delete history, fresh replay epoch, cleanup",
              flush=True)


def main():
    """默认无需 root，--tap 验证真实 rte_flow 驱动并检查临时接口清理"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--tap", action="store_true")
    args = parser.parse_args()
    if args.tap:
        check(os.geteuid() == 0, "--tap requires root")
    run_case(args.build_dir.resolve(), args.tap)


if __name__ == "__main__":
    main()
