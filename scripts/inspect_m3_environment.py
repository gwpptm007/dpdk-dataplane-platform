#!/usr/bin/env python3
"""只读盘点 Linux 上可见的 M3 条件，设备候选项不等于硬件验收通过"""

import argparse
import errno
import json
import os
from pathlib import Path
import platform
import stat
import subprocess


def unavailable(exc):
    """把没有文件和没有权限分开，避免把看不到误认为不存在"""
    if isinstance(exc, PermissionError) or getattr(exc, "errno", None) in (errno.EACCES, errno.EPERM):
        return {"status": "permission_denied"}
    if isinstance(exc, FileNotFoundError):
        return {"status": "missing"}
    return {"status": "error", "detail": str(exc)}


class Files:
    """根目录可替换，测试时读取临时目录，不碰宿主机的 sysfs"""

    def __init__(self, root="/"):
        self.root = Path(root)

    def path(self, name):
        return self.root / name.lstrip("/")

    def read(self, name):
        try:
            return {"status": "observed", "value": self.path(name).read_text(encoding="utf-8").strip()}
        except OSError as exc:
            return unavailable(exc)

    def entries(self, name):
        try:
            return {"status": "observed", "value": sorted(item.name for item in self.path(name).iterdir())}
        except OSError as exc:
            return unavailable(exc)

    def link(self, name):
        try:
            return {"status": "observed", "value": Path(os.readlink(self.path(name))).name}
        except OSError as exc:
            return unavailable(exc)

    def node(self, name):
        try:
            info = self.path(name).stat()
            return {
                "status": "observed",
                "character_device": stat.S_ISCHR(info.st_mode),
                "readable": os.access(self.path(name), os.R_OK),
                "writable": os.access(self.path(name), os.W_OK),
            }
        except OSError as exc:
            return unavailable(exc)


def run_command(argv):
    """只运行固定的查询命令，不使用 shell，也不提权"""
    try:
        result = subprocess.run(argv, capture_output=True, text=True, timeout=5, check=False)
    except subprocess.TimeoutExpired:
        return {"status": "timeout"}
    except OSError as exc:
        return unavailable(exc)
    if result.returncode == 0:
        return {"status": "observed", "value": result.stdout.strip()}
    detail = result.stderr.strip() or result.stdout.strip()
    denied = "permission denied" in detail.lower() or "operation not permitted" in detail.lower()
    return {"status": "permission_denied" if denied else "command_failed", "returncode": result.returncode, "detail": detail}


def value(observation, default=""):
    return observation.get("value", default) if observation.get("status") == "observed" else default


def hex_id(observation):
    try:
        return int(value(observation), 16)
    except (TypeError, ValueError):
        return None


def classify_device(device):
    """识别明确的虚拟网卡，其余设备也只登记候选，不推断卸载可用"""
    vendor = hex_id(device["vendor"])
    product = hex_id(device["device"])
    subsystem = hex_id(device["subsystem_vendor"])
    driver = value(device["driver"])
    if vendor in (0x15AD, 0x1AF4, 0x1B36) or driver in ("vmxnet3", "virtio-pci", "virtio_net"):
        kind, role, reason = "virtual", "not_candidate", "virtual_vendor_or_driver"
    elif vendor == 0x8086 and product == 0x100F and subsystem == 0x15AD:
        kind, role, reason = "virtual", "not_candidate", "vmware_emulated_82545em"
    elif vendor == 0x8086 and product == 0x100F and subsystem is None:
        kind, role, reason = "unknown", "not_candidate", "82545em_subsystem_not_observed"
    elif vendor is None or product is None:
        kind, role, reason = "unknown", "not_candidate", "identity_not_observed"
    elif vendor == 0x15B3:
        kind, role, reason = "physical_candidate", "mlx5_candidate", "mellanox_pci_identity_observed"
    else:
        kind, role, reason = "physical_candidate", "non_mlx5_candidate", "pci_identity_requires_hardware_confirmation"
    return {"kind": kind, "role": role, "reason": reason, "hardware_acceptance": "not_run"}


def read_json_command(run, argv):
    result = run(argv)
    if result["status"] != "observed":
        return result
    try:
        decoded = json.loads(result["value"])
    except (KeyError, TypeError, ValueError):
        return {"status": "invalid_output"}
    if not isinstance(decoded, list):
        return {"status": "invalid_output"}
    return {"status": "observed", "value": decoded}


def management_interfaces(routes, addresses, ssh_connection):
    """默认路由和当前 SSH 的本地地址都视为管理链路，不建议重绑"""
    names = set()
    known = all(result["status"] == "observed" for result in routes)
    for result in routes:
        if result["status"] == "observed":
            for route in result["value"]:
                if not isinstance(route, dict) or route.get("dst", "default") != "default":
                    continue
                if route.get("dev"):
                    names.add(route["dev"])
                for nexthop in route.get("nexthops", []):
                    if isinstance(nexthop, dict) and nexthop.get("dev"):
                        names.add(nexthop["dev"])
    parts = ssh_connection.split()
    if len(parts) == 4 and addresses["status"] == "observed":
        for address in addresses["value"]:
            if not isinstance(address, dict):
                continue
            for item in address.get("addr_info", []):
                if isinstance(item, dict) and item.get("local") == parts[2] and address.get("ifname"):
                    names.add(address["ifname"])
    elif len(parts) == 4:
        known = False
    return names, known


def inspect_environment(files=None, run=run_command, environ=None, cpu_count=None):
    """返回观察证据，缺失或权限不足不会被改写为硬件不支持"""
    files = files or Files()
    environ = os.environ if environ is None else environ
    routes = [read_json_command(run, ["ip", "-j", "route", "show", "table", "all"]),
              read_json_command(run, ["ip", "-j", "-6", "route", "show", "table", "all"])]
    addresses = read_json_command(run, ["ip", "-j", "address", "show"])
    management, management_known = management_interfaces(routes, addresses, environ.get("SSH_CONNECTION", ""))
    pci_entries = files.entries("/sys/bus/pci/devices")
    pci_devices = []
    unclassified = []
    for name in value(pci_entries, []):
        base = "/sys/bus/pci/devices/" + name
        class_code = files.read(base + "/class")
        number = hex_id(class_code)
        if number is None:
            unclassified.append({"pci_address": name, "class": class_code})
            continue
        if number >> 16 != 0x02:
            continue
        device = {"pci_address": name, "class": class_code}
        for field in ("vendor", "device", "subsystem_vendor", "subsystem_device", "sriov_totalvfs", "sriov_numvfs"):
            device[field] = files.read(base + "/" + field)
        device["driver"] = files.link(base + "/driver")
        device["iommu_group"] = files.link(base + "/iommu_group")
        device["classification"] = classify_device(device)
        pci_devices.append(device)
    interface_entries = files.entries("/sys/class/net")
    interfaces = []
    for name in value(interface_entries, []):
        base = "/sys/class/net/" + name
        interface = {"name": name, "pci_device": files.link(base + "/device")}
        for field in ("operstate", "phys_switch_id", "phys_port_name"):
            interface[field] = files.read(base + "/" + field)
        interface["management_interface"] = True if name in management else (False if management_known else None)
        interface["binding_policy"] = "never_rebind_automatically"
        interfaces.append(interface)
    memory = files.read("/proc/meminfo")
    hugepages = memory.copy()
    if memory["status"] == "observed":
        hugepages["value"] = {line.split(":", 1)[0]: line.split(":", 1)[1].strip()
                              for line in memory["value"].splitlines() if line.startswith(("HugePages_", "Hugepagesize:"))}
    library_entries = files.entries("/usr/lib/x86_64-linux-gnu")
    mlx5_library = library_entries.copy()
    if library_entries["status"] == "observed":
        mlx5_library["value"] = [name for name in library_entries["value"] if name.startswith("librte_net_mlx5.so")]
    virtualization = run(["systemd-detect-virt"])
    if virtualization.get("status") == "command_failed" and virtualization.get("detail") == "none":
        virtualization = {"status": "observed", "value": "none"}
    return {
        "schema_version": 1,
        "mode": "read_only_inventory",
        "host_platform": platform.system(),
        "hardware_acceptance": {"status": "not_run", "reason": "inventory_does_not_validate_offload_or_cpu_bypass"},
        "virtualization": virtualization,
        "dmi_product": files.read("/sys/class/dmi/id/product_name"),
        "dmi_vendor": files.read("/sys/class/dmi/id/sys_vendor"),
        "cpu_count": os.cpu_count() if cpu_count is None else cpu_count,
        "cpu_info": files.read("/proc/cpuinfo"),
        "dpdk_version": run(["pkg-config", "--modversion", "libdpdk"]),
        "mlx5_library": {"observation": mlx5_library, "scope": "common_debian_library_path_only", "proves_device": False},
        "pci_scan": {"status": pci_entries["status"], "network_devices": pci_devices, "unclassified_devices": unclassified},
        "interface_scan": {"status": interface_entries["status"], "interfaces": interfaces},
        "management_detection": {"routes": routes, "addresses": addresses, "interfaces": sorted(management)},
        "devlink_devices": run(["devlink", "dev", "show"]),
        "devlink_ports": run(["devlink", "port", "show"]),
        "iommu_groups": files.entries("/sys/kernel/iommu_groups"),
        "hugepages": hugepages,
        "tun": files.node("/dev/net/tun"),
        "vfio_control": files.node("/dev/vfio/vfio"),
        "vfio_nodes": files.entries("/dev/vfio"),
        "limitations": [
            "candidate_is_not_verified_hardware",
            "missing_or_denied_observations_do_not_prove_unsupported",
            "management_detection_may_be_incomplete",
            "no_bind_reset_sriov_or_network_changes_performed",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default="/", help="替换只读文件根目录，主要供离线测试使用")
    args = parser.parse_args()
    # 返回零只表示盘点程序执行完毕，验收状态始终单独记录为 not_run
    print(json.dumps(inspect_environment(Files(args.root)), ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
