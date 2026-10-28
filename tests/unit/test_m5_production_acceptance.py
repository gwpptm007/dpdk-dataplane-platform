#!/usr/bin/env python3
"""验收器必须拒绝缺项、旧源码、跳过、重复和伪成功，完整样例只在测试内构造"""

import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import verify_m4
import verify_m5_production as production


def digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


def steps(names):
    return [{"name": name, "returncode": 0} for name in names]


class Acceptance(unittest.TestCase):
    def setUp(self):
        self.source = {"meson.build": digest("build"), "lib/control/flow_service.c": digest("current source")}
        self.contract = json.loads((ROOT / "docs/m4_acceptance_contract.json").read_text(encoding="utf-8"))
        self.profiles = [self.profile(*key) for key in sorted(production.MATRIX)]
        self.tap = self.base()
        self.tap.update(tap_requested=True, complete_m2_validation=True, steps=steps(production.M2_STEPS),
                        dpdk=[{"name": "libdpdk", "version": "21.11.9"}])
        self.taps = {kind: [self.feature(kind, profile["binary_sha256"], True) for profile in self.profiles]
                     for kind in ("daemon", "requests", "release")}
        self.repro = {"schema": 1, "success": True, "scope": "two_independent_build_directories",
                      "code_source_sha256": self.source, "source_sha256": digest("source.tar"),
                      "builds": [{name: digest("reproduced:" + name) for name in production.PROGRAMS} for _ in range(2)],
                      "build_metadata": {"options": self.options(), "dependencies": [{"name": "libdpdk", "version": "21.11.9"}],
                                         "compiler": "gcc 11", "linker": "GNU ld", "architecture": "x86_64", "meson": "0.61"}}
        self.benchmarks = []
        for rules in (4096, 16384):
            common = dict(self.base(), schema_version=1, rules=rules, repetitions=5,
                          contract_sha256=verify_m4.CONTRACT_SHA256, cpu_affinity=[0, 1, 2, 3],
                          build_options=self.options(),
                          binary_sha256=digest("benchmark"), dpdk=[{"name": "libdpdk", "version": "21.11.9"}])
            cases = [{"path": "memory_driver", "rules": rules, "batch": batch, "install_ns": 1000000,
                      "rules_per_second": 2000000, "request_p99_ns": 4000, "remove_ns": 1000000} for batch in (1, 64)]
            cases.append({"path": "software", "rules": 1024, "batch": 1, "install_ns": 1000000,
                          "rules_per_second": 5000, "request_p99_ns": 500000, "remove_ns": 1000000})
            run = {"schema_version": 1, "hardware_acceptance": "not_run", "cases": cases,
                   "inprocess_residual_cleanup_ns": 1000, "snapshot_replay_rules": rules, "snapshot_replay_ns": 1000000}
            control = dict(common, scope="production_control_with_memory_driver_and_software_backend",
                           runs=[copy.deepcopy(run) for _ in range(5)], median_cases=copy.deepcopy(cases),
                           median_residual_cleanup_ns=1000, median_snapshot_replay_ns=1000000)
            control["performance_gates"] = verify_m4.evaluate_gates(control, self.contract)
            run = {"hardware_acceptance": "not_run", "path": "memory_driver_async_batch_service", "rules": rules,
                   "batch": 64, "queues": 8, "queue_depth": 256, "window_rules": 2048, "samples": rules // 64,
                   "install_ns": 1000000, "completion_p99_ns": 5000000}
            queue = dict(common, scope="production_async_batch_service_with_memory_driver",
                         runs=[copy.deepcopy(run) for _ in range(5)], median_completion_p99_ns=5000000,
                         median_install_ns=1000000,
                         performance_gates={"queue_4096_batch64_max_completion_p99_ns": True} if rules == 4096 else {})
            self.benchmarks += [control, queue]

    def base(self, scope=None):
        result = {"success": True, "source_sha256": self.source, "hardware_acceptance": "not_run"}
        if scope is not None:
            result["scope"] = scope
        return result

    @staticmethod
    def options(buildtype="debugoptimized", sanitize="none"):
        return {"tests": True, "werror": True, "b_ndebug": "false", "buildtype": buildtype, "b_sanitize": sanitize}

    def feature(self, kind, binaries, tap=False):
        names = production.PROGRAMS + ((production.FAULT,) if kind in ("daemon", "requests") else
                                       (production.STALL,) if kind == "observability" else ())
        result = dict(self.base(production.SCOPES[kind]), steps=list(production.STEPS[kind]),
                      binary_sha256={name: binaries[name] for name in names})
        if kind in ("daemon", "requests", "release"):
            result["tap"] = tap
        if kind == "requests":
            result["packets"] = {"received": 56, "forwarded": 24, "dropped": 32} if tap else {
                "received": 0, "forwarded": 0, "dropped": 0}
        elif kind == "daemon" and tap:
            result.update(packets=96, forwarded=32, dropped=64)
        elif kind == "release":
            result.update(lossless_claim=False, binary_reproducibility="separate_two_build_check_required",
                          release_pair="same_binaries_distinct_manifests", incompatible_reader="injected_failure",
                          failed_start="injected_failure",
                          package_sha256=digest("package"), release_source_sha256=digest("source.tar"))
            if tap:
                result.update(received=72, forwarded=8, dropped=64)
        elif kind == "observability":
            result.update(source_after_sha256=self.source, numa_performance_acceptance="not_run", stall_exit_seconds=0.5,
                          stop_stall_exit_seconds=0.5,
                          layout=[{"schema": 1, "component": "dppd", "event": "worker_layout", "queue": 0,
                                   "lcore": 1, "socket": 0, "timeout_ms": 5000}])
        return result

    def profile(self, version, buildtype, sanitize):
        options = self.options(buildtype, sanitize)
        binaries = {name: digest(version + buildtype + sanitize + name) for name in production.BINARIES}
        names = list(production.FEATURE_TESTS + verify_m4.FEATURE_TESTS)
        result = dict(self.base(production.PROFILE_SCOPE), schema_version=1, complete_m5_validation=False,
                      complete_m5_software_validation=False, dpdk=version, build_options=options,
                      source_after_sha256=self.source, binary_sha256=binaries, binary_after_sha256=copy.deepcopy(binaries),
                      steps=steps(("compile", "baseline", "northbound", "observability", "release")),
                      expected_unit_names=names, unit_passed=len(names), unit_log_sha256=digest("testlog"),
                      unit_results=[{"name": name, "result": "OK", "returncode": 0, "stdout": "", "stderr": ""} for name in names],
                      sanitizer_errors=0, scanned_logs_sha256={"baseline.log": digest("log")})
        baseline = dict(self.base("M5_M4_configuration_persistence_and_process_restart"),
                        steps=steps(("m4_regression", "fresh_process_worker", "formal_daemon", "durable_requests")),
                        m4_configuration_recovery_validated=True, m4_daemon_protocol_validated=True,
                        online_definitions_validated=True, durable_requests_validated=True, complete_m5_validation=False,
                        unit_passed=len(names), dpdk=version, restart_packets={"received": 96, "forwarded": 48, "dropped": 48})
        m4 = dict(self.base(verify_m4.SCOPE), schema_version=1, complete_m4_validation=False, complete_m4_software_validation=False,
                  steps=steps(("m3_regression", "m4_worker")), build_options=copy.deepcopy(options), dpdk=version,
                  unit_passed=len(names), coverage=list(verify_m4.FEATURE_TESTS))
        m3 = dict(self.base("virtual_control_and_software_regression"), complete_m3_validation=False, tap_requested=False,
                  steps=steps(("inventory", "regression", "transfer_process", "shared_recovery", "device_events", "transfer_simulation")))
        m2 = dict(self.base(), tap_requested=False, complete_m2_validation=False, steps=steps(production.M2_STEPS[:4]),
                  dpdk=[{"name": "libdpdk", "version": version}])
        worker = dict(self.base("production_M4_batch_service_net_ring_worker_packets"), schema_version=1, returncode=0,
                      binary_sha256=binaries[production.M4_WORKER], packets={"received": 272, "forwarded": 200, "dropped": 72})
        restart = dict(self.base("M5_configuration_fresh_process_real_net_ring_worker"), binary_sha256=binaries[production.RESTART],
                       stages=[{"name": name, "returncode": 0, "state_sha256": digest("state")} for name in ("before", "after")],
                       packets={"received": 96, "forwarded": 48, "dropped": 48})
        evidence = {"baseline": baseline, "m4": m4, "m3": m3, "m2": m2, "m4_worker": worker, "restart": restart}
        evidence.update({kind: self.feature(kind, binaries) for kind in production.STEPS})
        result["evidence"] = evidence
        return result

    def check(self):
        return production.complete_software(self.source, self.contract, self.profiles, self.tap,
                                            self.taps, self.repro, self.benchmarks)

    def reject(self, mutations):
        """每次从同一套完整证据出发，仅改变被检查的边界"""
        for index, mutation in enumerate(mutations):
            with self.subTest(mutation=index):
                self.setUp()
                mutation()
                with self.assertRaises(ValueError):
                    self.check()

    def test_complete_evidence_reuses_all_eight_frozen_gates(self):
        with mock.patch.object(verify_m4, "complete_software", wraps=verify_m4.complete_software) as original:
            gates = self.check()
        self.assertEqual(set(gates), set(self.contract["performance_gates"]))
        self.assertTrue(all(gates.values()))
        original.assert_called_once()
        self.assertTrue(all(profile["hardware_acceptance"] == "not_run" for profile in self.profiles))
        self.assertTrue(all(not profile["complete_m5_software_validation"] for profile in self.profiles))

    def test_profile_schema_failure_scope_and_skips(self):
        self.reject((lambda: self.profiles[0].update(schema_version=2),
                     lambda: self.profiles[0].update(schema_version=True),
                     lambda: self.profiles[0].update(success=1),
                     lambda: self.profiles[0].update(scope="one_feature_only"),
                     lambda: self.profiles[0].update(error="ignored"),
                     lambda: self.profiles[0].update(skipped=1),
                     lambda: self.profiles[0].update(hardware_acceptance="passed"),
                     lambda: self.profiles[0].update(complete_m5_software_validation=True),
                     lambda: self.profiles[0].pop("complete_m5_validation")))

    def test_source_and_binary_identity(self):
        self.reject((lambda: self.profiles[0].update(source_sha256={"old": digest("old")}),
                     lambda: self.profiles[0].update(source_after_sha256={"old": digest("old")}),
                     lambda: self.profiles[0]["binary_after_sha256"].update(dppd=digest("changed")),
                     lambda: self.profiles[0]["binary_sha256"].pop(production.STALL),
                     lambda: self.profiles[0]["evidence"]["northbound"]["binary_sha256"].update(dppd=digest("old")),
                     lambda: self.profiles[0]["evidence"]["restart"].update(binary_sha256=digest("old")),
                     lambda: self.profiles[0]["evidence"]["restart"]["stages"][1].update(state_sha256=digest("changed"))))

    def test_all_four_profiles_are_required_and_unique(self):
        self.reject((lambda: self.profiles.pop(),
                     lambda: self.profiles.__setitem__(3, copy.deepcopy(self.profiles[0])),
                     lambda: self.profiles[0].update(dpdk="24.11.9"),
                     lambda: self.profiles[0]["build_options"].update(b_sanitize="address"),
                     lambda: self.profiles[0]["build_options"].update(werror=False),
                     lambda: self.profiles[0]["build_options"].update(b_ndebug="true")))

    def test_unit_inventory_cannot_hide_missing_failed_or_repeated_tests(self):
        self.reject((lambda: self.profiles[0]["unit_results"].pop(),
                     lambda: self.profiles[0]["expected_unit_names"].pop(),
                     lambda: self.profiles[0]["unit_results"][0].update(result="SKIP"),
                     lambda: self.profiles[0]["unit_results"][0].update(returncode=False),
                     lambda: self.profiles[0]["unit_results"][0].update(returncode=1),
                     lambda: self.profiles[0]["unit_results"][0].update(name="invented"),
                     lambda: self.profiles[0]["unit_results"].__setitem__(1, self.profiles[0]["unit_results"][0]),
                     lambda: self.profiles[0].update(unit_passed=1),
                     lambda: self.profiles[0].pop("unit_log_sha256")))

    def test_inner_python_skip_and_sanitizer_output_are_rejected(self):
        self.reject((lambda: self.profiles[0]["unit_results"][0].update(stderr="Ran 9 tests\nOK (skipped=1)"),
                     lambda: self.profiles[0]["unit_results"][0].update(stdout="Skipped: 2\n"),
                     lambda: self.profiles[0]["unit_results"][0].update(stderr="ERROR: AddressSanitizer: heap-use-after-free"),
                     lambda: self.profiles[0]["unit_results"][0].update(stderr="runtime error: null pointer"),
                     lambda: self.profiles[0].update(sanitizer_errors=1),
                     lambda: self.profiles[0].update(scanned_logs_sha256={})))

    def test_meson_project_prefix_and_zero_skips_remain_accepted(self):
        for result in self.profiles[0]["unit_results"]:
            result["name"] = "dppd:" + result["name"]
            result["stderr"] = "OK (skipped=0)\nSkipped: 0\n"
        self.check()

    def test_exact_nested_baseline_and_production_steps(self):
        self.reject((lambda: self.profiles[0]["steps"].pop(),
                     lambda: self.profiles[0]["steps"][0].update(returncode=1),
                     lambda: self.profiles[0]["evidence"].pop("m3"),
                     lambda: self.profiles[0]["evidence"]["baseline"].update(durable_requests_validated=False),
                     lambda: self.profiles[0]["evidence"]["m4"]["coverage"].pop(),
                     lambda: self.profiles[0]["evidence"]["m3"]["steps"].pop(),
                     lambda: self.profiles[0]["evidence"]["m2"]["steps"][0].update(error="failed"),
                     lambda: self.profiles[0]["evidence"]["northbound"]["steps"].pop(),
                     lambda: self.profiles[0]["evidence"]["release"]["steps"].__setitem__(1, production.STEPS["release"][0]),
                     lambda: self.profiles[0]["evidence"]["m4_worker"]["packets"].update(forwarded=199),
                     lambda: self.profiles[0]["evidence"]["restart"]["stages"].pop()))

    def test_heartbeat_layout_and_release_limitations(self):
        self.reject((lambda: self.profiles[0]["evidence"]["observability"].update(stall_exit_seconds=10),
                     lambda: self.profiles[0]["evidence"]["observability"].update(stall_exit_seconds=float("nan")),
                     lambda: self.profiles[0]["evidence"]["observability"].pop("stop_stall_exit_seconds"),
                     lambda: self.profiles[0]["evidence"]["observability"].update(stop_stall_exit_seconds=5),
                     lambda: self.profiles[0]["evidence"]["observability"]["layout"][0].update(lcore=3),
                     lambda: self.profiles[0]["evidence"]["observability"].update(numa_performance_acceptance="passed"),
                     lambda: self.profiles[0]["evidence"]["release"].update(lossless_claim=True),
                     lambda: self.profiles[0]["evidence"]["release"].pop("package_sha256")))

    def test_release_version_and_fault_scope_is_required(self):
        for field in ("release_pair", "incompatible_reader", "failed_start"):
            for value in (None, "historical_versions_verified"):
                with self.subTest(field=field, value=value):
                    report = copy.deepcopy(self.profiles[0])
                    if value is None:
                        report["evidence"]["release"].pop(field)
                    else:
                        report["evidence"]["release"][field] = value
                    with self.assertRaises(ValueError):
                        production.validate_profile(report, self.source)

    def test_complete_m2_tap_cannot_be_replaced_with_smoke_tests(self):
        self.reject((lambda: self.tap.update(complete_m2_validation=False),
                     lambda: self.tap.update(tap_requested=False),
                     lambda: self.tap["steps"].pop(),
                     lambda: self.tap["steps"][0].update(returncode=1),
                     lambda: self.tap.update(source_sha256={"old": digest("old")})))

    def test_tap_categories_builds_packets_and_old_binaries(self):
        self.reject((lambda: self.taps.pop("requests"),
                     lambda: self.taps["release"].pop(),
                     lambda: self.taps["daemon"].__setitem__(1, copy.deepcopy(self.taps["daemon"][0])),
                     lambda: self.taps["requests"][0]["binary_sha256"].update(dppd=digest("old")),
                     lambda: self.taps["daemon"][0]["binary_sha256"].update({production.FAULT: digest("old")}),
                     lambda: self.taps["requests"][0]["packets"].update(forwarded=23),
                     lambda: self.taps["release"][0].update(package_sha256=digest("another package")),
                     lambda: self.taps["release"][0].update(forwarded=0),
                     lambda: self.taps["daemon"][0].update(packets=0),
                     lambda: self.taps["daemon"][0].update(tap=False),
                     lambda: self.taps["release"][0].update(success=False)))

    def test_reproducibility_requires_two_equal_builds_and_exact_source_package(self):
        self.reject((lambda: self.repro.update(schema=2),
                     lambda: self.repro.update(success=False),
                     lambda: self.repro.pop("code_source_sha256"),
                     lambda: self.repro.update(code_source_sha256={"old": digest("old")}),
                     lambda: self.repro.update(source_sha256=digest("different docs")),
                     lambda: self.repro["builds"].pop(),
                     lambda: self.repro["builds"][0].pop("dppctl"),
                     lambda: self.repro["builds"][1].update(dppd=digest("different binary")),
                     lambda: self.repro["build_metadata"].pop("compiler"),
                     lambda: self.repro["build_metadata"]["dependencies"][0].update(version="24.11.8")))

    def test_frozen_performance_thresholds_are_not_relaxed(self):
        self.reject((lambda: self.benchmarks.pop(),
                     lambda: self.benchmarks.__setitem__(3, copy.deepcopy(self.benchmarks[0])),
                     lambda: self.benchmarks[0].update(source_sha256={"old": digest("old")}),
                     lambda: self.benchmarks[0].update(contract_sha256=digest("lowered contract")),
                     lambda: self.benchmarks[0]["runs"].pop(),
                     lambda: self.benchmarks[1]["runs"][0].update(queues=1),
                     lambda: self.benchmarks[0]["runs"][0].pop("cases"),
                     lambda: self.benchmarks[0]["runs"][0]["cases"][0].update(request_p99_ns=-1),
                     lambda: self.benchmarks[0]["runs"][0].update(snapshot_replay_rules=1),
                     lambda: self.benchmarks[1]["runs"][0].update(install_ns=float("nan")),
                     lambda: self.benchmarks[0]["performance_gates"].clear(),
                     lambda: self.benchmarks[0]["median_cases"][0].update(rules_per_second=1),
                     lambda: self.benchmarks[1].update(median_completion_p99_ns=100000001),
                     lambda: self.benchmarks[2].update(median_snapshot_replay_ns=6000000000),
                     lambda: self.benchmarks[0]["build_options"].update(b_ndebug="true")))

    def test_duplicate_json_keys_and_nonfinite_values_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "report.json"
            for text in ('{"success":false,"success":true}', '{"nested":{"a":1,"a":2}}', '{"time":NaN}'):
                path.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError):
                    production.read_json(path)

    def arguments(self, directory):
        """给命令行放入完整的测试样例，便于验证失败时旧成功报告会被撤销"""
        args = ["verify_m5_production.py", "--complete-software", "--report-dir", str(directory / "result")]
        entries = [("profile-report", value) for value in self.profiles] + [("tap-report", self.tap)]
        entries += [(kind + "-tap-report", value) for kind, reports in self.taps.items() for value in reports]
        entries += [("reproducibility-report", self.repro)] + [("benchmark-report", value) for value in self.benchmarks]
        for index, (option, value) in enumerate(entries):
            path = directory / (str(index) + ".json")
            path.write_text(json.dumps(value), encoding="utf-8")
            args.extend(("--" + option, str(path)))
        return args

    def call_main(self, arguments):
        with mock.patch.object(sys, "argv", arguments), mock.patch.object(production, "source_digest", return_value=self.source), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return production.main()

    def test_cli_only_marks_complete_after_all_inputs_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            args = self.arguments(directory)
            self.assertEqual(self.call_main(args), 0)
            result = json.loads((directory / "result/report.json").read_text(encoding="utf-8"))
            self.assertTrue(result["complete_m5_software_validation"])
            self.assertFalse(result["complete_m5_validation"])
            self.assertEqual(result["hardware_acceptance"], "not_run")
            self.assertEqual(len(result["inputs"]), 22)
            with mock.patch.object(production, "complete_software", side_effect=TypeError("malformed nested input")):
                self.assertEqual(self.call_main(args), 1)
            result = json.loads((directory / "result/report.json").read_text(encoding="utf-8"))
            self.assertFalse(result["success"])
            self.assertFalse(result["complete_m5_software_validation"])

    def test_cli_repeated_path_or_missing_argument_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            args = self.arguments(directory)
            args[args.index("--profile-report") + 1] = args[args.index("--tap-report") + 1]
            self.assertEqual(self.call_main(args), 1)
            result = json.loads((directory / "result/report.json").read_text(encoding="utf-8"))
            self.assertIn("路径重复", result["error"])
            self.assertEqual(self.call_main(["verify_m5_production.py", "--report-dir", str(directory / "result")]), 1)
            result = json.loads((directory / "result/report.json").read_text(encoding="utf-8"))
            self.assertFalse(result["success"])

    def test_profile_runner_invokes_formal_checks_without_copying_host_environment(self):
        """只替换进程调用，保留真实文件读取和最终校验以发现报告装配遗漏"""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            build, destination = directory / "build", directory / "report"
            (build / "meson-info").mkdir(parents=True)
            (build / "meson-logs").mkdir()
            destination.mkdir()
            profile = self.profile("21.11.9", "debugoptimized", "none")
            for name in production.BINARIES:
                path = build / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(name.encode())
            binaries = production.binary_hashes(build)
            profile["binary_sha256"] = profile["binary_after_sha256"] = binaries
            profile["evidence"]["m4_worker"]["binary_sha256"] = binaries[production.M4_WORKER]
            profile["evidence"]["restart"]["binary_sha256"] = binaries[production.RESTART]
            for kind in production.STEPS:
                profile["evidence"][kind] = self.feature(kind, binaries)
            metadata = {"meson-info.json": {"directories": {"source": str(ROOT)}},
                        "intro-buildoptions.json": [{"name": name, "value": value} for name, value in self.options().items()],
                        "intro-dependencies.json": [{"name": "libdpdk", "version": "21.11.9"}],
                        "intro-tests.json": [{"name": name} for name in profile["expected_unit_names"]]}
            for name, value in metadata.items():
                (build / "meson-info" / name).write_text(json.dumps(value), encoding="utf-8")
            tests = [dict(test, env={"SECRET": "private-host-value"}) for test in profile["unit_results"]]
            (build / "meson-logs/testlog.json").write_text("\n".join(json.dumps(test) for test in tests), encoding="utf-8")
            calls = []

            def run(command, root, output, timeout):
                calls.append(command)
                if "--report-dir" in command:
                    child = Path(command[command.index("--report-dir") + 1])
                    if child.name == "baseline":
                        for name, relative in production.CHILD_PATHS.items():
                            if relative.startswith("baseline/"):
                                path = destination / relative
                                path.parent.mkdir(parents=True, exist_ok=True)
                                path.write_text(json.dumps(profile["evidence"][name]), encoding="utf-8")
                    else:
                        child.mkdir(parents=True, exist_ok=True)
                        (child / "report.json").write_text(json.dumps(profile["evidence"][child.name]), encoding="utf-8")
                output.write("formal check passed\n")
                return 0

            report = dict(self.base(production.PROFILE_SCOPE), schema_version=1, success=False, steps=[],
                          complete_m5_validation=False, complete_m5_software_validation=False)
            with mock.patch.object(production, "run_command", run), \
                    mock.patch.object(production, "source_digest", return_value=self.source):
                production.run_profile(ROOT, build, destination, report)
            self.assertEqual(len(calls), 5)
            self.assertEqual([Path(command[1]).name for command in calls[1:]],
                             ["verify_m5.py", "m5_northbound.py", "m5_observability.py", "m5_release.py"])
            self.assertNotIn("private-host-value", json.dumps(report))
            self.assertNotIn("env", report["unit_results"][0])
            self.assertFalse(report["complete_m5_software_validation"])
            self.assertEqual(report["binary_sha256"], report["binary_after_sha256"])


if __name__ == "__main__":
    unittest.main()
