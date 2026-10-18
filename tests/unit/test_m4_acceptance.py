#!/usr/bin/env python3
"""联合报告必须拒绝缺项、旧源码和未达标数值，不能只相信成功标记"""

import copy
import contextlib
import io
import json
from pathlib import Path
import sys
import unittest
from unittest import mock
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from verify_m4 import CONTRACT_SHA256, FEATURE_TESTS, SCOPE, complete_software
import verify_m4


class Acceptance(unittest.TestCase):
    def setUp(self):
        self.source = {"lib/control/flow_service.c": "current"}
        self.contract = json.loads((Path(__file__).resolve().parents[2] / "docs/m4_acceptance_contract.json").read_text())
        base = {"success": True, "source_sha256": self.source}
        profile = dict(base, scope=SCOPE, hardware_acceptance="not_run", coverage=list(FEATURE_TESTS),
                       dpdk="21.11.9", build_options={"tests": True, "werror": True, "b_ndebug": "false",
                                                    "buildtype": "debugoptimized", "b_sanitize": "none"})
        self.profiles = [copy.deepcopy(profile) for _ in range(3)]
        self.profiles[1]["dpdk"] = "24.11.8"
        self.profiles[2]["build_options"]["b_sanitize"] = "address,undefined"
        names = ("compile", "unit", "software_traffic", "ipv4_traffic", "ipv4_routing", "acl_traffic",
                 "conntrack_nat", "dnat_traffic", "ipv6_traffic", "nat66_traffic", "nd6_traffic", "tunnel_traffic")
        self.tap = dict(base, tap_requested=True, complete_m2_validation=True,
                        steps=[{"name": name, "returncode": 0} for name in names])
        self.benchmarks = []
        for rules in (4096, 16384):
            common = dict(base, rules=rules, repetitions=5, contract_sha256=CONTRACT_SHA256, cpu_affinity=[0, 1, 2, 3],
                          performance_gates={"recorded": True})
            control = dict(common, scope="production_control_with_memory_driver_and_software_backend", runs=[{}] * 5,
                           median_cases=[{"rules_per_second": 2000000, "request_p99_ns": 4000}, {},
                                         {"rules_per_second": 5000, "request_p99_ns": 500000}],
                           median_residual_cleanup_ns=1000, median_snapshot_replay_ns=1000000)
            queue = dict(common, scope="production_async_batch_service_with_memory_driver",
                         runs=[{"rules": rules, "batch": 64, "queues": 8, "queue_depth": 256,
                                "window_rules": 2048}] * 5, median_completion_p99_ns=5000000)
            self.benchmarks += [control, queue]

    def check(self):
        return complete_software(self.source, self.contract, self.profiles, self.tap, self.benchmarks)

    def test_complete_software_is_separate_from_hardware(self):
        self.assertEqual(set(self.check()), set(self.contract["performance_gates"]))

    def test_incomplete_or_stale_evidence_is_rejected(self):
        mutations = (
            lambda: self.profiles.pop(),
            lambda: self.profiles[1].update(source_sha256={"old": "source"}),
            lambda: self.profiles[1]["coverage"].pop(),
            lambda: self.profiles[2]["build_options"].update(b_ndebug="true"),
            lambda: self.tap["steps"].pop(),
            lambda: self.tap["steps"][0].update(returncode=1),
            lambda: self.benchmarks.pop(),
            lambda: self.benchmarks[0].update(contract_sha256="changed"),
            lambda: self.benchmarks[0]["runs"].pop(),
            lambda: self.benchmarks[1]["runs"][0].update(queues=1),
            lambda: self.benchmarks[0]["median_cases"][0].update(rules_per_second=1),
            lambda: self.benchmarks[1].update(median_completion_p99_ns=100000001),
            lambda: self.benchmarks[2].update(median_snapshot_replay_ns=6000000000),
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                self.setUp()
                mutation()
                with self.assertRaises(ValueError):
                    self.check()

    def test_malformed_final_input_cannot_leave_success_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "meson-info").mkdir()
            (build / "meson-logs").mkdir()
            (build / "meson-info/intro-buildoptions.json").write_text("[]")
            (build / "meson-info/intro-dependencies.json").write_text('[{"name":"libdpdk","version":"21.11.9"}]')
            (build / "meson-logs/testlog.json").write_text("\n".join(
                json.dumps({"name": name, "result": "OK", "returncode": 0}) for name in FEATURE_TESTS))
            (build / "tap.json").write_text(json.dumps(self.tap))

            def run(command, root, output, timeout):
                directory = Path(command[command.index("--report-dir") + 1])
                directory.mkdir(parents=True, exist_ok=True)
                (directory / "report.json").write_text(json.dumps({"success": True, "source_sha256": self.source}))
                return 0

            arguments = ["verify_m4.py", "--build-dir", str(build), "--complete-software",
                         "--tap-report", str(build / "tap.json")]
            with mock.patch.object(sys, "argv", arguments), mock.patch.object(verify_m4, "run_command", run), \
                    mock.patch.object(verify_m4, "source_digest", return_value=self.source), \
                    mock.patch.object(verify_m4, "complete_software", side_effect=TypeError("malformed input")), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(verify_m4.main(), 1)
            report = json.loads((build / "m4-acceptance/report.json").read_text())
            self.assertFalse(report["success"])
            self.assertFalse(report["complete_m4_software_validation"])


if __name__ == "__main__":
    unittest.main()
