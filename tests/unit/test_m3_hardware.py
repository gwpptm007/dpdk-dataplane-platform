#!/usr/bin/env python3
"""实机门禁的反例测试，夹具通过只说明验证器正确，不代表任何硬件验收完成"""

import copy
import base64
import errno
import json
from pathlib import Path
import struct
import sys
import tempfile
import signal
import os
import socket
import subprocess
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import verify_m3_hardware as HARDWARE
import m3_hardware_peer as PEER


def observed(item):
    return {"status": "observed", "value": item}


def configuration():
    endpoint = {"port_id": 0, "pci_address": "0000:03:00.0", "interface": "test0", "pmd": "mlx5_pci",
                "device_name": "0000:03:00.0", "firmware": "test-firmware", "role": "uplink", "switch_domain": 3}
    result = {"schema_version": 1, "stages": ["basic"], "ports": {name: copy.deepcopy(endpoint) for name in
                                           ("install", "source", "destination")}, "template": "pair", "rule_id": 50000,
              "eal_devargs": ["0000:03:00.0,representor=[0,1]"],
              "peer": {"ssh_argv": ["ssh", "test-peer"], "tx_interface": "peer0", "rx_interface": "peer1"},
              "physical_links": ["fixture source physical link", "fixture destination physical link"]}
    result["ports"]["destination"]["port_id"] = 1
    return result


def inventory():
    return {"management_detection": {"routes": [observed([{"dst": "default", "dev": "management0"}]), observed([])], "addresses": observed([]),
                                      "interfaces": ["management0"]},
            "pci_scan": {"network_devices": [{"pci_address": "0000:03:00.0",
                                               "classification": {"kind": "physical_candidate"}}]},
            "interface_scan": {"interfaces": [
                {"name": "management0", "pci_device": observed("0000:02:00.0"), "management_interface": True},
                {"name": "test0", "pci_device": observed("0000:03:00.0"), "management_interface": False}]}}


def fixture_gate(report, config):
    return HARDWARE.hardware_gate(report, config, environ={}, parent_reader=lambda bdf: bdf)


def generic_configuration():
    config = configuration()
    for endpoint in config["ports"].values():
        endpoint.update(role="unknown", pmd="net_i40e")
    return config


def identity_reader(config, overrides=None):
    """只生成端口查询夹具，不声明真实 PMD 或硬件已经通过验收"""
    endpoints = {item["port_id"]: item for _, item in HARDWARE.configured_endpoints(config)}
    calls = []

    def command(operation, port):
        calls.append((operation, port))
        endpoint = endpoints[port]
        if operation == "port-show":
            role = endpoint["role"]
            observed = {"port": str(port), "driver": endpoint["pmd"], "device": endpoint["pci_address"],
                        "interface": endpoint["interface"], "ifindex": "7", "role": role,
                        "role-source": "unknown" if role == "unknown" else "mlx5-phys-port-name",
                        "role-error": "-95" if role == "unknown" else "0", "phys-port-name": "",
                        "kind": "ethdev" if role in ("unknown", "uplink") else "representor",
                        "switch-domain": str(endpoint["switch_domain"]), "switch-port": str(port)}
        elif operation == "capability-show":
            observed = {"device": endpoint["device_name"], "driver": endpoint["pmd"], "dpdk": "DPDK 21.11.9",
                        "firmware": endpoint["firmware"], "firmware-error": "0", "configured": "yes", "started": "yes"}
        else:
            raise AssertionError("身份门禁不应创建规则或发包: " + operation)
        observed.update((overrides or {}).get((operation, port), {}))
        return {"returncode": 0, "stdout": " ".join(key + "=" + item for key, item in observed.items()) + "\n",
                "stderr": ""}

    return command, calls


def pci_configuration(pmd="net_sfc_efx"):
    config = configuration()
    base = {"port_id": 0, "pci_address": "0000:03:00.0", "device_name": "0000:03:00.0", "pmd": pmd,
            "interface": "", "phys_port_name": "", "ifindex": 0, "kind": "ethdev", "role": "unknown",
            "role_source": "unknown", "role_error": -19, "switch_domain": 3, "switch_port": 100,
            "pci_vendor": "0x1924" if pmd == "net_sfc_efx" else "0x14e4", "pci_device": "0x1234",
            "pci_class": "0x020000", "pci_driver": "vfio-pci", "firmware": "1.2.3 rxAAA txBBB",
            "firmware_error": 0, "firmware_scope": {"type": "self"}}
    def representor(number):
        suffix = "c0pf0vf" + str(number) if pmd == "net_sfc_efx" else str(number)
        return dict(base, port_id=number, switch_port=100 + number, kind="representor", firmware="unknown", firmware_error=-95,
                    device_name="net_0000:03:00.0_representor_" + suffix,
                    firmware_scope={"type": "parent-pci", "source_port_id": 0})
    config["ports"] = {"install": copy.deepcopy(base), "source": representor(1), "destination": representor(2)}
    config["identity_provider"] = {"type": "pci_only", "pmd": pmd, "backers": [copy.deepcopy(base)]}
    return config


def pci_inventory(config):
    data = inventory(); endpoint = config["ports"]["install"]
    device = data["pci_scan"]["network_devices"][0]
    device.update({key: observed(endpoint[name]) for key, name in
                   (("class", "pci_class"), ("vendor", "pci_vendor"), ("device", "pci_device"), ("driver", "pci_driver"))})
    data["interface_scan"]["interfaces"] = data["interface_scan"]["interfaces"][:1]
    return data


def pci_identity_reader(config, overrides=None):
    endpoints = {item["port_id"]: item for _, item in HARDWARE.configured_endpoints(config)}
    def command(operation, port, **unused):
        del unused
        endpoint = endpoints[int(port)]
        if operation == "port-show":
            result = {"device": endpoint["pci_address"], "driver": endpoint["pmd"], "kind": endpoint["kind"],
                      "ifindex": "0", "interface": "", "phys-port-name": "", "role": "unknown", "role-source": "unknown",
                      "role-error": "-19", "switch-domain": str(endpoint["switch_domain"]), "switch-port": str(endpoint["switch_port"])}
        elif operation == "capability-show":
            result = {"device": endpoint["device_name"], "driver": endpoint["pmd"], "kind": endpoint["kind"],
                      "dpdk": "DPDK 21.11.9", "firmware": endpoint["firmware"], "firmware-error": str(endpoint["firmware_error"]),
                      "configured": "yes", "started": "yes"}
        else:
            raise AssertionError(operation)
        result.update((overrides or {}).get((operation, int(port)), {}))
        return {"returncode": 0, "stdout": " ".join(key + "=" + item for key, item in result.items()) + "\n", "stderr": ""}
    return command


class PciOnlyTests(unittest.TestCase):
    def test_both_typed_pm_ds_preserve_raw_repr_firmware_and_unknown_identity(self):
        for pmd in HARDWARE.PCI_ONLY_PMDS:
            config = pci_configuration(pmd); HARDWARE.validate_config(config)
            self.assertEqual(fixture_gate(pci_inventory(config), config), ["0000:02:00.0"])
            actual = HARDWARE.runtime_port_identities(config, pci_identity_reader(config))
            self.assertEqual(actual["1"]["capability"]["firmware-error"], "-95")
            self.assertEqual(actual["1"]["port"]["role-error"], "-19")
            self.assertEqual(actual["0"]["capability"]["firmware"], "1.2.3 rxAAA txBBB")
            config.pop("identity_provider")
            with self.assertRaises(HARDWARE.Blocked): HARDWARE.validate_config(config)

    def test_typed_profile_never_enables_other_stages_templates_roles_or_netdev_fallback(self):
        for key, replacement in (("stages", ["basic", "age"]), ("stages", ["basic", "shared_count"]),
                                 ("stages", ["basic", "planned_reset"]), ("template", "vf-vf")):
            config = pci_configuration(); config[key] = replacement
            with self.subTest(key=key, replacement=replacement), self.assertRaises(HARDWARE.Blocked): HARDWARE.validate_config(config)
        for key, replacement in (("ifindex", 4), ("interface", "fake0"), ("phys_port_name", "p0"),
                                 ("role", "vf"), ("has_pf", True), ("kind", "fake"), ("pmd", "net_fake")):
            config = pci_configuration(); config["ports"]["source"][key] = replacement
            with self.subTest(key=key), self.assertRaises(HARDWARE.Blocked): HARDWARE.validate_config(config)

    def test_parent_firmware_scope_cannot_be_faked_rebound_or_used_after_failure(self):
        for change in ("repr-known", "missing-parent", "repr-parent", "cross-pci", "same-id-other-name", "parent-failed"):
            config = pci_configuration(); endpoint = config["ports"]["source"]
            if change == "repr-known": endpoint.update(firmware="fake", firmware_error=0)
            if change == "missing-parent": endpoint["firmware_scope"]["source_port_id"] = 99
            if change == "repr-parent": endpoint["firmware_scope"]["source_port_id"] = 2
            if change == "cross-pci": endpoint["pci_address"] = "0000:04:00.0"
            if change == "same-id-other-name": endpoint["port_id"] = 0
            if change == "parent-failed": config["identity_provider"]["backers"][0]["firmware_error"] = -95
            with self.subTest(change=change), self.assertRaises(HARDWARE.Blocked): HARDWARE.validate_config(config)

    def test_actual_identity_opaque_port_and_fw_errors_are_not_replaced_by_configuration(self):
        for operation, change in (("port-show", {"ifindex": "4"}), ("port-show", {"interface": "fake0"}),
                                  ("port-show", {"pf": "0"}), ("port-show", {"switch-port": "999"}),
                                  ("port-show", {"device": "0000:04:00.0"}), ("port-show", {"kind": "ethdev"}),
                                  ("capability-show", {"firmware": "fake", "firmware-error": "0"}),
                                  ("capability-show", {"device": "net_0000:03:00.0_representor_c0pf0vf9"})):
            config = pci_configuration(); reader = pci_identity_reader(config, {(operation, 1): change})
            with self.subTest(change=change), self.assertRaises(HARDWARE.Blocked): HARDWARE.runtime_port_identities(config, reader)

    def test_pci_inventory_and_management_parent_are_independent_of_no_netdev_declaration(self):
        config = pci_configuration()
        for field in ("class", "vendor", "device", "driver"):
            data = pci_inventory(config); data["pci_scan"]["network_devices"][0][field] = observed("wrong")
            with self.subTest(field=field), self.assertRaises(HARDWARE.Blocked): fixture_gate(data, config)
        with self.assertRaises(HARDWARE.Blocked):
            HARDWARE.hardware_gate(pci_inventory(config), config, environ={}, parent_reader=lambda unused: "0000:02:00.0")

    def test_eal_allowlist_parses_real_options_and_rejects_app_values_aliases_and_conflicts(self):
        config = pci_configuration(); devargs = config["eal_devargs"][0]
        for option in (["-a", devargs], ["--allow=" + devargs], ["-a" + devargs], ["--allowlist", devargs]):
            actual = {"cmdline": ["dppd", "-l", "0,1", "--file-prefix", "test"] + option + ["--"]}
            self.assertEqual(HARDWARE.pci_allowlist_evidence(config, actual)[0]["devargs"], devargs)
        for options in (["--", "-a", devargs], ["--file-prefix", "-a" + devargs, "--"],
                        ["-a", "0000:03:00.00", "--"], ["-a", devargs + "extra", "--"],
                        ["-a", devargs, "--no-pci", "--"], ["-a", devargs, "--vdev=net_ring0", "--"],
                        ["-a", devargs, "-b", "0000:04:00.0", "--"]):
            with self.subTest(options=options), self.assertRaises(HARDWARE.Blocked):
                HARDWARE.pci_allowlist_evidence(config, {"cmdline": ["dppd"] + options})

    def test_real_wrapper_accepts_attached_allow_options_and_only_uses_private_count(self):
        for option in ("--allow=", "-a"):
            report, calls, _, error = ProcessSafetyTests().run_fixture(typed_allow=option)
            self.assertIsNone(error)
            self.assertTrue(report["identity_provider"]["validated"])
            self.assertEqual(report["identity_provider"]["firmware_scopes"]["1"]["raw_error"], -95)
            self.assertTrue(report["cleanup"]["rule_deleted"])
            self.assertFalse(any(name.startswith(("shared-count", "rule-age", "device-reset")) for name, _ in calls))

    def test_delayed_service_counter_requires_exact_bounded_result_without_overshoot(self):
        now = [0.0]
        def sleep(seconds): now[0] += seconds
        def counter(hits): return {"stdout": f"count rule=1 generation=7 hits={hits} bytes={hits * 96}"}
        with patch.object(HARDWARE.time, "monotonic", side_effect=lambda: now[0]), patch.object(HARDWARE.time, "sleep", side_effect=sleep):
            results = iter([counter(0), counter(1), counter(2)])
            self.assertEqual(HARDWARE.wait_private_count(lambda *unused: next(results), 1, 7,
                             {"hits": 0, "bytes": 0}, [b"x" * 96], 2), {"hits": 2, "bytes": 192})
            with self.assertRaises(RuntimeError):
                HARDWARE.wait_private_count(lambda *unused: counter(3), 1, 7, {"hits": 0, "bytes": 0}, [b"x" * 96], 2)
            with self.assertRaises(RuntimeError):
                HARDWARE.wait_private_count(lambda *unused: counter(0), 1, 7, {"hits": 0, "bytes": 0}, [b"x" * 96], 2, timeout=0.03)


class GenericIdentityTests(unittest.TestCase):
    def test_explicit_unknown_pair_advances_only_with_complete_independent_identity(self):
        config = generic_configuration()
        self.assertIs(HARDWARE.validate_config(config), config)
        self.assertEqual(fixture_gate(inventory(), config), ["0000:02:00.0"])
        command, calls = identity_reader(config)
        identities = HARDWARE.runtime_port_identities(config, command, ifindex_reader=lambda unused: 7)
        for identity in identities.values():
            self.assertEqual(identity["port"]["role"], "unknown")
            self.assertEqual(identity["port"]["role-source"], "unknown")
            self.assertEqual(identity["port"]["role-error"], "-95")
        self.assertTrue(all(operation in ("port-show", "capability-show") for operation, _ in calls))
        self.assertTrue(all(row["status"] in ("blocked", "out_of_scope") for row in HARDWARE.matrix()))

    def test_all_six_named_templates_reject_unknown_source_or_destination_before_queries(self):
        for template, roles in HARDWARE.TEMPLATE_ROLES.items():
            for missing in ("source", "destination"):
                config = generic_configuration(); config["template"] = template
                for name, role in zip(("source", "destination"), roles): config["ports"][name]["role"] = role
                config["ports"][missing]["role"] = "unknown"
                command, calls = identity_reader(config)
                with self.subTest(template=template, missing=missing):
                    with self.assertRaisesRegex(HARDWARE.Blocked, "UNKNOWN"):
                        HARDWARE.validate_config(config)
                    with self.assertRaises(HARDWARE.Blocked):
                        HARDWARE.runtime_port_identities(config, command, ifindex_reader=lambda unused: 7)
                    self.assertFalse(calls)

    def test_named_template_still_allows_a_separate_unknown_install_port(self):
        config = generic_configuration(); config["template"] = "uplink-vf"
        config["ports"]["install"]["port_id"] = 2
        config["ports"]["source"].update(role="uplink", pmd="mlx5_pci")
        config["ports"]["destination"].update(role="vf", pmd="mlx5_pci")
        HARDWARE.validate_config(config)
        command, _ = identity_reader(config)
        identities = HARDWARE.runtime_port_identities(config, command, ifindex_reader=lambda unused: 7)
        self.assertEqual(identities["2"]["port"]["role"], "unknown")

    def test_unknown_pair_without_host_netdev_or_positive_matching_ifindex_remains_blocked(self):
        variants = ({"ifindex": "0"}, {"ifindex": ""}, {"ifindex": "unknown"}, {"interface": ""})
        for replacement in variants:
            config = generic_configuration()
            command, _ = identity_reader(config, {("port-show", 0): replacement})
            with self.subTest(replacement=replacement), self.assertRaises(HARDWARE.Blocked):
                HARDWARE.runtime_port_identities(config, command, ifindex_reader=lambda unused: 7)
        config = generic_configuration(); command, _ = identity_reader(config)
        with self.assertRaises(HARDWARE.Blocked):
            HARDWARE.runtime_port_identities(config, command, ifindex_reader=lambda unused: 99)
        def absent(unused): raise OSError(errno.ENODEV, "fixture host netdev absent")
        with self.assertRaises(HARDWARE.Blocked): HARDWARE.runtime_port_identities(config, command, ifindex_reader=absent)

    def test_unknown_pair_does_not_relax_switch_domain_port_alias_firmware_or_management_parent(self):
        variants = {("port-show", 1): {"switch-domain": "none"}}
        for overrides in (variants, {("port-show", 1): {"switch-port": "0"}},
                          {("port-show", 1): {"switch-port": "unknown"}},
                          {("capability-show", 1): {"firmware": "unknown", "firmware-error": "-95"}}):
            config = generic_configuration(); command, _ = identity_reader(config, overrides)
            with self.subTest(overrides=overrides), self.assertRaises(HARDWARE.Blocked):
                HARDWARE.runtime_port_identities(config, command, ifindex_reader=lambda unused: 7)
        with self.assertRaises(HARDWARE.Blocked):
            HARDWARE.hardware_gate(inventory(), generic_configuration(), environ={}, parent_reader=lambda unused: "0000:02:00.0")

    def test_named_roles_cannot_be_derived_from_generic_role_source_or_wrong_endpoint_kind(self):
        config = generic_configuration(); config["template"] = "vf-vf"
        config["ports"]["install"]["port_id"] = 2
        config["ports"]["source"]["role"] = config["ports"]["destination"]["role"] = "vf"
        for replacement in ({"role-source": "unknown"}, {"role-source": "generic"},
                            {"role-error": "-95"}, {"kind": "ethdev"}):
            command, _ = identity_reader(config, {("port-show", 0): replacement})
            with self.subTest(replacement=replacement), self.assertRaises(HARDWARE.Blocked):
                HARDWARE.runtime_port_identities(config, command, ifindex_reader=lambda unused: 7)


class RepresentorNameTests(unittest.TestCase):
    def representor_config(self):
        config = configuration(); config["template"] = "uplink-vf"
        destination = config["ports"]["destination"]
        destination.update(role="vf", device_name=destination["pci_address"] + "_representor_vf1")
        return config

    def test_pci_parent_name_and_full_ethdev_name_are_independently_required(self):
        config = self.representor_config()
        HARDWARE.validate_config(config)
        fixture_gate(inventory(), config)
        command, _ = identity_reader(config)
        observed = HARDWARE.runtime_port_identities(config, command, ifindex_reader=lambda unused: 7)
        self.assertEqual(observed["1"]["port"]["device"], "0000:03:00.0")
        self.assertEqual(observed["1"]["capability"]["device"], "0000:03:00.0_representor_vf1")

    def test_ethdev_name_must_use_the_exact_complete_bdf_and_a_nonempty_representor_suffix(self):
        for name in ("0000:04:00.0_representor_vf1", "0000:03:00.00_representor_vf1", "0000:03:00.0evil",
                     "0000:03:00.0_representor_", "0000:03:00.0_representor_vf1/extra"):
            config = self.representor_config(); config["ports"]["destination"]["device_name"] = name
            with self.subTest(name=name), self.assertRaises(HARDWARE.Blocked): HARDWARE.validate_config(config)

    def test_parent_or_ethdev_suffix_mismatch_cannot_borrow_another_identity(self):
        variants = ({("port-show", 1): {"device": "0000:04:00.0"}},
                    {("port-show", 1): {"device": "0000:03:00.0_representor_vf1"}},
                    {("capability-show", 1): {"device": "0000:03:00.0"}},
                    {("capability-show", 1): {"device": "0000:03:00.0_representor_vf9"}})
        for replacement in variants:
            config = self.representor_config(); command, _ = identity_reader(config, replacement)
            with self.subTest(replacement=replacement), self.assertRaises(HARDWARE.Blocked):
                HARDWARE.runtime_port_identities(config, command, ifindex_reader=lambda unused: 7)
        sample = inventory(); sample["interface_scan"]["interfaces"][1]["pci_device"] = observed("0000:04:00.0")
        with self.assertRaises(HARDWARE.Blocked): fixture_gate(sample, self.representor_config())

    def test_planned_reset_checks_both_names_for_every_configured_port_including_extra_representor(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary); snapshot = directory / "snapshot"; snapshot.write_bytes(b"fixture")
            config = self.representor_config(); config["stages"] = ["basic", "planned_reset"]
            extra = dict(config["ports"]["destination"], port_id=2, device_name="0000:03:00.0_representor_vf9")
            config["planned_reset"] = {"allow_full_group_reset": True, "modes": ["count"],
                                       "snapshot_path": str(snapshot), "device_ports": [
                                           dict(config["ports"]["source"]), dict(config["ports"]["destination"]), extra]}
            HARDWARE.validate_config(config)
            for failure in (None, ("port-show", {"device": "0000:03:00.0_representor_vf9"}),
                            ("capability-show", {"device": "0000:03:00.0_representor_vf8"})):
                overrides = {} if failure is None else {(failure[0], 2): failure[1]}
                command, calls = identity_reader(config, overrides)
                with self.subTest(failure=failure), patch.object(HARDWARE, "inspect_environment", return_value=inventory()), \
                        patch.object(HARDWARE, "pci_parent", side_effect=lambda bdf: bdf), \
                        patch.object(HARDWARE, "reset_snapshot", return_value=snapshot), \
                        patch.object(HARDWARE, "read_device_status", return_value={"device_ports": [0, 1, 2]}) as status, \
                        patch.object(HARDWARE.socket, "if_nametoindex", return_value=7), \
                        patch.dict(HARDWARE.os.environ, {"SSH_CONNECTION": ""}):
                    if failure is None:
                        _, identities, _ = HARDWARE.reset_preflight(config, {}, command, {}, directory)
                        self.assertEqual(identities["2"]["port"]["device"], "0000:03:00.0")
                        self.assertEqual(identities["2"]["capability"]["device"], extra["device_name"])
                        status.assert_called_once()
                    else:
                        with self.assertRaises(HARDWARE.Blocked): HARDWARE.reset_preflight(config, {}, command, {}, directory)
                        status.assert_not_called()
                    self.assertIn(("capability-show", 2), calls)
                    self.assertTrue(all(operation in ("port-show", "capability-show") for operation, _ in calls))


class HardwareGateTests(unittest.TestCase):
    def test_candidate_only_allows_next_preflight_without_claiming_acceptance(self):
        self.assertEqual(fixture_gate(inventory(), configuration()), ["0000:02:00.0"])
        self.assertTrue(all(row["status"] in ("blocked", "out_of_scope") for row in HARDWARE.matrix()))

    def test_virtual_pci_cannot_be_renamed_into_hardware(self):
        for kind in ("virtual", "unknown", "missing"):
            report = inventory()
            report["pci_scan"]["network_devices"][0]["classification"]["kind"] = kind
            with self.subTest(kind=kind), self.assertRaises(HARDWARE.Blocked):
                fixture_gate(report, configuration())

    def test_software_pmd_cannot_borrow_physical_pci(self):
        for pmd in ("net_ring", "net_tap", "net_null", "net_pcap", "net_af_packet", "net_af_xdp", "net_vmxnet3"):
            config = configuration()
            config["ports"]["source"]["pmd"] = pmd
            with self.subTest(pmd=pmd), self.assertRaises(HARDWARE.Blocked):
                fixture_gate(inventory(), config)

    def test_management_port_and_same_parent_pci_are_always_protected(self):
        for unknown in (False, None, True):
            report = inventory()
            report["interface_scan"]["interfaces"][1]["management_interface"] = unknown
            if unknown is False:
                report["interface_scan"]["interfaces"][0]["pci_device"] = observed("0000:03:00.0")
            with self.subTest(flag=unknown), self.assertRaises(HARDWARE.Blocked):
                fixture_gate(report, configuration())

    def test_missing_management_observation_is_blocked(self):
        for field in ("route", "address", "pci", "names"):
            report = inventory()
            if field == "route": report["management_detection"]["routes"][1] = {"status": "permission_denied"}
            if field == "address": report["management_detection"]["addresses"] = {"status": "missing"}
            if field == "pci": report["interface_scan"]["interfaces"][0]["pci_device"] = {"status": "missing"}
            if field == "names": report["management_detection"]["interfaces"] = []
            with self.subTest(field=field), self.assertRaises(HARDWARE.Blocked):
                fixture_gate(report, configuration())

    def test_interface_and_runtime_device_must_match_the_pci(self):
        config = configuration()
        config["ports"]["source"]["interface"] = "not-observed"
        with self.assertRaises(HARDWARE.Blocked): fixture_gate(inventory(), config)
        config = configuration()
        config["ports"]["source"]["device_name"] = "net_ring0"
        with self.assertRaises(HARDWARE.Blocked): fixture_gate(inventory(), config)

    def test_parameters_require_explicit_real_connections(self):
        for field, replacement in (("physical_links", []), ("eal_devargs", ["--vdev=net_ring0"]),
                                   ("eal_devargs", ["--no-pci"]), ("rule_id", True), ("rule_id", 0)):
            config = configuration(); config[field] = replacement
            with self.subTest(field=field), self.assertRaises(HARDWARE.Blocked): HARDWARE.validate_config(config)
        config = configuration(); config["ports"]["source"]["switch_domain"] = 4
        with self.assertRaises(HARDWARE.Blocked): HARDWARE.validate_config(config)

    def test_peer_ssh_arguments_cannot_contain_remote_commands_or_proxy_shell(self):
        for argv in (["ssh", "peer", "touch", "/tmp/file"], ["ssh", "-o", "ProxyCommand=sh bad", "peer"],
                     ["ssh", "peer;bad"], ["ssh", "-p", "0", "peer"]):
            with self.subTest(argv=argv), self.assertRaises(HARDWARE.Blocked): HARDWARE.validate_ssh_argv(argv)
        self.assertEqual(HARDWARE.validate_ssh_argv(["ssh", "-o", "BatchMode=yes", "-p", "22", "user@peer"])[-1], "user@peer")

    def test_firmware_with_spaces_is_preserved(self):
        self.assertEqual(HARDWARE.fields("capability port=0 firmware=1.2 build 5 firmware-error=0\n")["firmware"],
                         "1.2 build 5")

    def test_wrong_source_build_and_disabled_strict_checks_are_blocked(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary); metadata = build / "meson-info"; metadata.mkdir()
            (metadata / "meson-info.json").write_text(json.dumps({"directories": {"source": str(ROOT / "wrong")}}))
            (metadata / "intro-buildoptions.json").write_text("[]")
            with self.assertRaises(HARDWARE.Blocked): HARDWARE.build_binding(ROOT, build)
            (metadata / "meson-info.json").write_text(json.dumps({"directories": {"source": str(ROOT)}}))
            (metadata / "intro-buildoptions.json").write_text(json.dumps([
                {"name": "tests", "value": True}, {"name": "werror", "value": False}]))
            with self.assertRaises(HARDWARE.Blocked): HARDWARE.build_binding(ROOT, build)

    def test_unknown_firmware_blocks_before_peer_packets_or_daemon_pause(self):
        config = configuration(); config.update(daemon_pid=1234, socket=str(ROOT / "build/fixture.sock"))
        port = "driver=mlx5_pci device=0000:03:00.0 interface=test0 role=uplink switch-domain=3\n"
        for firmware in ("firmware=unknown firmware-error=-5", "firmware=wrong firmware-error=0"):
            def command(argv, *unused, **options):
                del unused, options
                raw = "device=0000:03:00.0 driver=mlx5_pci dpdk=21.11.9 " + firmware + "\n"
                return {"returncode": 0, "stdout": raw if "capability-show" in argv else port, "stderr": ""}
            with tempfile.TemporaryDirectory() as temporary, self.subTest(firmware=firmware), \
                    patch.object(HARDWARE, "execute", side_effect=command), \
                    patch.object(HARDWARE, "build_binding", return_value={}), \
                    patch.object(HARDWARE, "process_identity", return_value={"cmdline": config["eal_devargs"]}), \
                    patch.object(HARDWARE, "peer") as peer, patch.object(HARDWARE.os, "kill") as kill:
                with self.assertRaises(HARDWARE.Blocked):
                    HARDWARE.live_run(ROOT, ROOT / "build", Path(temporary), config,
                                      {"capability_matrix": HARDWARE.matrix()})
                peer.assert_not_called(); kill.assert_not_called()

    def test_default_run_is_blocked_and_never_calls_traffic(self):
        with tempfile.TemporaryDirectory() as temporary:
            with patch.object(HARDWARE, "inspect_environment", return_value=inventory()), \
                    patch.object(HARDWARE, "build_binding", return_value={"source_sha256": {}}), \
                    patch.object(HARDWARE, "live_run") as traffic:
                self.assertEqual(HARDWARE.main(["--report-dir", temporary]), 2)
                traffic.assert_not_called()
            report = json.loads((Path(temporary) / "report.json").read_text(encoding="utf-8"))
            self.assertEqual(report["status"], "blocked")
            self.assertFalse(report["success"])
            self.assertFalse(report["complete_m3_validation"])
            self.assertEqual(report["hardware_acceptance"], "not_run")
            self.assertTrue(any(row["stage"] == "physical_reset_recovery" and row["status"] == "blocked"
                                for row in report["capability_matrix"]))

    def test_fake_success_fields_do_not_bypass_inventory(self):
        config = configuration(); config.update(success=True, hardware_acceptance="passed", worker_paused=True)
        report = inventory(); report["pci_scan"]["network_devices"][0]["classification"]["kind"] = "virtual"
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "config.json"; path.write_text(json.dumps(config))
            with patch.object(HARDWARE, "inspect_environment", return_value=report), \
                    patch.object(HARDWARE, "build_binding", return_value={}), patch.object(HARDWARE, "live_run") as run:
                self.assertEqual(HARDWARE.main(["--config", str(path), "--report-dir", temporary, "--execute"]), 2)
                run.assert_not_called()


class PacketEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.frames = [b"\x01" * 96, b"\x02" * 128]
        self.records = [(1.1, frame) for frame in self.frames for _ in range(3)]
        self.capture = PEER.pcap(self.records)
        self.before = {"hits": 7, "bytes": 100}
        self.after = {"hits": 13, "bytes": 772}
        self.workers = {"rx_packets": 10, "rx_bytes": 400, "tx_packets": 9, "tx_bytes": 300}

    def check_evidence(self, tx=None, rx=None, after=None, workers=None):
        return HARDWARE.verify_packet_evidence(self.capture if tx is None else tx, self.capture if rx is None else rx,
                                              self.frames, 3, self.before, self.after if after is None else after,
                                              self.workers, self.workers if workers is None else workers)

    def test_exact_frames_and_hardware_count_delta(self):
        self.assertEqual(self.check_evidence(), {"packets": 6, "bytes": 672, "byte_exact": True})

    def test_rewrite_duplicate_missing_and_stale_frame_are_rejected(self):
        variants = [self.records[:-1], self.records + [self.records[0]], [(1.1, b"\xff" * 96)] + self.records[1:]]
        for records in variants:
            with self.subTest(records=len(records)), self.assertRaises(ValueError):
                self.check_evidence(rx=PEER.pcap(records))

    def test_private_count_cannot_fake_missing_packet_or_byte_evidence(self):
        for after in ({"hits": 12, "bytes": 772}, {"hits": 13, "bytes": 773}, {"hits": 0, "bytes": 0}):
            with self.subTest(after=after), self.assertRaises(ValueError): self.check_evidence(after=after)

    def test_cpu_processing_or_missing_worker_field_is_rejected(self):
        for key in self.workers:
            changed = dict(self.workers); changed[key] += 1
            with self.subTest(key=key), self.assertRaises(ValueError): self.check_evidence(workers=changed)
        with self.assertRaises(ValueError): self.check_evidence(workers={"paused": True})

    def test_pcap_parser_rejects_truncation_unknown_link_and_trailing_bytes(self):
        bad_link = bytearray(self.capture); struct.pack_into("<I", bad_link, 20, 113)
        bad_original = bytearray(self.capture); struct.pack_into("<I", bad_original, 36, 97)
        for data in (b"", self.capture[:23], self.capture[:-1], self.capture + b"bad", bytes(bad_link), bytes(bad_original)):
            with self.subTest(length=len(data)), self.assertRaises(ValueError): HARDWARE.parse_pcap(data)

    def test_peer_virtual_or_management_interface_never_opens_packet_socket(self):
        observed_peer = {"interfaces": [{"interface": "data0", "pci_address": "pci0", "virtual": True}],
                         "management_interfaces": [], "management_pci": []}
        with self.assertRaises(RuntimeError): PEER.require_dedicated(observed_peer)
        observed_peer["interfaces"][0]["virtual"] = False
        observed_peer["management_pci"] = ["pci0"]
        with self.assertRaises(RuntimeError): PEER.require_dedicated(observed_peer)
        with patch.object(PEER, "inspect", return_value=observed_peer), patch.object(PEER.socket, "socket") as packet:
            with self.assertRaises(RuntimeError):
                PEER.main({"operation": "traffic", "run_id": "fixture", "tx_interface": "data0", "rx_interface": "data1"})
            packet.assert_not_called()

    @unittest.skipUnless(sys.platform == "linux", "进程组清理只在 Linux 实机入口使用")
    def test_timeout_preserves_raw_failure_log_and_reaps_process(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "timeout.json"
            with self.assertRaisesRegex(RuntimeError, "超时"):
                HARDWARE.execute([sys.executable, "-c", "import time; print('started',flush=True); time.sleep(10)"],
                                 log, timeout=0.1)
            record = json.loads(log.read_text())
            self.assertNotEqual(record["returncode"], 0)
            self.assertIn("started", record["stdout"])


class ManagementIdentityTests(unittest.TestCase):
    def test_no_route_or_ssh_evidence_cannot_prove_a_dedicated_interface(self):
        observation = {"interfaces": [{"interface": "data0", "pci_address": "0000:03:00.1",
                                        "pci_parent": "0000:03:00.0", "virtual": False}],
                       "management_interfaces": [], "management_pci": []}
        with self.assertRaises(RuntimeError): PEER.require_dedicated(observation)
        with self.assertRaises(RuntimeError): PEER.management_interfaces([[], []], [], "")
        with patch.object(PEER, "inspect", return_value=observation), patch.object(PEER.socket, "socket") as packet:
            with self.assertRaises(RuntimeError):
                PEER.main({"operation": "traffic", "run_id": "fixture", "tx_interface": "data0", "rx_interface": "data1"})
            packet.assert_not_called()

    def test_peer_missing_malformed_or_unmatched_ssh_stops_before_packet_socket(self):
        addresses = [{"ifname": "management0", "ifindex": 2,
                      "addr_info": [{"local": "192.0.2.10"}]}]
        outputs = [json.dumps([{"dst": "default", "dev": "management0"}]), "[]", json.dumps(addresses)]
        for ssh in ("", "malformed", "192.0.2.1 2222 192.0.2.99 22", "192.0.2.1 bad 192.0.2.10 22"):
            with self.subTest(ssh=ssh), patch.dict(PEER.os.environ, {"SSH_CONNECTION": ssh}), \
                    patch.object(PEER, "query", side_effect=outputs), patch.object(PEER, "interface_identity") as identity, \
                    patch.object(PEER.socket, "socket") as packet:
                with self.assertRaises((RuntimeError, ValueError)):
                    PEER.main({"operation": "traffic", "run_id": "fixture", "tx_interface": "data0", "rx_interface": "data1"})
                packet.assert_not_called(); identity.assert_not_called()

    def test_default_multi_table_multipath_and_ssh_local_address_are_all_protected(self):
        routes = [[{"dst": "default", "table": 100, "nexthops": [{"dev": "route0"}, {"dev": "route1", "flags": ["dead"]}]}],
                  [{"dst": "::/0", "table": 200, "dev": "v6route"}]]
        addresses = [{"ifname": "ssh0", "ifindex": 8, "addr_info": [{"local": "192.0.2.10"}]}]
        self.assertEqual(PEER.management_interfaces(routes, addresses, "192.0.2.1 2222 ::ffff:192.0.2.10 22"),
                         ["route0", "route1", "ssh0", "v6route"])
        scoped = [{"ifname": "ssh0", "ifindex": 8, "addr_info": [{"local": "fe80::10"}]}]
        self.assertEqual(PEER.management_interfaces([[], []], scoped, "fe80::1 2222 fe80::10%8 22"), ["ssh0"])
        with self.assertRaises(RuntimeError): PEER.management_interfaces([[], []], scoped, "fe80::1 2222 fe80::10%wrong 22")

    def test_unresolved_default_nexthop_cannot_be_treated_as_no_management_route(self):
        for route in ({"dst": "default", "nhid": 7}, {"dst": "::/0", "nexthops": [{"gateway": "fe80::1"}]}):
            with self.subTest(route=route), self.assertRaises(RuntimeError):
                PEER.management_interfaces([[route], []], [], "")

    def test_peer_vf_with_a_different_bdf_is_rejected_when_it_shares_management_parent(self):
        observation = {"interfaces": [{"interface": "data0", "pci_address": "0000:03:00.1",
                                        "pci_parent": "0000:03:00.0", "virtual": False}],
                       "management_interfaces": ["management0"], "management_pci": ["0000:03:00.0"]}
        with self.assertRaises(RuntimeError): PEER.require_dedicated(observation)
        observation["interfaces"][0]["pci_parent"] = "0000:04:00.0"
        PEER.require_dedicated(observation)
        observation["interfaces"][0].pop("pci_parent")
        with self.assertRaises(RuntimeError): PEER.require_dedicated(observation)

    def test_basic_dut_gate_protects_same_management_parent_and_rejects_unknown_parent(self):
        for reader in (lambda unused: "0000:02:00.0", lambda unused: None):
            with self.subTest(reader=reader), self.assertRaises(HARDWARE.Blocked):
                HARDWARE.hardware_gate(inventory(), configuration(), environ={}, parent_reader=reader)

    def test_dut_ssh_claim_and_observed_management_list_must_be_independently_valid(self):
        for ssh in ("malformed", "192.0.2.1 2222 192.0.2.99 22"):
            with self.subTest(ssh=ssh), self.assertRaises(HARDWARE.Blocked):
                HARDWARE.hardware_gate(inventory(), configuration(), environ={"SSH_CONNECTION": ssh}, parent_reader=lambda bdf: bdf)
        sample = inventory(); sample["management_detection"]["routes"][0] = observed([])
        with self.assertRaises(HARDWARE.Blocked): fixture_gate(sample, configuration())


class ProcessSafetyTests(unittest.TestCase):
    @unittest.skipUnless(sys.platform == "linux", "运行中 exe 和 SO_PEERCRED 只在 Linux 实机入口使用")
    def test_process_identity_hashes_running_executable_instead_of_trusting_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "dppd").write_bytes(b"different binary contents")
            path = directory / "control.sock"
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as server:
                server.bind(str(path)); server.listen(4)
                with self.assertRaises(HARDWARE.Blocked):
                    HARDWARE.process_identity(os.getpid(), directory, path)

    @unittest.skipUnless(sys.platform == "linux", "正式 SEQPACKET 与 SO_PEERCRED 协议只在 Linux 使用")
    def test_formal_seqpacket_socket_proves_peer_pid_and_running_binary(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary); path = directory / "control.sock"
            (directory / "dppd").symlink_to(Path(sys.executable).resolve())
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as server:
                server.bind(str(path)); server.listen(4)
                identity = HARDWARE.process_identity(os.getpid(), directory, path)
                self.assertEqual(identity["pid"], os.getpid())
                self.assertTrue(identity["start_time"].isdecimal())
                self.assertTrue(identity["cmdline"])

    @unittest.skipUnless(sys.platform == "linux", "错误 STREAM socket 的 EPROTOTYPE 只在 Linux 验证")
    def test_stream_socket_is_rejected_before_accepting_the_same_peer_pid(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary); path = directory / "wrong.sock"
            (directory / "dppd").symlink_to(Path(sys.executable).resolve())
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
                server.bind(str(path)); server.listen(4)
                with self.assertRaises(HARDWARE.Blocked) as failure:
                    HARDWARE.process_identity(os.getpid(), directory, path)
                self.assertEqual(failure.exception.__cause__.errno, errno.EPROTOTYPE)

    @unittest.skipUnless(sys.platform == "linux", "pidfd 实际信号只在 Linux 使用")
    def test_real_pidfd_stops_only_a_dedicated_child_and_observes_task_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary); path = directory / "control.sock"
            (directory / "dppd").symlink_to(Path(sys.executable).resolve())
            source = ("import socket,sys,time; s=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET); "
                      "s.bind(sys.argv[1]); s.listen(8); time.sleep(10)")
            child = subprocess.Popen([sys.executable, "-c", source, str(path)])
            descriptor = None
            try:
                deadline = time.monotonic() + 3
                while not path.exists():
                    if child.poll() is not None or time.monotonic() >= deadline:
                        self.fail("专用子进程没有创建测试 socket")
                    time.sleep(0.01)
                identity = HARDWARE.process_identity(child.pid, directory, path)
                descriptor = HARDWARE.pin_process(identity, directory, path)
                signal.pidfd_send_signal(descriptor, signal.SIGSTOP, None, 0)
                states = HARDWARE.wait_stopped(child.pid)
                self.assertIn(str(child.pid), states)
                signal.pidfd_send_signal(descriptor, signal.SIGCONT, None, 0)
            finally:
                if descriptor is not None:
                    try: signal.pidfd_send_signal(descriptor, signal.SIGCONT, None, 0)
                    except ProcessLookupError: pass
                    os.close(descriptor)
                child.terminate()
                try: child.wait(timeout=3)
                except subprocess.TimeoutExpired: child.kill(); child.wait()

    def test_pid_reuse_while_opening_pidfd_closes_descriptor_and_blocks(self):
        old = {"pid": 1234, "start_time": "10"}
        new = {"pid": 1234, "start_time": "11"}
        with patch.object(HARDWARE, "process_identity", side_effect=[old, new]), \
                patch.object(HARDWARE.os, "pidfd_open", return_value=99, create=True), \
                patch.object(HARDWARE.signal, "pidfd_send_signal", create=True), \
                patch.object(HARDWARE.os, "close") as close:
            with self.assertRaises(HARDWARE.Blocked): HARDWARE.pin_process(old, ROOT / "build", "/fixture.sock")
            close.assert_called_once_with(99)

    def test_no_pidfd_never_falls_back_to_bare_pid_signal(self):
        with patch.object(HARDWARE.os, "pidfd_open", None, create=True), \
                patch.object(HARDWARE.os, "kill") as kill:
            with self.assertRaises(HARDWARE.Blocked):
                HARDWARE.pin_process({"pid": 1234, "start_time": "10"}, ROOT / "build", "/fixture.sock")
            kill.assert_not_called()

    def run_fixture(self, peer_error=None, signal_error=None, typed_allow=None):
        config = configuration() if typed_allow is None else pci_configuration()
        config.update(daemon_pid=1234, socket=str(ROOT / "build/fixture.sock"))
        arguments = config["eal_devargs"] if typed_allow is None else ["dppd", typed_allow + config["eal_devargs"][0], "--"]
        identity = {"pid": 1234, "start_time": "10", "cmdline": arguments}
        observation = {"machine_id": "separate-fixture-peer", "management_interfaces": ["management0"],
                       "management_pci": ["management-pci"], "interfaces": [
                           {"interface": "peer0", "mac": "02:00:00:00:00:01"},
                           {"interface": "peer1", "mac": "02:00:00:00:00:02"}]}
        calls, count_queries, peer_calls = [], 0, 0
        report = {"run_id": "0" * 32, "status": "blocked", "capability_matrix": HARDWARE.matrix(), "limitations": []}

        def command(argv, *unused, **options):
            nonlocal count_queries
            del unused, options
            if argv[0] == "meson": return {"returncode": 0, "stdout": "", "stderr": ""}
            operation, arguments = argv[3], argv[4:]; calls.append((operation, arguments))
            if typed_allow is not None and operation in ("port-show", "capability-show"):
                return pci_identity_reader(config)(operation, int(arguments[0]))
            output = {"port-show": "driver=mlx5_pci device=0000:03:00.0 interface=test0 ifindex=2 role=uplink "
                                   f"switch-domain=3 switch-port={arguments[0] if arguments else 0}\n",
                      "capability-show": "device=0000:03:00.0 driver=mlx5_pci dpdk=21.11.9 firmware=test-firmware firmware-error=0\n",
                      "transfer-plan": "repository-generation=42\n",
                      "transfer-apply": "repository-generation=43\nedge=0 generation=7\n",
                      "rule-status": "backend=rte_flow\n", "stats": "rx_packets=0 rx_bytes=0 tx_packets=0 tx_bytes=0\n",
                      "health": "ready=yes\n", "delete": "deleted=yes generation=44\n"}.get(operation, "")
            if operation == "get": return {"returncode": 1, "stdout": "", "stderr": "No such file or directory (-2)"}
            if operation == "count":
                count_queries += 1
                output = "count rule=50000 generation=7 " + ("hits=0 bytes=0\n" if count_queries == 1 else "hits=16 bytes=1792\n")
            return {"returncode": 0, "stdout": output, "stderr": ""}

        def peer(configured, request, destination):
            nonlocal peer_calls
            del configured, destination
            peer_calls += 1
            result = {"run_id": report["run_id"], "simulation": False, "capture_origin": "live_AF_PACKET",
                      "inventory": observation}
            if peer_calls == 2:
                if peer_error: raise HARDWARE.Blocked(peer_error)
                capture = PEER.pcap([(1.0, bytes.fromhex(frame)) for frame in request["frames_hex"] for _ in range(8)])
                result.update(tx_pcap_base64=base64.b64encode(capture).decode(),
                              rx_pcap_base64=base64.b64encode(capture).decode())
            return result

        original_read = Path.read_bytes
        def read(path):
            return b"fixture-dut" if path.as_posix() == "/etc/machine-id" else original_read(path)

        with tempfile.TemporaryDirectory() as temporary, patch.object(HARDWARE, "execute", side_effect=command), \
                patch.object(HARDWARE, "build_binding", return_value={}), \
                patch.object(HARDWARE, "process_identity", return_value=identity), \
                patch.object(HARDWARE, "pin_process", return_value=99), \
                patch.object(HARDWARE, "wait_stopped", return_value={"1": "State:\tT (stopped)"}), \
                patch.object(HARDWARE, "all_tasks_stopped", return_value={"1": "State:\tT (stopped)"}), \
                patch.object(HARDWARE, "peer", side_effect=peer), patch.object(Path, "read_bytes", read), \
                patch.object(HARDWARE, "inspect_environment", return_value=pci_inventory(config) if typed_allow else inventory()), \
                patch.object(HARDWARE, "pci_parent", side_effect=lambda bdf: bdf), \
                patch.dict(HARDWARE.os.environ, {"SSH_CONNECTION": ""}), \
                patch.object(HARDWARE.socket, "if_nametoindex", return_value=2), \
                patch.object(HARDWARE.signal, "SIGSTOP", getattr(signal, "SIGSTOP", 19), create=True), \
                patch.object(HARDWARE.signal, "SIGCONT", getattr(signal, "SIGCONT", 18), create=True), \
                patch.object(HARDWARE.signal, "pidfd_send_signal", side_effect=signal_error, create=True) as send, \
                patch.object(HARDWARE.os, "kill") as bare, patch.object(HARDWARE.os, "close") as close:
            try:
                HARDWARE.live_run(ROOT, ROOT / "build", Path(temporary), config, report)
                error = None
            except (HARDWARE.Blocked, RuntimeError, OSError) as failure:
                error = failure
            bare.assert_not_called(); close.assert_called_once_with(99)
            return report, calls, send.call_args_list, error

    def test_basic_runner_uses_pidfd_and_exact_version_cleanup(self):
        report, calls, signals, error = self.run_fixture()
        self.assertIsNone(error)
        self.assertEqual(report["status"], "partial")
        self.assertIn(("delete", ["50000", "7"]), calls)
        self.assertEqual([item.args[1] for item in signals], [getattr(signal, "SIGSTOP", 19), getattr(signal, "SIGCONT", 18)])
        self.assertTrue(report["cleanup"]["daemon_resumed"])
        self.assertTrue(report["cleanup"]["rule_deleted"])

    def test_peer_failure_after_pause_still_resumes_deletes_and_checks_health(self):
        report, calls, signals, error = self.run_fixture(peer_error="fixture permission denied")
        self.assertIsInstance(error, HARDWARE.Blocked)
        self.assertEqual([item.args[1] for item in signals], [getattr(signal, "SIGSTOP", 19), getattr(signal, "SIGCONT", 18)])
        self.assertIn(("delete", ["50000", "7"]), calls)
        self.assertTrue(report["cleanup"]["health_ready"])

    def test_resume_failure_does_not_skip_delete_or_health(self):
        def failed_continue(descriptor, number, *unused):
            del descriptor, unused
            if number == signal.SIGCONT: raise ProcessLookupError("fixture exited")
        report, calls, _, error = self.run_fixture(signal_error=failed_continue)
        self.assertIsInstance(error, RuntimeError)
        self.assertFalse(report["cleanup"]["daemon_resumed"])
        self.assertTrue(report["cleanup"]["rule_deleted"])
        self.assertTrue(report["cleanup"]["health_ready"])
        self.assertTrue(any("resume:" in message for message in report["cleanup"]["errors"]))
        self.assertIn(("delete", ["50000", "7"]), calls)


class LifecycleFixture:
    """独立维护规则/对象状态和计数，不调用硬件，也不把夹具报告交给实机入口"""

    def __init__(self, failure=""):
        self.failure, self.now, self.last_hit = failure, 0.0, 0.0
        self.rules, self.shared, self.calls, self.sends = {}, None, [], []
        self.next_generation, self.hits, self.bytes = 10, 0, 0
        self.config = configuration()
        self.report = {"capability_matrix": HARDWARE.matrix()}

    def command(self, operation, *arguments, required=True):
        self.calls.append((operation, arguments)); code, output, error = 0, "", ""
        rid = int(arguments[0]) if arguments else 0
        if operation == "transfer-plan": output = "repository-generation=5\n"
        elif operation == "transfer-apply":
            if self.failure == "unsupported": code, error = 1, "Operation not supported (-95)"
            else:
                ids = [rid] if arguments[5] == "oneway" else [rid, rid + 1]
                for key in ids:
                    self.next_generation += 1
                    self.rules[key] = {"generation": str(self.next_generation),
                                       "age": "age:3" in arguments}
                self.last_hit = self.now
        elif operation == "get":
            rule = self.rules.get(rid)
            limit = 0.5 if self.failure == "early-age" else 3
            if rule and rule["age"] and self.failure != "never-age" and self.now - self.last_hit >= limit:
                del self.rules[rid]; rule = None
            if rule: output = f"rule id={rid} generation=" + rule["generation"] + " install-port=0 domain=transfer\n"
            else: code, error = 1, "No such file or directory (-2)"
        elif operation == "rule-status": output = "backend=rte_flow\n"
        elif operation == "rule-age": output = "aged=no sec-since-last-hit=0\n"
        elif operation in ("count", "shared-count-query"):
            if operation == "count" and rid not in self.rules: code, error = 1, "No such file or directory (-2)"
            else:
                key = "rule" if operation == "count" else "id"
                output = f"{key}={rid} generation={arguments[1]} hits={self.hits} bytes={self.bytes}\n"
        elif operation == "delete":
            if rid not in self.rules: code, error = 1, "No such file or directory (-2)"
            elif arguments[1] != self.rules[rid]["generation"]: code, error = 1, "Stale (-116)"
            else: del self.rules[rid]; output = "deleted=yes generation=99\n"
        elif operation == "shared-count-create":
            if self.failure == "shared-unsupported": code, error = 1, "Operation not supported (-95)"
            else: self.shared = "9"; output = "shared-count id=50000 generation=9 references=0\n"
        elif operation == "shared-count-get":
            if self.shared is None: code, error = 1, "No such file or directory (-2)"
            else: output = f"shared-count id=50000 generation=9 install=0 domain=transfer references={len(self.rules)}\n"
        elif operation == "shared-count-delete":
            if self.rules: code, error = 1, "Device busy (-16)"
            elif self.failure == "shared-cleanup": code, error = 1, "I/O error (-5)"
            else: self.shared = None; output = "shared-count=deleted id=50000 generation=9 revision=10\n"
        else: raise AssertionError(operation)
        result = {"returncode": code, "stdout": output, "stderr": error}
        if required and code: raise RuntimeError(error)
        return result

    def send(self, tag, reverse):
        self.sends.append((tag, reverse))
        if self.failure == "peer-failure": raise RuntimeError("fixture peer failure")
        frames = [b"\x01" * 96, b"\x02" * 128]
        self.hits += 2
        self.bytes += 225 if self.failure == "wrong-count" else 224
        self.last_hit = self.now
        capture = PEER.pcap([(1.0, frame) for frame in frames])
        workers = {"rx_packets": 0, "rx_bytes": 0, "tx_packets": 0, "tx_bytes": 0}
        return {"tx": capture, "rx": capture, "frames": frames, "packets": 1,
                "workers_before": workers, "workers_after": workers}

    def sleep(self, seconds): self.now += seconds

    def run_age(self):
        with patch.object(HARDWARE.time, "monotonic", side_effect=lambda: self.now), \
                patch.object(HARDWARE.time, "sleep", side_effect=self.sleep):
            HARDWARE.run_age_stage(self.config, self.report, self.command, self.send)


class LifecycleTests(unittest.TestCase):
    def test_age_requires_real_query_renewal_and_delayed_repository_deletion(self):
        fixture = LifecycleFixture(); fixture.run_age()
        self.assertFalse(fixture.rules)
        self.assertEqual(fixture.sends, [("age-first-hit", False), ("age-renew-hit", False)])
        self.assertGreaterEqual(fixture.now, 4)
        self.assertEqual(next(row for row in fixture.report["capability_matrix"] if row["stage"] == "age")["status"], "passed")

    def test_age_early_expiry_never_expiry_wrong_count_and_peer_failure_are_not_passed(self):
        for failure in ("early-age", "never-age", "wrong-count", "peer-failure"):
            fixture = LifecycleFixture(failure)
            with self.subTest(failure=failure), self.assertRaises((RuntimeError, ValueError)): fixture.run_age()
            self.assertFalse(fixture.rules)
            self.assertFalse(fixture.report["age_cleanup"]["rule_cleanup_errors"])
            self.assertNotEqual(next(row for row in fixture.report["capability_matrix"] if row["stage"] == "age")["status"], "passed")

    def test_age_explicit_driver_rejection_is_not_hardware_success(self):
        fixture = LifecycleFixture("unsupported"); fixture.run_age()
        self.assertFalse(fixture.sends)
        self.assertEqual(next(row for row in fixture.report["capability_matrix"] if row["stage"] == "age")["status"], "rejected")

    def test_shared_bidirectional_counter_and_partial_delete_preserve_total(self):
        fixture = LifecycleFixture()
        HARDWARE.run_shared_stage(fixture.config, fixture.report, fixture.command, fixture.send)
        self.assertEqual(fixture.sends, [("shared-forward", False), ("shared-reverse", True),
                                        ("shared-after-one-delete", False)])
        self.assertEqual((fixture.hits, fixture.bytes), (6, 672))
        self.assertFalse(fixture.rules); self.assertIsNone(fixture.shared)
        self.assertEqual(next(row for row in fixture.report["capability_matrix"] if row["stage"] == "shared_count")["status"], "passed")

    def test_shared_packet_failure_cleans_rules_before_object(self):
        fixture = LifecycleFixture("peer-failure")
        with self.assertRaises(RuntimeError):
            HARDWARE.run_shared_stage(fixture.config, fixture.report, fixture.command, fixture.send)
        self.assertFalse(fixture.rules); self.assertIsNone(fixture.shared)
        self.assertFalse(fixture.report["shared_cleanup"]["cleanup_errors"])

    def test_shared_object_cleanup_failure_is_reported(self):
        fixture = LifecycleFixture("shared-cleanup")
        with self.assertRaises(RuntimeError):
            HARDWARE.run_shared_stage(fixture.config, fixture.report, fixture.command, fixture.send)
        self.assertFalse(fixture.rules); self.assertEqual(fixture.shared, "9")
        self.assertTrue(fixture.report["shared_cleanup"]["cleanup_errors"])

    def test_shared_unsupported_does_not_send_or_claim_success(self):
        fixture = LifecycleFixture("shared-unsupported")
        HARDWARE.run_shared_stage(fixture.config, fixture.report, fixture.command, fixture.send)
        self.assertFalse(fixture.sends); self.assertFalse(fixture.rules); self.assertIsNone(fixture.shared)
        self.assertEqual(next(row for row in fixture.report["capability_matrix"] if row["stage"] == "shared_count")["status"], "rejected")


def reset_configuration(snapshot):
    config = configuration()
    config["stages"] = ["basic", "planned_reset"]
    config["planned_reset"] = {"allow_full_group_reset": True, "snapshot_path": str(snapshot), "modes": ["count"],
                               "device_ports": [copy.deepcopy(config["ports"]["source"]),
                                                copy.deepcopy(config["ports"]["destination"])]}
    return config


class ResetFixture(LifecycleFixture):
    """以独立状态模拟正式 CLI 的故障，只用于验证实机入口不会把命令成功当成实机证据"""

    def __init__(self, directory, failure="", modes=None):
        super().__init__(failure)
        self.directory = directory
        self.snapshot = directory / "rules.snapshot"
        self.snapshot.write_bytes(b"fixture snapshot initial")
        self.config = reset_configuration(self.snapshot)
        self.config["planned_reset"]["modes"] = modes or ["count"]
        self.report = {"capability_matrix": HARDWARE.matrix()}
        self.attempts, self.resets, self.recoveries = 0, 0, 0
        self.isolated = False
        self.revision = 0

    def status(self):
        mask = 3 if self.resets else 0
        return {"state": "isolated" if self.isolated else "running", "sequence": 0, "approved-sequence": 0,
                "planned": "yes" if self.isolated else "no", "quarantined": "yes" if self.isolated else "no",
                "workers-stopped": "yes" if self.isolated else "no", "attempts": self.attempts,
                "resets": self.resets, "recoveries": self.recoveries, "phase": 6 if self.resets else 0,
                "last-error": -5 if self.isolated else 0, "cleanup-error": 0,
                "desired-rules": len(self.rules), "actual-rules": 0 if self.isolated else len(self.rules),
                "desired-shared": int(self.shared is not None), "actual-shared": int(self.shared is not None),
                "stopped": mask, "reset": mask, "configured": mask, "started": mask,
                "reset-events": 0, "removal-events": 0, "destroy-events": 0, "device_ports": [0, 1]}

    def prepare(self, tag):
        artifact = self.directory / tag
        artifact.mkdir()
        state = HARDWARE.read_device_status(self.command)
        if self.failure == "unrelated-rules": state["desired-rules"] += 1
        if self.failure == "incomplete-group": state["device_ports"] = [0, 1, 9]
        return state, {"fixture": "topology"}, self.snapshot, artifact

    def command(self, operation, *arguments, required=True, timeout=8):
        del timeout
        if operation == "device-status":
            state = self.status()
            groups = state.pop("device_ports")
            for key in ("stopped", "reset", "configured", "started", "reset-events", "removal-events", "destroy-events"):
                state[key] = hex(state[key])
            output = "device " + " ".join(f"{key}={item}" for key, item in state.items())
            output += "\ndevice-ports count=2 ids=" + ",".join(str(item) for item in groups) + "\n"
            return {"returncode": 0, "stdout": output, "stderr": ""}
        if operation == "device-reset":
            self.calls.append((operation, arguments))
            if arguments != (0,): raise AssertionError("必须使用当前精确 sequence，零也是有效值")
            if self.failure == "reset-unsupported":
                return {"returncode": 1, "stdout": "", "stderr": "Operation not supported (-95)"}
            if self.failure == "reset-timeout": raise RuntimeError("fixture bounded reset timeout")
            self.attempts += 1
            if self.failure == "partial-reset":
                self.isolated = True
                return {"returncode": 1, "stdout": "", "stderr": "Operation not supported (-95)"}
            if self.failure != "cli-only-success":
                self.resets += 1
                self.recoveries += 1
            self.hits, self.bytes, self.last_hit = 0, 0, self.now
            if self.failure == "counter-not-reset": self.hits = 1
            if self.failure == "changed-version": self.rules[50000]["generation"] = "999"
            if self.failure == "changed-snapshot": self.snapshot.write_bytes(b"unexpected rewrite")
            if self.failure == "changed-shared-revision": self.revision += 1
            return {"returncode": 0, "stdout": "device state=running\n", "stderr": ""}
        if operation in ("list", "shared-count-list", "persistence-status", "health"):
            output = {"list": f"repository-generation={self.next_generation} total={len(self.rules)}\n",
                      "shared-count-list": f"shared-counts revision={self.revision} total={int(self.shared is not None)}\n",
                      "persistence-status": f"persistence enabled=yes dirty=no current-generation={self.next_generation}\n",
                      "health": "ready=yes\n"}[operation]
            return {"returncode": 0, "stdout": output, "stderr": ""}
        result = super().command(operation, *arguments, required=required)
        if operation == "transfer-apply" and not result["returncode"]:
            self.rules[int(arguments[0])]["age"] = "age:6" in arguments
        if operation in ("shared-count-create", "shared-count-delete") and not result["returncode"]:
            self.revision += 1
        return result

    def command_with_age(self, operation, *arguments, **options):
        if operation == "rule-age":
            idle = 2 if self.failure == "age-not-restarted" and self.resets else int(self.now - self.last_hit)
            return {"returncode": 0, "stdout": f"rule-age rule={arguments[0]} generation={arguments[1]} aged=no idle-seconds={idle}\n", "stderr": ""}
        if operation == "get":
            rule = self.rules.get(int(arguments[0]))
            if rule and rule["age"] and self.now - self.last_hit < 6:
                return {"returncode": 0, "stdout": f"rule id={arguments[0]} generation=" + rule["generation"] +
                        " install-port=0 domain=transfer\n", "stderr": ""}
        result = self.command(operation, *arguments, **options)
        if operation == "shared-count-get" and self.resets >= 2 and self.failure == "shared-reference-after":
            result["stdout"] = result["stdout"].replace("references=2", "references=1")
        return result

    def send(self, tag, reverse):
        if self.resets and self.failure == "after-reset-peer":
            raise RuntimeError("fixture external peer failed after physical reset")
        batch = super().send(tag, reverse)
        if self.resets and self.failure == "after-reset-cpu":
            batch["workers_after"] = dict(batch["workers_after"], rx_packets=1)
        if self.resets and self.failure == "after-reset-bytes":
            batch["rx"] = PEER.pcap([(1.0, b"wrong" * 20)])
        return batch

    def run(self):
        with patch.object(HARDWARE.time, "monotonic", side_effect=lambda: self.now), \
                patch.object(HARDWARE.time, "sleep", side_effect=self.sleep), \
                patch.object(HARDWARE, "runtime_port_identities", return_value={"fixture": "topology"}):
            HARDWARE.run_reset_stage(self.config, self.report, self.command_with_age, self.send, self.prepare)


class PlannedResetTests(unittest.TestCase):
    def test_reset_requires_explicit_full_group_modes_snapshot_and_consent(self):
        for failure in ("missing-consent", "duplicate", "missing-port", "virtual-extra", "bad-mode", "bad-snapshot"):
            config = reset_configuration(ROOT / "build/fixture.snapshot")
            reset = config["planned_reset"]
            if failure == "missing-consent": reset["allow_full_group_reset"] = False
            if failure == "duplicate": reset["device_ports"].append(reset["device_ports"][0])
            if failure == "missing-port": reset["device_ports"].pop()
            if failure == "virtual-extra":
                extra = copy.deepcopy(reset["device_ports"][0]); extra.update(port_id=8, pmd="net_ring")
                reset["device_ports"].append(extra)
            if failure == "bad-mode": reset["modes"] = ["age"]
            if failure == "bad-snapshot": reset["snapshot_path"] = "relative.snapshot"
            with self.subTest(failure=failure), self.assertRaises(HARDWARE.Blocked): HARDWARE.validate_config(config)

    def test_extra_daemon_port_also_has_to_pass_pci_and_management_gate(self):
        config = reset_configuration(ROOT / "build/fixture.snapshot")
        extra = copy.deepcopy(config["planned_reset"]["device_ports"][0])
        extra.update(port_id=8, pci_address="0000:02:00.0", device_name="0000:02:00.0", interface="management0")
        config["planned_reset"]["device_ports"].append(extra)
        config["eal_devargs"].append("0000:02:00.0")
        sample = inventory()
        sample["pci_scan"]["network_devices"].append({"pci_address": "0000:02:00.0",
                                                      "classification": {"kind": "physical_candidate"}})
        with self.assertRaises(HARDWARE.Blocked): fixture_gate(sample, config)

    def test_snapshot_is_bound_to_actual_unique_application_argument(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "snapshot"; path.write_bytes(b"fixture")
            config = reset_configuration(path)
            for arguments in (["dppd", "--state-path", str(path)], ["dppd", "--", "--state-path", "other"],
                              ["dppd", "--", "--ports", "0,1", "--state-path", str(path), "--state-path=" + str(path)],
                              ["dppd", "--", "--ports", "0,1,9", "--state-path", str(path)],
                              ["dppd", "--", "--ports", "0,1", "--queues", "0", "--state-path", str(path)],
                              ["dppd", "--", "--ports", "0,1", "--nd6", "0,::1", "--state-path", str(path)]):
                with self.subTest(arguments=arguments), self.assertRaises(HARDWARE.Blocked):
                    HARDWARE.reset_snapshot(config, {"cmdline": arguments})
            identity = {"cmdline": ["dppd", "--", "--ports", "0,1", "--state-path=" + str(path)]}
            self.assertEqual(HARDWARE.reset_snapshot(config, identity), path)

    def test_default_sequence_zero_count_reset_requires_external_evidence_and_preserves_versions(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = ResetFixture(Path(temporary)); fixture.run()
            self.assertEqual([call for call in fixture.calls if call[0] == "device-reset"], [("device-reset", (0,))])
            self.assertEqual(fixture.sends, [("reset-count-before", False), ("reset-count-after", False)])
            self.assertFalse(fixture.rules); self.assertFalse(fixture.report["planned_reset"]["preserve_scene"])
            attempt = fixture.report["planned_reset"]["attempts"][0]
            self.assertEqual(attempt["status"], "passed"); self.assertEqual(attempt["packet_evidence"]["packets"], 2)
            self.assertEqual(next(row for row in fixture.report["capability_matrix"] if row["stage"] == HARDWARE.RESET_STAGE)["status"], "passed")

    def test_unrelated_rules_or_missing_group_prevent_any_create_reset_or_traffic(self):
        for failure in ("unrelated-rules", "incomplete-group"):
            with tempfile.TemporaryDirectory() as temporary, self.subTest(failure=failure):
                fixture = ResetFixture(Path(temporary), failure)
                with self.assertRaises(HARDWARE.Blocked): fixture.run()
                self.assertFalse(fixture.calls); self.assertFalse(fixture.sends)

    def test_cli_success_wrong_count_versions_snapshot_or_revision_cannot_pass_and_keep_scene(self):
        for failure in ("cli-only-success", "counter-not-reset", "changed-version", "changed-snapshot",
                        "reset-timeout", "partial-reset", "changed-shared-revision", "after-reset-peer",
                        "after-reset-cpu", "after-reset-bytes"):
            with tempfile.TemporaryDirectory() as temporary, self.subTest(failure=failure):
                fixture = ResetFixture(Path(temporary), failure)
                with self.assertRaises((HARDWARE.Blocked, RuntimeError, ValueError)): fixture.run()
                self.assertTrue(fixture.report["planned_reset"]["preserve_scene"])
                self.assertFalse(any(call[0] == "delete" for call in fixture.calls))
                self.assertFalse(any(call[0] == "acknowledge-clean" for call in fixture.calls))
                row = next(row for row in fixture.report["capability_matrix"] if row["stage"] == HARDWARE.RESET_STAGE)
                self.assertNotEqual(row["status"], "passed")

    def test_explicit_unchanged_unsupported_is_rejected_and_known_rules_can_be_cleaned(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = ResetFixture(Path(temporary), "reset-unsupported"); fixture.run()
            self.assertFalse(fixture.report["planned_reset"]["preserve_scene"])
            self.assertFalse(fixture.rules)
            self.assertEqual(next(row for row in fixture.report["capability_matrix"] if row["stage"] == HARDWARE.RESET_STAGE)["status"], "rejected")

    def test_shared_and_age_reset_have_separate_counter_and_lifecycle_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = ResetFixture(Path(temporary), modes=["count", "shared_count", "age"]); fixture.run()
            self.assertEqual([attempt["mode"] for attempt in fixture.report["planned_reset"]["attempts"]],
                             ["count", "shared_count", "age"])
            self.assertTrue(all(attempt["status"] == "passed" for attempt in fixture.report["planned_reset"]["attempts"]))
            self.assertIn(("reset-shared_count-after-reverse", True), fixture.sends)
            self.assertFalse(fixture.rules); self.assertIsNone(fixture.shared)

    def test_age_old_idle_time_after_reset_is_failure_without_old_handle_cleanup(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = ResetFixture(Path(temporary), "age-not-restarted", ["count", "age"])
            with self.assertRaises(RuntimeError): fixture.run()
            self.assertTrue(fixture.report["planned_reset"]["preserve_scene"])
            reset_index = max(index for index, call in enumerate(fixture.calls) if call[0] == "device-reset")
            self.assertFalse(any(call[0] == "delete" for call in fixture.calls[reset_index + 1:]))

    def test_shared_reference_mismatch_after_reset_preserves_objects_without_destroy(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = ResetFixture(Path(temporary), "shared-reference-after", ["count", "shared_count"])
            with self.assertRaises(RuntimeError): fixture.run()
            self.assertTrue(fixture.report["planned_reset"]["preserve_scene"])
            reset_index = max(index for index, call in enumerate(fixture.calls) if call[0] == "device-reset")
            self.assertFalse(any(call[0] in ("delete", "shared-count-delete") for call in fixture.calls[reset_index + 1:]))

    def test_missing_or_duplicated_protocol_group_never_allows_reset(self):
        for output in ("device state=running\n", "device-ports count=2 ids=0,0\n",
                       "device-ports count=3 ids=0,1\n", "device-ports count=2 ids=0,1\ndevice-ports count=2 ids=0,1\n"):
            with self.subTest(output=output), self.assertRaises(HARDWARE.Blocked):
                HARDWARE.read_device_status(lambda *unused: {"stdout": output})

    def test_count_response_from_another_generation_is_never_accepted(self):
        def command(*unused): return {"stdout": "count rule=50000 generation=99 hits=0 bytes=0\n"}
        with self.assertRaises(RuntimeError): HARDWARE._count(command, "count", 50000, "7")

    def test_shared_action_generation_cannot_overwrite_its_rule_generation(self):
        output = "rule id=50000 generation=7 install-port=0\n  action[0] shared-count id=6 generation=99\n"
        self.assertEqual(HARDWARE.rule_identity(output, 50000)["generation"], "7")
        with self.assertRaises(RuntimeError): HARDWARE.rule_identity(output, 50001)

    def test_fresh_sysfs_parent_gate_protects_management_pf_against_a_test_vf(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "snapshot"; path.write_bytes(b"fixture")
            config = reset_configuration(path)
            with patch.object(HARDWARE, "inspect_environment", return_value=inventory()), \
                    patch.object(HARDWARE, "pci_parent", return_value="0000:02:00.0"), \
                    patch.object(HARDWARE, "runtime_port_identities") as runtime, \
                    patch.dict(HARDWARE.os.environ, {"SSH_CONNECTION": ""}):
                with self.assertRaises(HARDWARE.Blocked):
                    HARDWARE.reset_preflight(config, {}, None, {}, Path(temporary))
                runtime.assert_not_called()

    @unittest.skipUnless(sys.platform == "linux", "真实 sysfs 风格链接只在 Linux 夹具中创建")
    def test_pci_parent_follows_physfn_and_rejects_missing_nonnetwork_and_dangling_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            pf, vf = root / "0000:03:00.0", root / "0000:03:00.1"
            pf.mkdir(); vf.mkdir()
            (pf / "class").write_text("0x020000\n"); (vf / "class").write_text("0x020000\n")
            (vf / "physfn").symlink_to(pf)
            self.assertEqual(HARDWARE.pci_parent(vf.name, root), pf.name)
            (pf / "class").write_text("0x060000\n")
            with self.assertRaises(HARDWARE.Blocked): HARDWARE.pci_parent(vf.name, root)
            (pf / "class").unlink()
            with self.assertRaises(HARDWARE.Blocked): HARDWARE.pci_parent(vf.name, root)
            (vf / "physfn").unlink(); (vf / "physfn").symlink_to(root / "0000:04:00.0")
            with self.assertRaises(HARDWARE.Blocked): HARDWARE.pci_parent(vf.name, root)


if __name__ == "__main__":
    unittest.main()
