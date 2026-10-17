#!/usr/bin/env python3
"""用离线证据验证盘点结论，测试通过不表示 M3 硬件验收通过"""

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "inspect_m3_environment.py"
SPEC = importlib.util.spec_from_file_location("m3_inventory", SCRIPT)
INVENTORY = importlib.util.module_from_spec(SPEC)
# 测试只读取脚本，避免在源码目录留下字节码文件
WRITE_BYTECODE = sys.dont_write_bytecode
try:
    sys.dont_write_bytecode = True
    SPEC.loader.exec_module(INVENTORY)
finally:
    sys.dont_write_bytecode = WRITE_BYTECODE


def observed(text):
    return {"status": "observed", "value": text}


def device(vendor, product, subsystem="0x0000", driver=""):
    return {"vendor": observed(vendor), "device": observed(product),
            "subsystem_vendor": observed(subsystem), "driver": observed(driver)}


class FakeFiles:
    """所有证据都来自内存，单测不会读取真实网卡或改变网络"""

    def __init__(self):
        self.text = {}
        self.directories = {}
        self.links = {}
        self.nodes = {}

    def read(self, name):
        return self.text.get(name, {"status": "missing"})

    def entries(self, name):
        return self.directories.get(name, {"status": "missing"})

    def link(self, name):
        return self.links.get(name, {"status": "missing"})

    def node(self, name):
        return self.nodes.get(name, {"status": "missing"})


class FakeCommands:
    def __init__(self, outputs=None):
        self.outputs = outputs or {}
        self.calls = []

    def __call__(self, command):
        self.calls.append(command)
        return self.outputs.get(tuple(command), {"status": "missing"})


class InventoryTests(unittest.TestCase):
    def test_vmware_emulated_intel_is_not_physical_intel(self):
        classification = INVENTORY.classify_device(device("0x8086", "0x100f", "0x15ad", "e1000"))
        self.assertEqual(classification["kind"], "virtual")
        self.assertEqual(classification["role"], "not_candidate")
        self.assertEqual(classification["hardware_acceptance"], "not_run")

    def test_vmxnet3_and_virtio_are_virtual(self):
        for identity in (device("0x15ad", "0x07b0", driver="vmxnet3"), device("0x1af4", "0x1000")):
            with self.subTest(identity=identity):
                self.assertEqual(INVENTORY.classify_device(identity)["kind"], "virtual")

    def test_mellanox_is_only_a_candidate(self):
        classification = INVENTORY.classify_device(device("0x15b3", "0x1019", driver="mlx5_core"))
        self.assertEqual(classification["role"], "mlx5_candidate")
        self.assertEqual(classification["hardware_acceptance"], "not_run")

    def test_non_mlx5_is_only_a_candidate(self):
        classification = INVENTORY.classify_device(device("0x8086", "0x1572", driver="i40e"))
        self.assertEqual(classification["role"], "non_mlx5_candidate")
        self.assertEqual(classification["hardware_acceptance"], "not_run")

    def test_unknown_identity_is_not_classified_as_hardware(self):
        identity = device("0x15b3", "0x1019")
        identity["vendor"] = {"status": "permission_denied"}
        self.assertEqual(INVENTORY.classify_device(identity)["kind"], "unknown")
        identity = device("0x8086", "0x100f")
        identity["subsystem_vendor"] = {"status": "permission_denied"}
        self.assertEqual(INVENTORY.classify_device(identity)["kind"], "unknown")

    def test_empty_and_missing_inventory_are_not_acceptance(self):
        commands = FakeCommands()
        report = INVENTORY.inspect_environment(FakeFiles(), commands, {}, 2)
        self.assertEqual(report["hardware_acceptance"]["status"], "not_run")
        self.assertEqual(report["pci_scan"]["status"], "missing")
        self.assertEqual(report["tun"]["status"], "missing")
        self.assertEqual(report["dpdk_version"]["status"], "missing")
        self.assertEqual(report["cpu_count"], 2)
        allowed = {("ip", "-j", "route", "show", "table", "all"),
                   ("ip", "-j", "-6", "route", "show", "table", "all"),
                   ("ip", "-j", "address", "show"), ("systemd-detect-virt",),
                   ("pkg-config", "--modversion", "libdpdk"),
                   ("devlink", "dev", "show"), ("devlink", "port", "show")}
        self.assertEqual({tuple(call) for call in commands.calls}, allowed)

    def test_installed_mlx5_library_does_not_create_device(self):
        files = FakeFiles()
        files.directories["/usr/lib/x86_64-linux-gnu"] = observed(["librte_net_mlx5.so", "libc.so.6"])
        files.directories["/sys/bus/pci/devices"] = observed([])
        report = INVENTORY.inspect_environment(files, FakeCommands(), {}, 1)
        self.assertFalse(report["mlx5_library"]["proves_device"])
        self.assertEqual(report["mlx5_library"]["observation"]["value"], ["librte_net_mlx5.so"])
        self.assertEqual(report["pci_scan"]["network_devices"], [])

    def test_denied_and_unclassified_evidence_are_preserved(self):
        files = FakeFiles()
        files.directories["/sys/bus/pci/devices"] = observed(["0000:01:00.0"])
        files.text["/sys/bus/pci/devices/0000:01:00.0/class"] = {"status": "permission_denied"}
        files.nodes["/dev/net/tun"] = {"status": "permission_denied"}
        commands = FakeCommands({("devlink", "dev", "show"): {"status": "permission_denied"}})
        report = INVENTORY.inspect_environment(files, commands, {}, 1)
        self.assertEqual(report["tun"]["status"], "permission_denied")
        self.assertEqual(report["devlink_devices"]["status"], "permission_denied")
        self.assertEqual(report["pci_scan"]["unclassified_devices"][0]["class"]["status"], "permission_denied")

    def test_management_default_route_and_ssh_address_are_protected(self):
        files = FakeFiles()
        files.directories["/sys/class/net"] = observed(["ens33", "ens192", "ens34"])
        commands = FakeCommands({
            ("ip", "-j", "route", "show", "table", "all"): observed(json.dumps([{"dst": "default", "dev": "ens33"}])),
            ("ip", "-j", "-6", "route", "show", "table", "all"): observed("[]"),
            ("ip", "-j", "address", "show"): observed(json.dumps([
                {"ifname": "ens192", "addr_info": [{"local": "192.168.100.1"}]}])),
        })
        report = INVENTORY.inspect_environment(files, commands, {"SSH_CONNECTION": "192.168.100.2 1234 192.168.100.1 22"}, 1)
        interfaces = {item["name"]: item for item in report["interface_scan"]["interfaces"]}
        self.assertTrue(interfaces["ens33"]["management_interface"])
        self.assertTrue(interfaces["ens192"]["management_interface"])
        self.assertFalse(interfaces["ens34"]["management_interface"])
        self.assertTrue(all(item["binding_policy"] == "never_rebind_automatically" for item in interfaces.values()))

    def test_unavailable_routes_leave_management_unknown(self):
        files = FakeFiles()
        files.directories["/sys/class/net"] = observed(["ens33"])
        report = INVENTORY.inspect_environment(files, FakeCommands(), {}, 1)
        self.assertIsNone(report["interface_scan"]["interfaces"][0]["management_interface"])

    def test_partial_or_multipath_routes_do_not_expose_management_port(self):
        names, known = INVENTORY.management_interfaces([
            observed([{"dst": "default", "nexthops": [{"dev": "ens33"}, {"dev": "ens34"}]}]),
            {"status": "permission_denied"}], {"status": "missing"}, "")
        self.assertEqual(names, {"ens33", "ens34"})
        self.assertFalse(known)

    def test_invalid_command_json_is_not_empty_evidence(self):
        for output in ("not-json", "{}", "null"):
            with self.subTest(output=output):
                commands = FakeCommands({("ip",): observed(output)})
                self.assertEqual(INVENTORY.read_json_command(commands, ["ip"])["status"], "invalid_output")

    def test_root_override_and_permission_denied(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "proc" / "meminfo"
            path.parent.mkdir()
            path.write_text("HugePages_Total: 0\n", encoding="utf-8")
            files = INVENTORY.Files(root)
            self.assertEqual(files.read("/proc/meminfo")["value"], "HugePages_Total: 0")
            self.assertEqual(files.read("/proc/missing")["status"], "missing")
            with mock.patch.object(Path, "read_text", side_effect=PermissionError("denied")):
                self.assertEqual(files.read("/proc/meminfo")["status"], "permission_denied")

    def test_command_failure_does_not_become_success(self):
        with mock.patch.object(subprocess, "run", return_value=subprocess.CompletedProcess([], 1, "", "Operation not permitted")):
            self.assertEqual(INVENTORY.run_command(["devlink", "dev", "show"])["status"], "permission_denied")
        with mock.patch.object(subprocess, "run", side_effect=FileNotFoundError()):
            self.assertEqual(INVENTORY.run_command(["devlink", "dev", "show"])["status"], "missing")
        with mock.patch.object(subprocess, "run", side_effect=subprocess.TimeoutExpired([], 5)):
            self.assertEqual(INVENTORY.run_command(["devlink", "dev", "show"])["status"], "timeout")

    def test_sysfs_network_scan_preserves_hardware_evidence(self):
        files = FakeFiles()
        files.directories["/sys/bus/pci/devices"] = observed(["0000:01:00.0", "0000:00:00.0"])
        files.text["/sys/bus/pci/devices/0000:00:00.0/class"] = observed("0x060000")
        base = "/sys/bus/pci/devices/0000:01:00.0"
        for name, content in (("class", "0x020000"), ("vendor", "0x15b3"), ("device", "0x1019"),
                              ("subsystem_vendor", "0x15b3"), ("subsystem_device", "0x0001"),
                              ("sriov_totalvfs", "16"), ("sriov_numvfs", "0")):
            files.text[base + "/" + name] = observed(content)
        files.links[base + "/driver"] = observed("mlx5_core")
        files.links[base + "/iommu_group"] = observed("12")
        files.text["/proc/meminfo"] = observed("MemTotal: 1000 kB\nHugePages_Total: 2\nHugepagesize: 2048 kB")
        report = INVENTORY.inspect_environment(files, FakeCommands(), {}, 1)
        self.assertEqual(len(report["pci_scan"]["network_devices"]), 1)
        nic = report["pci_scan"]["network_devices"][0]
        self.assertEqual(nic["sriov_totalvfs"]["value"], "16")
        self.assertEqual(nic["iommu_group"]["value"], "12")
        self.assertEqual(nic["classification"]["role"], "mlx5_candidate")
        self.assertEqual(report["hugepages"]["value"]["HugePages_Total"], "2")


if __name__ == "__main__":
    unittest.main()
