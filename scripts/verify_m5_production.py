#!/usr/bin/env python3
"""联合验收 M5 软件生产化，单套功能通过不会提前宣称整体或物理硬件验收完成"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import subprocess
import sys

import verify_m4
from verify_m2 import run_command, source_digest


PROFILE_SCOPE = "M5_current_environment_software_production_profile"
COMPLETE_SCOPE = "M5_complete_software_production_acceptance"
PROGRAMS = ("dppd", "dppctl", "dppd-flow-state")
FAULT = "tests/integration/test_m5_daemon_fault"
STALL = "tests/integration/test_m5_worker_stall"
RESTART = "tests/integration/test_m5_flow_restart"
M4_WORKER = "tests/integration/test_m4_flow_traffic"
BINARIES = PROGRAMS + (FAULT, STALL, RESTART, M4_WORKER)
MATRIX = {(version, buildtype, sanitize) for version in ("21.11.9", "24.11.8")
          for buildtype, sanitize in (("debugoptimized", "none"), ("debug", "address,undefined"))}
FEATURE_TESTS = (
    "M5 M4 configuration codec integrity and atomic persistence",
    "M5 M4 durable receipts crash replay and residual cleanup",
    "M5 16384 rule configuration replay and exact teardown",
    "M5 configuration CLI create check and software replay",
    "M5 managed protocol durable receipts and session boundaries",
    "M5 online definition transactions shared counting and durable recovery",
    "M5 persistent request idempotency crash receipts and stale key protection",
    "M5 NUMA worker placement and enabled CPU boundaries",
    "M5 managed telemetry snapshots counts and lifecycle",
    "M5 northbound authentication parsing and resource limits",
    "M5 reproducible package and managed deployment safety",
    "M5 production acceptance evidence boundaries",
)
M2_STEPS = ("compile", "unit", "software_traffic", "ipv4_traffic", "ipv4_routing", "acl_traffic",
            "conntrack_nat", "dnat_traffic", "ipv6_traffic", "nat66_traffic", "nd6_traffic", "tunnel_traffic")
STEPS = {
    "daemon": (
        "durable_create_and_exclusive_writer",
        "sigkill_replay_exact_generation_update_delete_and_stale_session",
        "write_failure_stops_workers_flush_recovers_durable_ack",
        "online_templates_shared_transactions_counting_references_crash_and_storage_recovery",
        "corruption_legacy_mode_and_path_alias_rejected",
    ),
    "requests": (
        "concurrent_identical_keys_one_rule_conflict_and_exact_revision",
        "lost_response_disconnected_client_query_without_resubmit",
        "sigkill_policy_receipt_shared_identity_replay_without_reexecuting_configuration",
        "receipt_rotation_retains_client_high_water_across_restart",
        "fsync_acceptance_failure_stops_workers_flush_persists_failure_and_new_key_recovers",
    ),
    "northbound": (
        "authenticated_roles_typed_rules_and_persistent_idempotency",
        "daemon_sigkill_replay_and_gateway_reconnect_without_duplicate_mutation",
        "admin_configuration_transaction_shared_rule_migration_exact_delete_and_metrics",
        "verified_tls_authentication_untrusted_certificate_and_remote_plaintext_rejection",
    ),
    "observability": (
        "formal_daemon_health_port_queue_rule_telemetry_and_idle_heartbeat",
        "json_lifecycle_periodic_statistics_and_eal_restricted_numa_layout",
        "hung_worker_bounded_failure_exit_and_exact_checkpoint_restart",
    ),
    "release": (
        "two_canonical_packages_identical_and_complete_manifest_verified",
        "two_formal_daemons_and_durable_declared_policy",
        "healthy_upgrade_and_explicit_rollback_keep_exact_policy",
        "failed_candidate_automatically_restores_old_binary_with_current_policy",
        "incompatible_reader_rejected_before_stopping_current_process",
        "controller_interruption_recovers_ready_candidate_without_policy_restore",
        "two_independent_instances_roll_with_other_ready_gate",
        "daemon_crash_replays_current_durable_policy",
    ),
}
SCOPES = {"daemon": "M5_formal_daemon_local_protocol", "requests": "M5_durable_request_formal_daemon",
          "northbound": "M5_formal_daemon_authenticated_northbound_v1",
          "observability": "M5_managed_observability_numa_heartbeat", "release": "M5_software_release_lifecycle"}
CHILD_PATHS = {"baseline": "baseline/report.json", "m4": "baseline/m4/report.json",
               "m3": "baseline/m4/m3/report.json", "m2": "baseline/m4/m3/software-regression/report.json",
               "m4_worker": "baseline/m4/worker/report.json", "restart": "baseline/restart/report.json",
               "daemon": "baseline/daemon/report.json", "requests": "baseline/requests/report.json",
               "northbound": "northbound/report.json", "observability": "observability/report.json",
               "release": "release/report.json"}
SANITIZER_ERROR = re.compile(r"(?:ERROR|SUMMARY):[^\n]*(?:AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer)|"
                             r"AddressSanitizer:DEADLYSIGNAL|runtime error:")
PYTHON_SKIP = re.compile(r"\b(?:skipped|skip)\s*[=:]\s*[1-9][0-9]*|^\s*Skipped:\s*[1-9][0-9]*", re.MULTILINE)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(value):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def hash_map(value, names=None):
    require(isinstance(value, dict) and value and all(isinstance(key, str) and key and sha256(digest)
                                                     for key, digest in value.items()), "摘要清单缺失或格式不正确")
    if names is not None:
        require(set(value) == set(names), "二进制摘要清单存在缺项或多项")


def no_skips(value):
    """JSON 标记和 Python 内层跳过都不能被外层成功标记掩盖"""
    if isinstance(value, dict):
        for key, child in value.items():
            if key in ("skip", "skips", "skipped", "tests_skipped"):
                require(child in (0, False, []), "报告含跳过项目")
            if key == "result":
                require(child not in ("SKIP", "EXPECTEDFAIL", "UNEXPECTEDPASS", "TIMEOUT", "FAIL"), "测试结果异常")
            no_skips(child)
    elif isinstance(value, list):
        for child in value:
            no_skips(child)


def matching(report, source, scope=None, hardware=True):
    require(isinstance(report, dict) and report.get("success") is True and "error" not in report,
            "报告缺失、失败或仍带错误")
    require(report.get("source_sha256") == source, "证据与本轮源码不同")
    if scope is not None:
        require(report.get("scope") == scope, "报告范围不正确")
    if hardware:
        require(report.get("hardware_acceptance") == "not_run", "软件证据不能冒充物理硬件验收")
    no_skips(report)


def command_steps(report, expected):
    steps = report.get("steps")
    require(isinstance(steps, list) and len(steps) == len(expected), "命令步骤缺失或重复")
    for step, name in zip(steps, expected):
        require(isinstance(step, dict) and step.get("name") == name and type(step.get("returncode")) is int and
                step["returncode"] == 0 and "error" not in step, "命令步骤顺序或退出状态异常")


def strict_options(options):
    require(isinstance(options, dict) and options.get("tests") is True and options.get("werror") is True and
            options.get("b_ndebug") == "false", "构建必须开启测试、警告报错和断言")


def profile_key(report):
    options = report["build_options"]
    return report["dpdk"], options.get("buildtype"), options.get("b_sanitize")


def packets(report, expected):
    require(isinstance(report, dict) and set(report) == {"received", "forwarded", "dropped"} and
            all(type(value) is int for value in report.values()) and report == expected, "真实收发数量缺失或不正确")


def validate_feature(report, kind, source, binaries, tap=False):
    matching(report, source, SCOPES[kind])
    require(report.get("steps") == list(STEPS[kind]), kind + " 验收步骤缺失、重复或顺序变化")
    names = PROGRAMS + ((FAULT,) if kind in ("daemon", "requests") else (STALL,) if kind == "observability" else ())
    hash_map(report.get("binary_sha256"), names)
    require(all(report["binary_sha256"][name] == binaries[name] for name in names), "功能验收使用另一批二进制")
    if kind in ("daemon", "requests", "release"):
        require(report.get("tap") is tap, "正式功能 TAP 类型不符")
    if kind == "requests":
        packets(report.get("packets"), {"received": 56, "forwarded": 24, "dropped": 32} if tap else
                {"received": 0, "forwarded": 0, "dropped": 0})
    elif kind in ("daemon", "release"):
        received = "packets" if kind == "daemon" else "received"
        if tap:
            packets({"received": report.get(received), "forwarded": report.get("forwarded"), "dropped": report.get("dropped")},
                    {"received": 96, "forwarded": 32, "dropped": 64} if kind == "daemon" else
                    {"received": 72, "forwarded": 8, "dropped": 64})
        else:
            require(not any(name in report for name in (received, "forwarded", "dropped")), "无 TAP 报告不应记录 TAP 结果")
    if kind == "release":
        # 发布交接和注入故障不能扩大为历史版本兼容性证明，缺少范围字段时拒绝验收
        require(report.get("release_pair") == "same_binaries_distinct_manifests" and
                report.get("incompatible_reader") == "injected_failure" and
                report.get("failed_start") == "injected_failure", "发布报告缺少真实版本与故障注入范围")
        require(report.get("lossless_claim") is False and
                report.get("binary_reproducibility") == "separate_two_build_check_required" and
                sha256(report.get("package_sha256")) and sha256(report.get("release_source_sha256")),
                "发布验收缺少可校验的包或夸大了无损及可复现结论")
    elif kind == "observability":
        require(report.get("source_after_sha256") == source and report.get("numa_performance_acceptance") == "not_run",
                "观测验收混用源码或夸大 NUMA 性能结论")
        elapsed = report.get("stall_exit_seconds")
        require(type(elapsed) in (int, float) and math.isfinite(elapsed) and 0 < elapsed < 10, "卡死 worker 未在有界时间退出")
        stopped = report.get("stop_stall_exit_seconds")
        require(type(stopped) in (int, float) and math.isfinite(stopped) and 0 < stopped < 5, "停止请求绕过了卡死 worker 的有界等待")
        layout = report.get("layout")
        require(isinstance(layout, list) and len(layout) == 1 and isinstance(layout[0], dict), "缺少受 EAL 核集合限制的布局")
        row = layout[0]
        require(row.get("schema") == 1 and row.get("component") == "dppd" and row.get("event") == "worker_layout" and
                row.get("queue") == 0 and row.get("lcore") == 1 and row.get("timeout_ms") == 5000 and
                type(row.get("socket")) is int and row["socket"] >= 0, "NUMA 布局或超时配置不正确")


def validate_units(report):
    """使用 Meson 注册清单核对全量结果，不允许只留下几条成功测试"""
    expected, units = report.get("expected_unit_names"), report.get("unit_results")
    require(isinstance(expected, list) and expected and all(isinstance(name, str) and name for name in expected) and
            len(set(expected)) == len(expected), "注册测试清单缺失或重复")
    require(isinstance(units, list) and len(units) == len(expected), "测试结果数量与注册清单不同")
    names = []
    for unit in units:
        require(isinstance(unit, dict) and isinstance(unit.get("name"), str) and unit.get("result") == "OK" and
                type(unit.get("returncode")) is int and unit["returncode"] == 0, "测试存在失败或跳过")
        names.append(unit["name"].rsplit(":", 1)[-1])
        for field in ("stdout", "stderr"):
            text = unit.get(field)
            require(isinstance(text, str) and not PYTHON_SKIP.search(text) and not SANITIZER_ERROR.search(text),
                    "测试内层存在跳过或 sanitizer 错误")
    require(len(set(names)) == len(names) and set(names) == set(expected), "测试结果有重复、缺项或冒名")
    require(set(FEATURE_TESTS + verify_m4.FEATURE_TESTS).issubset(names), "M4 或 M5 必需功能没有注册并执行")
    require(type(report.get("unit_passed")) is int and report["unit_passed"] == len(units), "通过数量不正确")
    require(sha256(report.get("unit_log_sha256")), "缺少本轮原始测试日志摘要")


def validate_profile(report, source):
    matching(report, source, PROFILE_SCOPE)
    require(type(report.get("schema_version")) is int and report["schema_version"] == 1 and
            report.get("complete_m5_software_validation") is False and report.get("complete_m5_validation") is False,
            "单套证据版本不符或提前宣称整体验收完成")
    hash_map(source)
    strict_options(report.get("build_options"))
    require(profile_key(report) in MATRIX, "DPDK 版本或严格构建类型不在四套验收矩阵")
    require(report.get("source_after_sha256") == source, "验收期间源码改变")
    binaries = report.get("binary_sha256")
    hash_map(binaries, BINARIES)
    require(report.get("binary_after_sha256") == binaries, "验收期间二进制改变")
    command_steps(report, ("compile", "baseline", "northbound", "observability", "release"))
    validate_units(report)
    require(type(report.get("sanitizer_errors")) is int and report["sanitizer_errors"] == 0, "存在 sanitizer 错误")
    hash_map(report.get("scanned_logs_sha256"))
    evidence = report.get("evidence")
    require(isinstance(evidence, dict) and set(evidence) == set(CHILD_PATHS), "嵌套原始报告缺失或重复")
    baseline = evidence["baseline"]
    matching(baseline, source, "M5_M4_configuration_persistence_and_process_restart")
    command_steps(baseline, ("m4_regression", "fresh_process_worker", "formal_daemon", "durable_requests"))
    require(all(baseline.get(name) is True for name in ("m4_configuration_recovery_validated",
            "m4_daemon_protocol_validated", "online_definitions_validated", "durable_requests_validated")) and
            baseline.get("complete_m5_validation") is False, "配置恢复或持久请求未闭环")
    packets(baseline.get("restart_packets"), {"received": 96, "forwarded": 48, "dropped": 48})
    m4 = evidence["m4"]
    matching(m4, source, verify_m4.SCOPE)
    command_steps(m4, ("m3_regression", "m4_worker"))
    require(m4.get("schema_version") == 1 and m4.get("build_options") == report["build_options"] and
            m4.get("complete_m4_validation") is False and m4.get("complete_m4_software_validation") is False and
            isinstance(m4.get("coverage"), list) and len(m4["coverage"]) == len(verify_m4.FEATURE_TESTS) and
            set(m4["coverage"]) == set(verify_m4.FEATURE_TESTS), "M4 基线构建或覆盖不完整")
    for result in (baseline, m4):
        require(result.get("dpdk") == report["dpdk"] and result.get("unit_passed") == report["unit_passed"],
                "基线测试数量或 DPDK 版本不同")
    m3 = evidence["m3"]
    matching(m3, source, "virtual_control_and_software_regression")
    command_steps(m3, ("inventory", "regression", "transfer_process", "shared_recovery", "device_events", "transfer_simulation"))
    require(m3.get("complete_m3_validation") is False and m3.get("tap_requested") is False, "M3 软件范围错误")
    validate_m2(evidence["m2"], source, False)
    worker = evidence["m4_worker"]
    matching(worker, source, "production_M4_batch_service_net_ring_worker_packets")
    require(worker.get("schema_version") == 1 and worker.get("binary_sha256") == binaries[M4_WORKER] and
            type(worker.get("returncode")) is int and worker["returncode"] == 0, "M4 worker 缺少本轮二进制或成功状态")
    packets(worker.get("packets"), {"received": 272, "forwarded": 200, "dropped": 72})
    restart = evidence["restart"]
    matching(restart, source, "M5_configuration_fresh_process_real_net_ring_worker")
    require(restart.get("binary_sha256") == binaries[RESTART], "恢复测试二进制不同")
    stages = restart.get("stages")
    require(isinstance(stages, list) and len(stages) == 2, "缺少重启前后全新进程")
    for stage, name in zip(stages, ("before", "after")):
        require(isinstance(stage, dict) and stage.get("name") == name and type(stage.get("returncode")) is int and
                stage["returncode"] == 0 and sha256(stage.get("state_sha256")), "恢复进程失败或缺少配置摘要")
    require(stages[0]["state_sha256"] == stages[1]["state_sha256"], "退出或重启改变了持久期望状态")
    packets(restart.get("packets"), {"received": 96, "forwarded": 48, "dropped": 48})
    for kind in STEPS:
        validate_feature(evidence[kind], kind, source, binaries)
    return profile_key(report)


def validate_m2(report, source, tap):
    matching(report, source, hardware=False)
    require(report.get("tap_requested") is tap and report.get("complete_m2_validation") is tap, "M2 TAP 验收范围不正确")
    command_steps(report, M2_STEPS if tap else M2_STEPS[:4])
    versions = [entry.get("version") for entry in report.get("dpdk", []) if entry.get("name") == "libdpdk"]
    require(len(versions) == 1 and versions[0] in ("21.11.9", "24.11.8"), "M2 未记录支持的 DPDK 版本")


def validate_benchmark(report, source, contract):
    """由五轮原始样本重算中位数，避免把手写汇总数字当成真实测量"""
    matching(report, source)
    strict_options(report.get("build_options"))
    require(report.get("schema_version") == 1 and sha256(report.get("binary_sha256")) and
            report["build_options"].get("buildtype") == "debugoptimized" and
            report["build_options"].get("b_sanitize") == "none" and
            [entry.get("version") for entry in report.get("dpdk", []) if entry.get("name") == "libdpdk"] == ["21.11.9"],
            "性能报告不是参考版本或缺少二进制身份")
    rules, runs = report.get("rules"), report.get("runs")
    require(type(rules) is int and rules in (4096, 16384) and isinstance(runs, list) and len(runs) == 5,
            "性能规模或五轮样本不完整")

    def positive(value):
        return type(value) in (int, float) and math.isfinite(value) and value > 0

    if report.get("scope") == "production_async_batch_service_with_memory_driver":
        expected = {"hardware_acceptance": "not_run", "path": "memory_driver_async_batch_service", "rules": rules,
                    "batch": 64, "queues": 8, "queue_depth": 256, "window_rules": 2048, "samples": rules // 64}
        for run in runs:
            require(isinstance(run, dict) and all(run.get(key) == value for key, value in expected.items()) and
                    all(positive(run.get(key)) for key in ("install_ns", "completion_p99_ns")), "队列原始样本规模或计时无效")
        for key in ("install_ns", "completion_p99_ns"):
            require(report.get("median_" + key) == statistics.median(run[key] for run in runs), "队列中位数与五轮样本不一致")
        gate = "queue_4096_batch64_max_completion_p99_ns"
        expected_gates = {gate: report["median_completion_p99_ns"] <= contract["performance_gates"][gate]} if rules == 4096 else {}
    else:
        require(report.get("scope") == "production_control_with_memory_driver_and_software_backend", "性能测量范围不正确")
        identities = (("memory_driver", rules, 1), ("memory_driver", rules, 64), ("software", 1024, 1))
        fields = ("install_ns", "rules_per_second", "request_p99_ns", "remove_ns")
        for run in runs:
            require(isinstance(run, dict) and run.get("schema_version") == 1 and run.get("hardware_acceptance") == "not_run" and
                    run.get("snapshot_replay_rules") == rules and positive(run.get("snapshot_replay_ns")) and
                    positive(run.get("inprocess_residual_cleanup_ns")), "控制面原始样本没有覆盖完整重放或恢复")
            cases = run.get("cases")
            require(isinstance(cases, list) and len(cases) == 3, "控制面原始样本缺少三种路径")
            for case, identity in zip(cases, identities):
                require(isinstance(case, dict) and tuple(case.get(key) for key in ("path", "rules", "batch")) == identity and
                        all(positive(case.get(key)) for key in fields), "控制面原始样本路径或计时不正确")
        medians = []
        for index, identity in enumerate(identities):
            case = dict(zip(("path", "rules", "batch"), identity))
            case.update({key: statistics.median(run["cases"][index][key] for run in runs) for key in fields})
            medians.append(case)
        require(report.get("median_cases") == medians and report.get("median_residual_cleanup_ns") ==
                statistics.median(run["inprocess_residual_cleanup_ns"] for run in runs) and
                report.get("median_snapshot_replay_ns") == statistics.median(run["snapshot_replay_ns"] for run in runs),
                "控制面中位数与五轮原始样本不一致")
        expected_gates = verify_m4.evaluate_gates(report, contract)
    require(report.get("performance_gates") == expected_gates and all(value is True for value in expected_gates.values()),
            "原始性能门禁缺失或没有通过")


def complete_software(source, contract, profiles, tap, feature_taps, reproducibility, benchmarks):
    """所有独立证据都对齐后才给出软件完成标记，八项旧性能门槛继续由原验收器计算"""
    hash_map(source)
    require(isinstance(profiles, list) and len(profiles) == 4, "需要四套完整生产化报告")
    keys = [validate_profile(report, source) for report in profiles]
    require(set(keys) == MATRIX and len(set(keys)) == len(keys), "四套构建缺项或重复")
    validate_m2(tap, source, True)
    require(isinstance(feature_taps, dict) and set(feature_taps) == {"daemon", "requests", "release"}, "TAP 功能类别不完整")
    for kind, reports in feature_taps.items():
        require(isinstance(reports, list) and len(reports) == 4, kind + " 需要四套正式程序 TAP 证据")
        covered = set()
        for report in reports:
            candidates = [profile for profile in profiles if isinstance(report, dict) and
                          isinstance(report.get("binary_sha256"), dict) and
                          all(report["binary_sha256"].get(name) == profile["binary_sha256"][name] for name in PROGRAMS)]
            require(len(candidates) == 1, "TAP 二进制无法唯一对应本轮构建")
            profile = candidates[0]
            validate_feature(report, kind, source, profile["binary_sha256"], True)
            if kind == "release":
                require(all(report[name] == profile["evidence"]["release"][name]
                            for name in ("package_sha256", "release_source_sha256")), "TAP 使用了另一份发布包")
            key = profile_key(profile)
            require(key not in covered, "同一套构建的 TAP 证据重复")
            covered.add(key)
        require(covered == MATRIX, "TAP 尚未覆盖全部构建")
    result = reproducibility
    require(isinstance(result, dict) and type(result.get("schema")) is int and result["schema"] == 1 and
            result.get("success") is True and "error" not in result and
            result.get("scope") == "two_independent_build_directories" and result.get("code_source_sha256") == source,
            "两目录可复现报告版本、范围或源码不正确")
    no_skips(result)
    require(sha256(result.get("source_sha256")) and all(profile["evidence"]["release"]["release_source_sha256"] ==
            result["source_sha256"] for profile in profiles), "可复现构建源码包与正式发布验收不同")
    builds = result.get("builds")
    require(isinstance(builds, list) and len(builds) == 2, "必须有两个独立构建结果")
    for build in builds:
        hash_map(build, PROGRAMS)
    require(builds[0] == builds[1], "两个新目录的二进制不是逐字节一致")
    metadata = result.get("build_metadata")
    require(isinstance(metadata, dict), "缺少可复现构建工具链信息")
    strict_options(metadata.get("options"))
    require(metadata["options"].get("buildtype") == "debugoptimized" and metadata["options"].get("b_sanitize") == "none" and
            [entry.get("version") for entry in metadata.get("dependencies", []) if entry.get("name") == "libdpdk"] == ["21.11.9"] and
            all(isinstance(metadata.get(name), str) and metadata[name] for name in ("compiler", "linker", "architecture", "meson")),
            "可复现构建不是严格的参考版本或缺少工具链身份")
    require(isinstance(benchmarks, list) and len(benchmarks) == 4, "需要原四份冻结性能报告")
    for benchmark in benchmarks:
        validate_benchmark(benchmark, source, contract)
    # 保留原有八项计算和全部规模约束，生产化不能通过降低旧门槛来完成
    return verify_m4.complete_software(source, contract, [profile["evidence"]["m4"] for profile in profiles], tap, benchmarks)


def read_json(path):
    """重复 JSON 键会覆盖旧值，因此在加载时直接拒绝歧义证据"""
    def unique(items):
        result = {}
        for key, value in items:
            require(key not in result, "JSON 对象出现重复键：" + key)
            result[key] = value
        return result

    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique,
                      parse_constant=lambda value: require(False, "JSON 含非有限数值：" + value))


def binary_hashes(build):
    return {name: hashlib.sha256((build / name).read_bytes()).hexdigest() for name in BINARIES}


def run_profile(root, build, destination, report):
    metadata = read_json(build / "meson-info/meson-info.json")
    require(Path(metadata["directories"]["source"]).resolve() == root, "构建目录对应另一份源码")
    commands = (("compile", ["meson", "compile", "-C", str(build)], 180),
                ("baseline", [sys.executable, "scripts/verify_m5.py", "--build-dir", str(build),
                              "--report-dir", str(destination / "baseline")], None))
    commands += tuple((kind, [sys.executable, f"tests/integration/m5_{kind}.py", "--build-dir", str(build),
                             "--report-dir", str(destination / kind)], 300)
                      for kind in ("northbound", "observability", "release"))
    for name, command, timeout in commands:
        (destination / name / "report.json").unlink(missing_ok=True)
        with (destination / (name + ".log")).open("w") as output:
            rc = run_command(command, root, output, timeout)
        report["steps"].append({"name": name, "command": command, "returncode": rc})
        require(rc == 0, name + " 未通过")
        if name == "compile":
            report["build_options"] = {item["name"]: item["value"] for item in read_json(build / "meson-info/intro-buildoptions.json")}
            dependencies = read_json(build / "meson-info/intro-dependencies.json")
            versions = [item["version"] for item in dependencies if item["name"] == "libdpdk"]
            require(len(versions) == 1, "无法唯一识别 DPDK 依赖")
            report["dpdk"] = versions[0]
            report["binary_sha256"] = binary_hashes(build)
            report["expected_unit_names"] = [item["name"] for item in read_json(build / "meson-info/intro-tests.json")]
    report["evidence"] = {name: read_json(destination / path) for name, path in CHILD_PATHS.items()}
    log_bytes = (build / "meson-logs/testlog.json").read_bytes()
    report["unit_log_sha256"] = hashlib.sha256(log_bytes).hexdigest()
    tests = [json.loads(line) for line in log_bytes.decode("utf-8").splitlines() if line.strip()]
    # 原始 Meson 日志可能包含主机环境变量，只嵌入验收所需字段，环境内容不进入联合报告
    report["unit_results"] = [{name: test.get(name, "") for name in ("name", "result", "returncode", "stdout", "stderr")}
                              for test in tests]
    report["unit_passed"] = len(tests)
    scanned = {}
    for path in sorted(destination.rglob("*.log")):
        content = path.read_bytes()
        require(not SANITIZER_ERROR.search(content.decode("utf-8", errors="replace")), "日志存在 sanitizer 错误：" + str(path))
        scanned[path.relative_to(destination).as_posix()] = hashlib.sha256(content).hexdigest()
    report["sanitizer_errors"], report["scanned_logs_sha256"] = 0, scanned
    report["binary_after_sha256"] = binary_hashes(build)
    report["source_after_sha256"] = source_digest(root)
    validate_profile(dict(report, success=True), report["source_sha256"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--report-dir", type=Path, required=True)
    parser.add_argument("--complete-software", action="store_true")
    parser.add_argument("--profile-report", type=Path, action="append", default=[])
    parser.add_argument("--tap-report", type=Path)
    for kind in ("daemon", "requests", "release"):
        parser.add_argument("--" + kind + "-tap-report", type=Path, action="append", default=[])
    parser.add_argument("--reproducibility-report", type=Path)
    parser.add_argument("--benchmark-report", type=Path, action="append", default=[])
    args = parser.parse_args()
    root, destination = Path(__file__).resolve().parents[1], args.report_dir.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    output = destination / "report.json"
    report = {"schema_version": 1, "scope": COMPLETE_SCOPE if args.complete_software else PROFILE_SCOPE,
              "success": False, "complete_m5_validation": False, "complete_m5_software_validation": False,
              "hardware_acceptance": "not_run", "source_sha256": source_digest(root),
              "started_utc": datetime.now(timezone.utc).isoformat(), "steps": [], "inputs": []}
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    used = set()

    def load(path):
        require(path is not None, "缺少联合证据参数")
        path = path.resolve()
        require(path not in used and path != output, "联合报告路径重复或引用自身")
        used.add(path)
        result = read_json(path)
        report["inputs"].append({"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
        return result

    try:
        if args.complete_software:
            require(args.build_dir is None, "聚合模式不重复执行单套构建")
            contract_bytes = (root / "docs/m4_acceptance_contract.json").read_bytes().replace(b"\r\n", b"\n")
            require(hashlib.sha256(contract_bytes).hexdigest() == verify_m4.CONTRACT_SHA256, "冻结性能契约改变")
            profiles = [load(path) for path in args.profile_report]
            tap = load(args.tap_report)
            feature_taps = {kind: [load(path) for path in getattr(args, kind + "_tap_report")]
                            for kind in ("daemon", "requests", "release")}
            reproducibility = load(args.reproducibility_report)
            benchmarks = [load(path) for path in args.benchmark_report]
            report["performance_gates"] = complete_software(report["source_sha256"], json.loads(contract_bytes), profiles,
                                                            tap, feature_taps, reproducibility, benchmarks)
            report["validated_profiles"] = [list(profile_key(profile)) for profile in profiles]
            report["complete_m5_software_validation"] = True
        else:
            require(args.build_dir is not None, "单套验收需要 --build-dir")
            require(not args.profile_report and args.tap_report is None and not args.benchmark_report and
                    args.reproducibility_report is None and not any(getattr(args, kind + "_tap_report")
                    for kind in ("daemon", "requests", "release")), "联合证据参数必须配合 --complete-software")
            report["build"] = str(args.build_dir.resolve())
            run_profile(root, args.build_dir.resolve(), destination, report)
        require(source_digest(root) == report["source_sha256"], "验收期间源码改变")
        report["success"] = True
    except (OSError, ValueError, TypeError, KeyError, IndexError, AttributeError, subprocess.SubprocessError) as error:
        report["success"] = report["complete_m5_software_validation"] = False
        report["error"] = str(error)
        print(str(error), file=sys.stderr)
    finally:
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(str(output))
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
