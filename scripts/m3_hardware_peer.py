#!/usr/bin/env python3
"""独立真实报文对端，只读检查后用 AF_PACKET 收发，不更改网卡或路由"""

import base64
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import select
import socket
import struct
import subprocess
import sys
import time


def query(argv):
    result = subprocess.run(argv, capture_output=True, text=True, timeout=5, check=False)
    if result.returncode:
        raise RuntimeError(f"只读查询失败 {argv}: {result.stderr}")
    return result.stdout


def read(name):
    return Path(name).read_text().strip()


def pci_parent(bdf, root=Path("/sys/bus/pci/devices")):
    visited = set()
    for _ in range(8):
        if not isinstance(bdf, str) or not re.fullmatch(r"[0-9a-f]{4}:[0-9a-f]{2}:[0-1][0-9a-f]\.[0-7]", bdf):
            raise RuntimeError("PCI parent 不是完整网络 BDF")
        if bdf in visited:
            raise RuntimeError("PCI physfn 身份出现循环")
        visited.add(bdf)
        path = (root / bdf).resolve(strict=True)
        if path.name != bdf or int(read(path / "class"), 16) >> 16 != 2:
            raise RuntimeError("PCI parent 身份不完整或不是网络设备")
        parent = path / "physfn"
        if not parent.is_symlink():
            if parent.exists():
                raise RuntimeError("PCI physfn 不是可核对的 sysfs 链接")
            return bdf
        bdf = parent.resolve(strict=True).name
    raise RuntimeError("PCI physfn 层次超出安全边界")


def management_interfaces(routes, addresses, ssh_connection, require_ssh=False):
    """默认路由必须有输出接口，声明过的 SSH 本地地址必须实际匹配，未知一律拒绝"""
    if not isinstance(routes, list) or len(routes) != 2 or not isinstance(addresses, list):
        raise RuntimeError("管理路由或地址盘点不完整")
    management = set()
    for family in routes:
        if not isinstance(family, list):
            raise RuntimeError("管理路由 JSON 不是列表")
        for route in family:
            if not isinstance(route, dict):
                raise RuntimeError("管理路由条目无效")
            if route.get("dst", "default") not in ("default", "0.0.0.0/0", "::/0"):
                continue
            if route.get("type", "unicast") in ("blackhole", "unreachable", "prohibit", "throw"):
                continue
            hops = route.get("nexthops", [])
            if not isinstance(hops, list) or any(not isinstance(hop, dict) or not hop.get("dev") for hop in hops):
                raise RuntimeError("默认路由多路径接口不完整")
            devices = [hop["dev"] for hop in hops]
            if route.get("dev"):
                devices.append(route["dev"])
            if not devices or any(not isinstance(name, str) or not name for name in devices):
                raise RuntimeError("默认路由没有可核对的输出接口")
            management.update(devices)
    if not isinstance(ssh_connection, str) or (require_ssh and not ssh_connection):
        raise RuntimeError("SSH 管理连接身份缺失")
    if ssh_connection:
        ssh = ssh_connection.split()
        if (len(ssh) != 4 or any(not item.isdecimal() or not 1 <= int(item) <= 65535 for item in ssh[1::2])):
            raise RuntimeError("SSH_CONNECTION 格式或端口不完整")

        def address(text):
            parsed = ipaddress.ip_address(text.split("%", 1)[0])
            return parsed.ipv4_mapped if isinstance(parsed, ipaddress.IPv6Address) and parsed.ipv4_mapped else parsed

        address(ssh[0])
        local = address(ssh[2])
        scope = ssh[2].split("%", 1)[1] if "%" in ssh[2] else None
        matched = set()
        for interface in addresses:
            if not isinstance(interface, dict) or not isinstance(interface.get("addr_info", []), list):
                raise RuntimeError("管理接口地址条目无效")
            name = interface.get("ifname")
            for item in interface.get("addr_info", []):
                if not isinstance(item, dict) or not isinstance(item.get("local"), str):
                    raise RuntimeError("管理地址字段缺失")
                if address(item["local"]) == local and scope in (None, name, str(interface.get("ifindex"))):
                    if not isinstance(name, str) or not name:
                        raise RuntimeError("SSH 本地接口名称缺失")
                    matched.add(name)
        if not matched:
            raise RuntimeError("SSH 本地地址未匹配到任何实际接口")
        management.update(matched)
    if not management:
        raise RuntimeError("没有可信的管理接口证据")
    return sorted(management)


def interface_identity(name):
    if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_.:-]{1,15}", name):
        raise ValueError("接口名无效")
    base = Path("/sys/class/net") / name
    device = (base / "device").resolve(strict=True)
    parent = pci_parent(device.name)
    if int(read(device / "class"), 16) >> 16 != 2:
        raise RuntimeError("对端接口未映射到 PCI 网络设备")
    vendor, product = int(read(device / "vendor"), 16), int(read(device / "device"), 16)
    subsystem = int(read(device / "subsystem_vendor"), 16)
    driver = (device / "driver").resolve(strict=True).name
    virtual = (vendor in (0x15ad, 0x1af4, 0x1b36) or driver in ("vmxnet3", "virtio-pci", "virtio_net") or
               (vendor == 0x8086 and product == 0x100f and subsystem == 0x15ad))
    return {"interface": name, "pci_address": device.name, "pci_parent": parent, "vendor": vendor, "device": product,
            "subsystem_vendor": subsystem, "driver": driver, "virtual": virtual,
            "mac": read(base / "address")}


def inspect(interfaces):
    routes = [query(["ip", "-j", "route", "show", "table", "all"]),
              query(["ip", "-j", "-6", "route", "show", "table", "all"])]
    addresses = query(["ip", "-j", "address", "show"])
    management = management_interfaces([json.loads(raw) for raw in routes], json.loads(addresses),
                                       os.environ.get("SSH_CONNECTION", ""), require_ssh=True)
    protected_pci = set()
    for name in management:
        try:
            protected_pci.add(interface_identity(name)["pci_parent"])
        except (OSError, ValueError):
            # 管理接口无法映射时不允许继续收发，不能把未知当成安全
            raise RuntimeError("对端管理接口的 PCI 身份不完整")
    devices = [interface_identity(name) for name in interfaces]
    return {"machine_id": hashlib.sha256(Path("/etc/machine-id").read_bytes()).hexdigest(),
            "interfaces": devices, "management_interfaces": sorted(management),
            "management_pci": sorted(protected_pci), "raw_routes": routes, "raw_addresses": addresses}


def require_dedicated(observation):
    if not observation.get("management_interfaces") or not observation.get("management_pci"):
        raise RuntimeError("对端管理接口或 PCI parent 身份未知")
    for device in observation["interfaces"]:
        if device["virtual"]:
            raise RuntimeError("对端接口是虚拟设备，不能作为实机报文证据")
        if not device.get("pci_parent"):
            raise RuntimeError("对端测试接口的 PCI parent 身份未知")
        if (device["interface"] in observation["management_interfaces"] or
                device["pci_parent"] in observation["management_pci"]):
            raise RuntimeError("对端测试口与管理口或其 PCI 设备重合")


def pcap(records):
    result = bytearray(struct.pack("<IHHIIII", 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1))
    for timestamp, frame in records:
        seconds = int(timestamp)
        result.extend(struct.pack("<IIII", seconds, int((timestamp - seconds) * 1000000), len(frame), len(frame)))
        result.extend(frame)
    return bytes(result)


def main(request):
    interfaces = [request["tx_interface"], request["rx_interface"]]
    if interfaces[0] == interfaces[1]:
        raise ValueError("发送和接收必须使用两个独立接口")
    observation = inspect(interfaces)
    require_dedicated(observation)
    result = {"schema_version": 1, "run_id": request["run_id"], "inventory": observation,
              "capture_origin": "live_AF_PACKET", "simulation": False}
    if request["operation"] == "inspect":
        return result
    if request["operation"] != "traffic":
        raise ValueError("未知操作")
    frames = [bytes.fromhex(frame) for frame in request["frames_hex"]]
    packets = request["packets_per_frame"]
    duration = request["duration_seconds"]
    nonce = request["run_id"].encode()
    if (not frames or len(frames) > 8 or any(len(frame) < 60 or len(frame) > 1514 or nonce not in frame for frame in frames)
            or not isinstance(packets, int) or isinstance(packets, bool) or not 1 <= packets <= 128 or
            not isinstance(duration, (int, float)) or not 0.2 <= duration <= 10):
        raise ValueError("报文或限时参数越界")
    received, sent = [], []
    # 两个套接字只借用现有接口，退出和异常都会关闭，不设置混杂模式或更改链路
    with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)) as receiver:
        with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)) as sender:
            receiver.bind((interfaces[1], 0))
            sender.bind((interfaces[0], 0))
            receiver.setblocking(False)
            for _ in range(packets):
                for frame in frames:
                    if sender.send(frame) != len(frame):
                        raise RuntimeError("AF_PACKET 未完整提交报文")
                    sent.append((time.time(), frame))
            deadline = time.monotonic() + duration
            while time.monotonic() < deadline:
                ready, _, _ = select.select([receiver], [], [], max(0, deadline - time.monotonic()))
                if not ready:
                    break
                frame, address = receiver.recvfrom(65535)
                # 排除本机发出的副本，保留本轮 nonce 的全部入站报文以便发现重复或改写
                if address[2] != socket.PACKET_OUTGOING and nonce in frame:
                    received.append((time.time(), frame))
    result.update({"tx_pcap_base64": base64.b64encode(pcap(sent)).decode(),
                   "rx_pcap_base64": base64.b64encode(pcap(received)).decode(),
                   "tx_capture_kind": "AF_PACKET_submission", "rx_capture_kind": "external_interface_ingress"})
    return result


if __name__ == "__main__":
    try:
        # SSH 启动器把参数作为 Python 数据传入，不把接口名或路径拼进远端 shell
        request = globals().get("M3_REQUEST")
        if request is None:
            request = json.load(sys.stdin)
        print(json.dumps(main(request), ensure_ascii=False), flush=True)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(json.dumps({"error": str(error)}), file=sys.stderr, flush=True)
        raise SystemExit(1)
