#!/usr/bin/env python3
"""M3 实机证据门禁，缺硬件返回 blocked，虚拟和模拟结果永远不登记为硬件通过"""

import argparse
import base64
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import sys
import time
import uuid

from inspect_m3_environment import inspect_environment, value
from m3_hardware_peer import management_interfaces, pci_parent as read_pci_parent
from verify_m2 import source_digest


TEMPLATES = ("pair", "uplink-vf", "uplink-sf", "pf-vf", "pf-sf", "vf-vf", "sf-sf")
VIRTUAL_PMDS = ("ring", "tap", "null", "pcap", "af_packet", "af_xdp", "virtio", "vmxnet3", "vhost", "memif", "failsafe")
RESET_STAGE = "physical_reset_recovery"


class Blocked(RuntimeError):
    pass


def canonical_digest(data):
    return hashlib.sha256(json.dumps(data, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def fields(raw):
    # 字段的值可以含空格，例如固件版本，以下一个 key= 而不是单个空格作为边界
    return {match.group(1): match.group(2).strip() for match in
            re.finditer(r"([A-Za-z][A-Za-z0-9_-]*)=(.*?)(?=\s+[A-Za-z][A-Za-z0-9_-]*=|\n|$)", raw)}


def not_found(record):
    return record["returncode"] != 0 and re.search(r"\(-2\)\s*$", record["stderr"]) is not None


def header_fields(raw):
    lines = raw.splitlines()
    if not lines or not lines[0].strip():
        raise RuntimeError("管理响应缺少首行字段")
    return fields(lines[0])


def rule_identity(raw, rid):
    # shared-count 动作也有 generation，规则版本只允许从首行读取
    header = header_fields(raw)
    if header.get("id") != str(rid):
        raise RuntimeError("规则响应 ID 与请求不一致")
    require_integer(int(header["generation"]), 1, (1 << 64) - 2, "rule generation")
    return header


def matrix():
    rows = [{"stage": "discovery", "status": "blocked", "capability": "unknown", "evidence": []}]
    rows += [{"stage": "transfer_" + name, "status": "blocked", "capability": "unknown", "evidence": []}
             for name in TEMPLATES]
    rows += [{"stage": name, "status": "blocked", "capability": "unknown", "evidence": []}
             for name in ("count", "age", "shared_count", "cpu_bypass", "physical_removal",
                          "physical_reset_recovery", "mlx5_hardware", "non_mlx5_hardware")]
    rows.append({"stage": "performance", "status": "out_of_scope", "capability": "not_required",
                 "reason": "M4 性能目标不作为 M3 通过门槛", "evidence": []})
    for row in rows:
        if row["status"] == "blocked":
            row["reason"] = "尚无本轮实机证据"
    return rows


def mark(report, stage, status, reason, evidence=()):
    row = next(item for item in report["capability_matrix"] if item["stage"] == stage)
    row.update(status=status, reason=reason, evidence=list(evidence),
               capability="supported" if status == "passed" else "rejected" if status == "rejected" else "unknown")


def require_integer(number, low, high, name):
    if not isinstance(number, int) or isinstance(number, bool) or not low <= number <= high:
        raise Blocked(name + " 超出允许范围")
    return number


def validate_ssh_argv(argv):
    """只允许连接参数和最后一个主机名，不能借附加位置参数执行另一条远端命令"""
    if (not isinstance(argv, list) or not 2 <= len(argv) <= 20 or
            any(not isinstance(item, str) or not item or len(item) > 1024 for item in argv) or
            Path(argv[0]).name not in ("ssh", "ssh.exe")):
        raise Blocked("缺少受限的 SSH 对端参数")
    host = re.compile(r"[A-Za-z0-9_@.:%\[\]-]+")
    if argv[-1].startswith("-") or host.fullmatch(argv[-1]) is None:
        raise Blocked("SSH 最后一个参数必须是明确主机名，不允许远端命令")
    index = 1
    while index < len(argv) - 1:
        option = argv[index]
        if option not in ("-o", "-p", "-i", "-J") or index + 1 >= len(argv) - 1:
            raise Blocked("SSH 含未允许的选项或额外远端命令")
        parameter = argv[index + 1]
        if option == "-o" and not re.fullmatch(r"BatchMode=yes|ConnectTimeout=[1-9][0-9]?|StrictHostKeyChecking=yes", parameter):
            raise Blocked("SSH -o 只允许 BatchMode、ConnectTimeout 和严格主机检查")
        if option == "-p" and (not parameter.isdecimal() or not 1 <= int(parameter) <= 65535):
            raise Blocked("SSH 端口无效")
        if option == "-J" and any(host.fullmatch(item) is None or item.startswith("-") for item in parameter.split(",")):
            raise Blocked("SSH 跳板主机无效")
        index += 2
    return argv


def validate_endpoint(endpoint, role):
    if not isinstance(endpoint, dict):
        raise Blocked(role + " 端点必须为对象")
    require_integer(endpoint.get("port_id"), 0, 65534, role + " port_id")
    for key in ("pci_address", "interface", "pmd", "device_name", "firmware", "role"):
        if not isinstance(endpoint.get(key), str) or not endpoint[key] or "FILL" in endpoint[key]:
            raise Blocked(role + " 缺少显式 " + key)
    bdf = endpoint["pci_address"]
    if not re.fullmatch(r"[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-1][0-9a-fA-F]\.[0-7]", bdf):
        raise Blocked(role + " PCI 地址格式无效")
    if any(name in endpoint["pmd"].lower() for name in VIRTUAL_PMDS):
        raise Blocked(role + " PMD 为虚拟或软件设备")
    if not re.fullmatch(re.escape(bdf) + r"(?:_representor_[A-Za-z0-9_]+)?", endpoint["device_name"]):
        raise Blocked(role + " DPDK device 未明确关联完整 PCI 设备")
    require_integer(endpoint.get("switch_domain"), 0, 65534, role + " switch_domain")
    if endpoint["role"] not in ("unknown", "uplink", "pf", "vf", "sf"):
        raise Blocked(role + " 角色未知")


def configured_endpoints(config):
    endpoints = list(config["ports"].items())
    if "planned_reset" in config.get("stages", []):
        endpoints += [("reset-port-" + str(item["port_id"]), item) for item in config["planned_reset"]["device_ports"]]
    return endpoints


def validate_config(config):
    if not isinstance(config, dict) or config.get("schema_version") != 1:
        raise Blocked("配置 schema_version 必须为 1")
    ports = config.get("ports", {})
    if not isinstance(ports, dict) or set(ports) != {"install", "source", "destination"}:
        raise Blocked("必须只显式列出 install/source/destination 三个端点")
    for role in ("install", "source", "destination"):
        validate_endpoint(ports.get(role, {}), role)
    if ports["source"]["port_id"] == ports["destination"]["port_id"]:
        raise Blocked("源端口与目的端口相同")
    if len({item["switch_domain"] for item in ports.values()}) != 1:
        raise Blocked("安装、源和目的端口不在同一个已知交换域")
    if config.get("template") not in TEMPLATES:
        raise Blocked("未选择已实现的 transfer 模板")
    stages = config.get("stages", ["basic", "age", "shared_count"])
    if not isinstance(stages, list) or not stages or any(not isinstance(item, str) for item in stages) or len(set(stages)) != len(stages) or any(
            item not in ("basic", "age", "shared_count", "planned_reset") for item in stages) or "basic" not in stages:
        raise Blocked("stages 必须含 basic，可选 age/shared_count/planned_reset，且不能重复")
    if "planned_reset" in stages:
        reset = config.get("planned_reset", {})
        if not isinstance(reset, dict) or reset.get("allow_full_group_reset") is not True:
            raise Blocked("planned_reset 必须显式允许对完整配置端口组执行物理复位")
        group = reset.get("device_ports")
        if not isinstance(group, list) or not 1 <= len(group) <= 16 or any(not isinstance(item, dict) for item in group):
            raise Blocked("planned_reset 必须列出 daemon 完整配置端口组的独立身份")
        ids = [require_integer(item.get("port_id"), 0, 65534, "reset port_id") for item in group]
        if len(ids) != len(set(ids)):
            raise Blocked("复位端口组含重复端口")
        by_id = dict(zip(ids, group))
        for endpoint in ports.values():
            if by_id.get(endpoint["port_id"]) != endpoint:
                raise Blocked("复位完整端口组没有精确包含图的全部端点身份")
        # 额外配置端口也要通过与图端点相同的身份和 devargs 校验
        for endpoint in group:
            validate_endpoint(endpoint, "reset-port-" + str(endpoint["port_id"]))
        modes = reset.get("modes", ["count"])
        if (not isinstance(modes, list) or not modes or any(not isinstance(item, str) for item in modes) or
                len(set(modes)) != len(modes) or "count" not in modes or
                any(item not in ("count", "age", "shared_count") for item in modes)):
            raise Blocked("planned_reset modes 必须含 count，可选 age/shared_count 且不能重复")
        path = reset.get("snapshot_path")
        if not isinstance(path, str) or not Path(path).is_absolute() or "FILL" in path:
            raise Blocked("复位阶段必须显式指定 daemon 实际使用的绝对 snapshot_path")
    require_integer(config.get("rule_id"), 1, (1 << 64) - 2, "rule_id")
    require_integer(config.get("priority", 10), 0, (1 << 32) - 1, "priority")
    devargs = config.get("eal_devargs")
    if not isinstance(devargs, list) or not devargs or any(not isinstance(item, str) or not item for item in devargs):
        raise Blocked("缺少明确的 EAL devargs")
    if any("--vdev" in item or "--no-pci" in item for item in devargs):
        raise Blocked("EAL 配置含虚拟设备或禁用 PCI")
    if any(not any(endpoint["pci_address"] in item for item in devargs) for _, endpoint in configured_endpoints(config)):
        raise Blocked("所选每个 PCI 设备都必须出现在显式 EAL devargs 中")
    peer = config.get("peer", {})
    if not isinstance(peer, dict):
        raise Blocked("peer 必须为对象")
    ssh = peer.get("ssh_argv")
    validate_ssh_argv(ssh)
    if peer.get("tx_interface") == peer.get("rx_interface"):
        raise Blocked("对端必须有独立的发送和接收接口")
    for key in ("tx_interface", "rx_interface"):
        if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,15}", str(peer.get(key, ""))):
            raise Blocked("对端接口名无效")
    links = config.get("physical_links")
    if not isinstance(links, list) or len(links) < 2 or any(not isinstance(link, str) or not link.strip() or
                                                           "FILL" in link for link in links):
        raise Blocked("缺少源和目的的真实物理连接/端点说明")
    return config


def hardware_gate(inventory, config, environ=None, parent_reader=None):
    """先验证独立 PCI 观察，管理身份不明或虚拟设备时不查询/改变测试端口"""
    validate_config(config)
    routes = inventory["management_detection"]["routes"]
    addresses = inventory["management_detection"]["addresses"]
    if any(item.get("status") != "observed" for item in routes) or addresses.get("status") != "observed":
        raise Blocked("管理路由或地址观察不完整")
    environ = os.environ if environ is None else environ
    parent_reader = pci_parent if parent_reader is None else parent_reader
    try:
        management = management_interfaces([value(item) for item in routes], value(addresses),
                                           environ.get("SSH_CONNECTION", ""))
    except (OSError, ValueError, RuntimeError) as error:
        raise Blocked("管理路由或 SSH 本地地址证据不完整: " + str(error)) from error
    if set(management) != set(inventory["management_detection"]["interfaces"]):
        raise Blocked("管理接口清单与独立默认路由/SSH 本地地址核对不一致")
    devices = {item["pci_address"]: item for item in inventory["pci_scan"]["network_devices"]}
    interfaces = {item["name"]: item for item in inventory["interface_scan"]["interfaces"]}
    protected = set()
    for name in management:
        interface = interfaces.get(name)
        if interface is None or interface["pci_device"].get("status") != "observed":
            raise Blocked("管理接口无法映射到 PCI，不能保证测试设备独立")
        protected.add(value(interface["pci_device"]))
    def checked_parent(bdf):
        try:
            parent = parent_reader(bdf)
        except (OSError, ValueError, RuntimeError) as error:
            raise Blocked("管理或测试接口 PCI parent 身份不完整: " + str(error)) from error
        if not isinstance(parent, str) or not re.fullmatch(r"[0-9a-f]{4}:[0-9a-f]{2}:[0-1][0-9a-f]\.[0-7]", parent):
            raise Blocked("管理或测试接口 PCI parent 不是完整 BDF")
        return parent

    parents = {bdf: checked_parent(bdf) for bdf in protected}
    for role, configured in configured_endpoints(config):
        bdf, name = configured["pci_address"], configured["interface"]
        device, interface = devices.get(bdf), interfaces.get(name)
        if device is None or device["classification"]["kind"] != "physical_candidate":
            raise Blocked(role + " 没有真实 PCI 候选证据，虚拟网卡和库文件不能替代设备")
        if interface is None or value(interface["pci_device"]) != bdf:
            raise Blocked(role + " 接口与 PCI 设备未独立核对")
        if (interface.get("management_interface") is not False or bdf in protected or
                checked_parent(bdf) in parents.values()):
            raise Blocked(role + " 与管理链路或管理 PCI 设备重合/未知")
    return sorted(set(parents.values()))


def parse_pcap(data):
    """只接受未截断的经典 Ethernet PCAP，拒绝错误长度、未知链路和尾部垃圾"""
    if len(data) < 24:
        raise ValueError("PCAP 头被截断")
    magic = data[:4]
    order = {b"\xd4\xc3\xb2\xa1": "<", b"\xa1\xb2\xc3\xd4": ">",
             b"\x4d\x3c\xb2\xa1": "<", b"\xa1\xb2\x3c\x4d": ">"}.get(magic)
    if order is None:
        raise ValueError("未知 PCAP magic")
    _, major, minor, _, _, snaplen, link = struct.unpack(order + "IHHIIII", data[:24])
    if (major, minor) != (2, 4) or link != 1 or not 60 <= snaplen <= 65535:
        raise ValueError("PCAP 版本或链路类型无效")
    result, offset = [], 24
    while offset < len(data):
        if len(data) - offset < 16:
            raise ValueError("PCAP 记录头被截断")
        _, _, captured, original = struct.unpack(order + "IIII", data[offset:offset + 16])
        offset += 16
        if captured != original or captured < 14 or captured > snaplen or captured > len(data) - offset:
            raise ValueError("PCAP 记录被截断或长度无效")
        result.append(data[offset:offset + captured])
        offset += captured
    return result


def verify_packet_evidence(tx_data, rx_data, frames, packets, before, after, workers_before, workers_after):
    if not frames or len(set(frames)) != len(frames) or not isinstance(packets, int) or isinstance(packets, bool) or packets < 1:
        raise ValueError("期望帧必须非空且互不相同，期望包数必须为正整数")
    expected = Counter({frame: packets for frame in frames})
    if Counter(parse_pcap(tx_data)) != expected or Counter(parse_pcap(rx_data)) != expected:
        raise ValueError("真实提交/外部接收 PCAP 的报文字节或包数不一致")
    expected_packets = len(frames) * packets
    expected_bytes = sum(len(frame) * packets for frame in frames)
    if (after["hits"] - before["hits"], after["bytes"] - before["bytes"]) != (expected_packets, expected_bytes):
        raise ValueError("真实硬件 COUNT 增量与报文数量/长度不一致")
    for key in ("rx_packets", "rx_bytes", "tx_packets", "tx_bytes"):
        if key not in workers_before or key not in workers_after or workers_before[key] != workers_after[key]:
            raise ValueError("CPU worker 的实际处理计数变化或证据缺失")
    return {"packets": expected_packets, "bytes": expected_bytes, "byte_exact": True}


def execute(argv, log, timeout=10, stdin=None):
    """每阶段有独立进程组，超时先结束再强制回收，不留下本地 SSH 或客户端子进程"""
    started = time.monotonic()
    timed_out = False
    output, error = b"", b""
    with subprocess.Popen(argv, stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True) as process:
        try:
            output, error = process.communicate(stdin, timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            for action in (signal.SIGTERM, signal.SIGKILL):
                try:
                    os.killpg(process.pid, action)
                except ProcessLookupError:
                    pass
                try:
                    output, error = process.communicate(timeout=2)
                    break
                except subprocess.TimeoutExpired:
                    continue
        except BaseException:
            # 入口被中断时也结束本阶段子进程，外层 finally 才能及时恢复暂停的 daemon
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.communicate(timeout=2)
            raise
    record = {"argv": argv, "returncode": process.returncode, "seconds": round(time.monotonic() - started, 3),
              "stdout": output.decode(errors="replace"), "stderr": error.decode(errors="replace")}
    Path(log).write_text(json.dumps(record, ensure_ascii=False, indent=2), encoding="utf-8")
    if timed_out:
        raise RuntimeError("阶段超时: " + str(argv))
    return record


def peer(config, request, destination):
    helper = Path(__file__).with_name("m3_hardware_peer.py").read_text(encoding="utf-8")
    # SSH 远端命令固定，用户参数只作为 Python 数据通过标准输入传递
    script = "import json\nM3_REQUEST=json.loads(" + repr(json.dumps(request)) + ")\n" + helper
    result = execute(config["peer"]["ssh_argv"] + ["python3", "-u", "-"],
                     destination / ("peer-" + request["operation"] + "-command.json"),
                     timeout=20, stdin=script.encode())
    if result["returncode"] != 0:
        raise Blocked("外部对端只读检查或 AF_PACKET 收发失败，见 peer 阶段日志")
    return json.loads(result["stdout"])


def same_peer(left, right):
    keys = ("machine_id", "management_interfaces", "management_pci")
    return (all(left.get(key) == right.get(key) for key in keys) and
            {item["interface"]: item for item in left["interfaces"]} ==
            {item["interface"]: item for item in right["interfaces"]})


def build_binding(root, build):
    if not (build / "meson-info/meson-info.json").is_file() or not (build / "meson-info/intro-buildoptions.json").is_file():
        raise Blocked("缺少可核对的 Meson 构建元数据")
    metadata = json.loads((build / "meson-info/meson-info.json").read_text())
    if Path(metadata["directories"]["source"]).resolve() != root:
        raise Blocked("构建目录对应另一份源码")
    options = {item["name"]: item["value"] for item in
               json.loads((build / "meson-info/intro-buildoptions.json").read_text())}
    if options.get("tests") is not True or options.get("werror") is not True:
        raise Blocked("构建必须开启 tests 与 werror")
    binaries = {}
    for name in ("dppd", "dppctl"):
        path = build / name
        if not path.is_file():
            raise Blocked("构建缺少 " + name)
        binaries[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    return {"source_root": str(root), "build_root": str(build), "options": {"tests": True, "werror": True},
            "source_sha256": source_digest(root), "binary_sha256": binaries}


def process_identity(pid, build, control_socket):
    require_integer(pid, 2, (1 << 31) - 1, "daemon_pid")
    # 正式管理入口使用 SEQPACKET，类型错误时必须在任何规则或进程信号操作前停止
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as connection:
        connection.settimeout(3)
        try:
            connection.connect(str(control_socket))
        except OSError as error:
            raise Blocked("无法连接正式 SEQPACKET 管理 socket: " + str(error)) from error
        peer_pid, _, _ = struct.unpack("3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
    if peer_pid != pid:
        raise Blocked("管理 socket 不属于指定 daemon PID")
    process = Path("/proc") / str(pid)
    if hashlib.sha256((process / "exe").read_bytes()).digest() != hashlib.sha256((build / "dppd").read_bytes()).digest():
        raise Blocked("运行中的 daemon 与本次构建二进制不一致")
    raw = (process / "stat").read_text()
    start_time = raw[raw.rfind(")") + 2:].split()[19]
    return {"pid": pid, "start_time": start_time,
            "cmdline": [part.decode() for part in (process / "cmdline").read_bytes().split(b"\0") if part]}


def all_tasks_stopped(pid):
    observations = {}
    for task in (Path("/proc") / str(pid) / "task").iterdir():
        raw = (task / "status").read_text()
        observations[task.name] = raw
    if not observations or any(not re.search(r"^State:\s+T", raw, re.MULTILINE) for raw in observations.values()):
        raise RuntimeError("尚未观察到 daemon 的全部线程都停止")
    return observations


def pin_process(identity, build, control_socket):
    """固定原进程，PID 被复用也不能把暂停或恢复信号发送给另一个进程"""
    if not callable(getattr(os, "pidfd_open", None)) or not callable(getattr(signal, "pidfd_send_signal", None)):
        raise Blocked("当前 Python/内核不提供 pidfd，不能安全暂停测试 daemon")
    before = process_identity(identity["pid"], build, control_socket)
    if (before["pid"], before["start_time"]) != (identity["pid"], identity["start_time"]):
        raise Blocked("打开 pidfd 前 daemon 身份变化")
    try:
        descriptor = os.pidfd_open(identity["pid"], 0)
    except OSError as error:
        raise Blocked("内核或权限不允许打开 pidfd: " + str(error)) from error
    try:
        after = process_identity(identity["pid"], build, control_socket)
        if (after["pid"], after["start_time"]) != (before["pid"], before["start_time"]):
            raise Blocked("打开 pidfd 期间 daemon PID 已被复用")
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


def wait_stopped(pid, timeout=3):
    deadline = time.monotonic() + timeout
    while True:
        try:
            return all_tasks_stopped(pid)
        except RuntimeError:
            if time.monotonic() >= deadline:
                raise RuntimeError("等待 daemon 全部线程暂停超时")
            time.sleep(0.01)


def _create_graph(ctl, graph, modifiers, owned, report, stage):
    plan = ctl("transfer-plan", *graph, *modifiers, required=False)
    if plan["returncode"]:
        mark(report, stage, "rejected", plan["stderr"])
        return False
    applied = ctl("transfer-apply", *graph, fields(plan["stdout"])["repository-generation"],
                  *modifiers, required=False)
    if applied["returncode"]:
        mark(report, stage, "rejected", applied["stderr"])
        return False
    ids = [graph[0]] if graph[5] == "oneway" else [graph[0], graph[0] + 1]
    for rid in ids:
        owned[rid] = None
    for rid in ids:
        current = rule_identity(ctl("get", rid)["stdout"], rid)
        owned[rid] = current["generation"]
        if fields(ctl("rule-status", rid, owned[rid])["stdout"]).get("backend") != "rte_flow":
            raise RuntimeError("生命周期阶段规则实际后端不是 rte_flow")
    return True


def _cleanup_rules(ctl, owned):
    errors = []
    for rid, generation in list(owned.items()):
        try:
            current = ctl("get", rid, required=False)
            if not_found(current):
                del owned[rid]
                continue
            if current["returncode"]:
                raise RuntimeError(current["stderr"])
            observed = rule_identity(current["stdout"], rid)["generation"]
            if generation is not None and generation != observed:
                raise RuntimeError("规则代次发生变化，不能删除替代规则")
            removed = ctl("delete", rid, observed, required=False)
            if removed["returncode"] or fields(removed["stdout"]).get("deleted") != "yes":
                raise RuntimeError("精确删除失败: " + removed["stderr"])
            del owned[rid]
        except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
            errors.append(str(rid) + ": " + str(error))
    return errors


def _count(ctl, command, rid, generation):
    observed = fields(ctl(command, rid, generation)["stdout"])
    identity_key = "rule" if command == "count" else "id"
    if observed.get(identity_key) != str(rid) or observed.get("generation") != str(generation):
        raise RuntimeError("COUNT 响应不是请求的精确对象版本")
    return {key: require_integer(int(observed[key]), 0, (1 << 64) - 1, key) for key in ("hits", "bytes")}


def _check_batch(batch, before, after):
    return verify_packet_evidence(batch["tx"], batch["rx"], batch["frames"], batch["packets"], before, after,
                                  batch["workers_before"], batch["workers_after"])


def _age(ctl, rid, generation):
    age = fields(ctl("rule-age", rid, generation)["stdout"])
    if age.get("rule") != str(rid) or age.get("generation") != str(generation):
        raise RuntimeError("AGE 响应不是请求的精确规则版本")
    return age


def runtime_port_identities(config, ctl):
    identities = {}
    for role, configured in configured_endpoints(config):
        port = ctl("port-show", configured["port_id"])
        capability = ctl("capability-show", configured["port_id"])
        actual, caps = fields(port["stdout"]), fields(capability["stdout"])
        expected = {"driver": configured["pmd"], "device": configured["device_name"],
                    "interface": configured["interface"], "role": configured["role"],
                    "switch-domain": str(configured["switch_domain"])}
        if any(actual.get(key) != expected_value for key, expected_value in expected.items()):
            raise Blocked(role + " port-show 身份与显式配置不一致")
        if (caps.get("firmware") != configured["firmware"] or caps.get("firmware-error") != "0" or
                caps.get("firmware") in (None, "unknown", "") or caps.get("device") != configured["device_name"] or
                caps.get("driver") != configured["pmd"] or not caps.get("dpdk")):
            raise Blocked(role + " PMD/device/firmware/DPDK 证据不完整或不一致")
        if "planned_reset" in config.get("stages", []) and any(caps.get(key) != "yes" for key in ("configured", "started")):
            raise Blocked(role + " 复位完整端口组尚未全部配置和启动")
        # 缓存命中次数会变化，拓扑、设备和实际队列配置则必须在复位前后完全一致
        stable = {key: item for key, item in actual.items() if key not in ("state", "link")}
        identities[str(configured["port_id"])] = {
            "port": stable, "capability": fields("\n".join(capability["stdout"].splitlines()[:2]))}
    return identities


def read_device_status(ctl):
    raw = ctl("device-status")["stdout"]
    lines = raw.splitlines()
    group = [fields(line) for line in lines if line.startswith("device-ports ")]
    if len(group) != 1 or not re.fullmatch(r"[0-9]+(?:,[0-9]+)*", group[0].get("ids", "")):
        raise Blocked("协议 v27 device-status 没有可信的完整端口清单")
    ids = [int(item) for item in group[0]["ids"].split(",")]
    count = require_integer(int(group[0]["count"]), 1, 16, "device-ports count")
    if count != len(ids) or len(set(ids)) != count or any(not 0 <= item <= 65534 for item in ids):
        raise Blocked("device-status 完整端口清单重复或数量不一致")
    observed = fields(raw)
    for key in ("sequence", "approved-sequence", "attempts", "resets", "recoveries", "phase",
                "desired-rules", "desired-shared", "actual-rules", "actual-shared"):
        observed[key] = require_integer(int(observed[key]), 0, (1 << 64) - 2, key)
    for key in ("last-error", "cleanup-error"):
        observed[key] = int(observed[key])
    for key in ("stopped", "reset", "configured", "started", "reset-events", "removal-events", "destroy-events"):
        observed[key] = require_integer(int(observed[key], 0), 0, (1 << 32) - 1, key)
    observed["device_ports"] = ids
    return observed


def check_running_device(status, config, rules=0, shared=0):
    configured = {item["port_id"] for item in config["planned_reset"]["device_ports"]}
    if set(status["device_ports"]) != configured:
        raise Blocked("device-status 完整端口组与显式配置不一致，禁止只复位图中三个端点")
    if (status.get("state") != "running" or any(status.get(key) != "no" for key in
            ("planned", "quarantined", "workers-stopped")) or status["last-error"] or status["cleanup-error"] or
            status["sequence"] != status["approved-sequence"] or status["removal-events"] or status["destroy-events"]):
        raise Blocked("设备不是已批准且 worker 运行的稳定状态，不能尝试主动复位")
    if any(status[key] != expected for key, expected in
           (("desired-rules", rules), ("actual-rules", rules), ("desired-shared", shared), ("actual-shared", shared))):
        raise Blocked("专用 daemon 的 desired/actual 规则或共享对象不符合本轮独占计划")


def reset_snapshot(config, identity):
    """只接受实际 daemon 应用参数中唯一且绝对的持久化路径，不信任另给的无关文件"""
    arguments = identity["cmdline"]
    if "--" not in arguments:
        raise Blocked("不能确定 daemon 的 EAL 与应用参数边界")
    application = arguments[arguments.index("--") + 1:]
    observed = {}
    names = {"--ports": "ports", "-p": "ports", "--queues": "queues", "-q": "queues",
             "--state-path": "state-path", "--recovery-path": "recovery-path"}
    for index, item in enumerate(application):
        option, equal, parameter = item.partition("=")
        if option not in names:
            continue
        key = names[option]
        if key in observed or (not equal and index + 1 == len(application)):
            raise Blocked("daemon 应用参数重复或缺失: " + key)
        observed[key] = parameter if equal else application[index + 1]
    raw_ports = observed.get("ports", "")
    if not re.fullmatch(r"[0-9]+(?:,[0-9]+)*", raw_ports):
        raise Blocked("复位阶段要求 daemon 显式使用 --ports 列出完整端口组")
    port_ids = [int(item) for item in raw_ports.split(",")]
    if (len(port_ids) != len(set(port_ids)) or set(port_ids) !=
            {item["port_id"] for item in config["planned_reset"]["device_ports"]}):
        raise Blocked("daemon 实际 --ports 与复位完整端口组不一致")
    if not re.fullmatch(r"[1-9][0-9]*", observed.get("queues", "1")) or int(observed.get("queues", "1")) > 16:
        raise Blocked("daemon 实际 queues 参数不完整")
    unsafe_modes = ("--ipv4-routing", "--ip-routing", "--conntrack", "--snat", "--dnat", "--snat6", "--dnat6", "--nd6")
    if any(item.split("=", 1)[0] in unsafe_modes for item in application):
        raise Blocked("主动硬件复位验收只支持纯硬件专用 daemon，不能含路由、连接跟踪或邻居状态")
    paths = []
    for index, item in enumerate(application):
        if item == "--state-path":
            if index + 1 == len(application):
                raise Blocked("daemon --state-path 缺少参数")
            paths.append(application[index + 1])
        elif item.startswith("--state-path="):
            paths.append(item.split("=", 1)[1])
    path = Path(config["planned_reset"]["snapshot_path"])
    if len(paths) != 1 or not Path(paths[0]).is_absolute() or Path(paths[0]).resolve() != path.resolve():
        raise Blocked("snapshot_path 与当前 daemon 实际 --state-path 不一致或不唯一")
    if path.is_symlink() or not path.is_file() or not 1 <= path.stat().st_size <= 16 * 1024 * 1024:
        raise Blocked("复位阶段要求实际存在且有界的普通快照文件")
    return path


def pci_parent(bdf, root=Path("/sys/bus/pci/devices")):
    """沿 VF 的 physfn 保护管理 PF，不把不同 representor 或 VF 误当独立复位设备"""
    try:
        return read_pci_parent(bdf, root)
    except (OSError, ValueError, RuntimeError) as error:
        raise Blocked("PCI parent 读取不完整: " + str(error)) from error


def reset_preflight(config, report, ctl, identity, destination):
    # 独立盘点和运行期身份在第一次创建前与每次复位前都重新读取
    inventory = inspect_environment()
    protected = hardware_gate(inventory, config)
    parents = {bdf: pci_parent(bdf) for bdf in protected +
               [endpoint["pci_address"] for _, endpoint in configured_endpoints(config)]}
    management = {parents[bdf] for bdf in protected}
    if any(parents[item["pci_address"]] in management for item in config["planned_reset"]["device_ports"]):
        raise Blocked("复位完整端口组与管理 PCI parent 重合")
    snapshot = reset_snapshot(config, identity)
    ports = runtime_port_identities(config, ctl)
    status = read_device_status(ctl)
    data = {"inventory": inventory, "pci_parents": parents, "ports": ports, "device_status": status,
            "snapshot_path": str(snapshot)}
    (destination / "reset-preflight.json").write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    report.setdefault("planned_reset", {})["group_port_ids"] = status["device_ports"]
    return status, ports, snapshot


def reset_rule_evidence(ctl, owned, shared_id=None, shared_generation=None):
    rules = {}
    for rid, generation in owned.items():
        current = ctl("get", rid)["stdout"]
        if rule_identity(current, rid).get("generation") != str(generation):
            raise RuntimeError("复位规则精确代次发生变化")
        if fields(ctl("rule-status", rid, generation)["stdout"]).get("backend") != "rte_flow":
            raise RuntimeError("复位恢复使用了非硬件后端")
        rules[str(rid)] = current
    evidence = {"rules": rules, "repository": header_fields(ctl("list")["stdout"]),
                "shared_list": header_fields(ctl("shared-count-list")["stdout"]),
                "persistence": fields(ctl("persistence-status")["stdout"])}
    if evidence["persistence"].get("enabled") != "yes" or evidence["persistence"].get("dirty") != "no":
        raise Blocked("复位证据需要已启用且干净的持久化快照")
    if shared_id is not None:
        evidence["shared"] = ctl("shared-count-get", shared_id, shared_generation)["stdout"]
        shared = fields(evidence["shared"])
        first_rule = rule_identity(next(iter(rules.values())), next(iter(owned)))
        if (shared.get("id") != str(shared_id) or shared.get("generation") != str(shared_generation) or
                shared.get("references") != str(len(owned)) or shared.get("domain") != "transfer" or
                shared.get("install") != first_rule.get("install-port")):
            raise RuntimeError("复位共享对象的精确身份、端口、域或引用数不符合规则图")
    return evidence


def run_reset_stage(config, report, ctl, send_batch, prepare):
    reset = report.setdefault("planned_reset", {})
    reset.update(attempts=[], preserve_scene=False)
    for mode in config["planned_reset"].get("modes", ["count"]):
        if not run_reset_mode(config, report, ctl, send_batch, prepare, mode):
            return
    mark(report, RESET_STAGE, "passed", "仅本轮指定模式取得真实 reset 阶段计数、版本/快照保持和复位后暂停 CPU 的外部流量证据",
         ["reset-*/", "ctl-*.json"])


def run_reset_mode(config, report, ctl, send_batch, prepare, mode):
    owned, shared_generation, shared_created = {}, None, False
    reset = report["planned_reset"]
    record = {"mode": mode, "status": "blocked", "reset_requested": False}
    reset["attempts"].append(record)
    ports, rid = config["ports"], config["rule_id"]
    graph = [rid, ports["install"]["port_id"], ports["source"]["port_id"], ports["destination"]["port_id"],
             config["template"], "bidir" if mode == "shared_count" else "oneway"]
    modifiers = ["count"]
    try:
        initial, _, _, _ = prepare(mode + "-initial")
        check_running_device(initial, config)
        if mode == "shared_count":
            created = ctl("shared-count-create", rid, ports["install"]["port_id"], "transfer", required=False)
            if created["returncode"]:
                mark(report, RESET_STAGE, "rejected", created["stderr"])
                record["status"] = "rejected"
                return False
            shared_created = True
            shared_generation = fields(created["stdout"])["generation"]
            modifiers = [f"shared-count:{rid}:{shared_generation}"]
        if mode == "age":
            modifiers.append("age:6")
        modifiers.append("priority:" + str(config.get("priority", 10)))
        if not _create_graph(ctl, graph, modifiers, owned, report, RESET_STAGE):
            record["status"] = "rejected"
            return False
        generation = owned[rid]
        command = "shared-count-query" if mode == "shared_count" else "count"
        counter_generation = shared_generation if mode == "shared_count" else generation
        before_count = _count(ctl, command, rid, counter_generation)
        before_batch = send_batch("reset-" + mode + "-before", False)
        populated = _count(ctl, command, rid, counter_generation)
        _check_batch(before_batch, before_count, populated)
        if mode == "age":
            time.sleep(2)
            age = _age(ctl, rid, generation)
            if age.get("aged") != "no" or age.get("idle-seconds") == "unknown" or int(age["idle-seconds"]) < 2:
                raise Blocked("AGE 复位前没有可核对的实际空闲秒数")
            record["age_before"] = age
        before, identities, path, artifacts = prepare(mode + "-before-reset")
        check_running_device(before, config, len(owned), 1 if shared_generation is not None else 0)
        version = reset_rule_evidence(ctl, owned, rid if shared_generation is not None else None, shared_generation)
        snapshot = path.read_bytes()
        (artifacts / "snapshot-before.bin").write_bytes(snapshot)
        record.update(before=before, versions=version, snapshot_sha256=hashlib.sha256(snapshot).hexdigest())
        # 从请求发出起保守保留现场，只有完整恢复和实证均通过才解除该标记
        reset["preserve_scene"], record["reset_requested"] = True, True
        result = ctl("device-reset", before["sequence"], required=False, timeout=120)
        after = read_device_status(ctl)
        record["after"] = after
        if result["returncode"]:
            explicit = re.search(r"\(-(95|61)\)\s*$", result["stderr"]) is not None
            if explicit:
                mark(report, RESET_STAGE, "rejected", result["stderr"], ["ctl-*.json"])
                record["status"] = "rejected"
            # 只在明确的前置拒绝且完整状态/版本/计数/快照未变时允许清理已知对象
            unchanged = (after == before and path.read_bytes() == snapshot and
                         reset_rule_evidence(ctl, owned, rid if shared_generation is not None else None,
                                             shared_generation) == version and
                         _count(ctl, command, rid, counter_generation) == populated)
            if explicit and unchanged:
                reset["preserve_scene"] = False
                return False
            raise RuntimeError("物理复位失败或已部分改变设备，保留隔离现场并禁止自动重试/旧句柄清理")
        check_running_device(after, config, len(owned), 1 if shared_generation is not None else 0)
        expected_mask = (1 << len(before["device_ports"])) - 1
        if (any(after[key] != before[key] + 1 for key in ("attempts", "resets", "recoveries")) or
                any(after[key] != expected_mask for key in ("stopped", "reset", "configured", "started")) or
                after["phase"] != 6 or after["sequence"] < before["sequence"]):
            raise RuntimeError("CLI 虽然成功，仍缺完整端口组 stop/reset/configure/start 与恢复阶段证据")
        if runtime_port_identities(config, ctl) != identities:
            raise RuntimeError("复位后端口、交换域、角色、固件或设备身份发生变化")
        observed = reset_rule_evidence(ctl, owned, rid if shared_generation is not None else None, shared_generation)
        restored_snapshot = path.read_bytes()
        (artifacts / "snapshot-after.bin").write_bytes(restored_snapshot)
        if observed != version or restored_snapshot != snapshot:
            raise RuntimeError("复位改变了规则/共享对象版本、共享 revision 或持久化快照")
        reset_count = _count(ctl, command, rid, counter_generation)
        if reset_count != {"hits": 0, "bytes": 0}:
            raise RuntimeError("复位后硬件 COUNT 没有从零开始")
        if mode == "age":
            age = _age(ctl, rid, generation)
            if age.get("aged") != "no" or age.get("idle-seconds") not in ("0", "1"):
                raise RuntimeError("复位后 AGE 没有重新开始空闲计时")
            record["age_after"] = age
        if fields(ctl("health")["stdout"]).get("ready") != "yes":
            raise RuntimeError("复位后真实 worker 尚未 READY")
        batch = send_batch("reset-" + mode + "-after", False)
        record["packet_evidence"] = _check_batch(batch, reset_count, _count(ctl, command, rid, counter_generation))
        if mode == "shared_count":
            before_reverse = _count(ctl, command, rid, counter_generation)
            reverse = send_batch("reset-shared_count-after-reverse", True)
            record["reverse_packet_evidence"] = _check_batch(reverse, before_reverse,
                                                             _count(ctl, command, rid, counter_generation))
        if mode == "age":
            started, deadline = time.monotonic(), time.monotonic() + 12
            while True:
                current = ctl("get", rid, required=False)
                if not_found(current):
                    if time.monotonic() - started < 5:
                        raise RuntimeError("复位重建的 AGE 沿用了旧空闲时间并提前删除")
                    del owned[rid]
                    break
                if (current["returncode"] or rule_identity(current["stdout"], rid).get("generation") != str(generation) or
                        time.monotonic() >= deadline):
                    raise RuntimeError("复位后 AGE 到期行为或精确代次不符合预期")
                time.sleep(0.1)
        record["status"], reset["preserve_scene"] = "passed", False
        return True
    except BaseException as error:
        record["error"] = str(error)
        record["status"] = "failed"
        if reset["preserve_scene"] and isinstance(error, Blocked):
            raise RuntimeError("复位发起后身份或恢复状态校验失败，保留现场: " + str(error)) from error
        raise
    finally:
        if reset["preserve_scene"]:
            record["cleanup"] = "preserved_unknown_handles_no_delete_or_acknowledge"
        else:
            errors = _cleanup_rules(ctl, owned)
            if shared_created:
                try:
                    if shared_generation is None:
                        current = ctl("shared-count-get", rid, "any")
                        shared_generation = fields(current["stdout"])["generation"]
                    removed = ctl("shared-count-delete", rid, shared_generation, required=False)
                    if removed["returncode"] or fields(removed["stdout"]).get("shared-count") != "deleted":
                        errors.append("复位阶段共享对象精确清理失败")
                except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
                    errors.append(str(error))
            record["cleanup_errors"] = errors
            if errors:
                raise RuntimeError("复位阶段清理失败: " + str(errors))


def run_age_stage(config, report, ctl, send_batch):
    owned = {}
    ports, rid = config["ports"], config["rule_id"]
    graph = [rid, ports["install"]["port_id"], ports["source"]["port_id"], ports["destination"]["port_id"],
             config["template"], "oneway"]
    try:
        if not _create_graph(ctl, graph, ["count", "age:3", "priority:" + str(config.get("priority", 10))],
                             owned, report, "age"):
            return
        generation = owned[rid]
        age = ctl("rule-age", rid, generation, required=False)
        if age["returncode"]:
            mark(report, "age", "rejected", age["stderr"])
            return
        before = _count(ctl, "count", rid, generation)
        first = send_batch("age-first-hit", False)
        _check_batch(first, before, _count(ctl, "count", rid, generation))
        time.sleep(1)
        if ctl("get", rid, required=False)["returncode"]:
            raise RuntimeError("AGE 规则在配置超时前消失")
        before = _count(ctl, "count", rid, generation)
        renewed = send_batch("age-renew-hit", False)
        _check_batch(renewed, before, _count(ctl, "count", rid, generation))
        started = time.monotonic()
        deadline = started + 9
        while True:
            current = ctl("get", rid, required=False)
            if not_found(current):
                # 驱动秒级计时有粒度差异，仍须观察到续期后至少两秒没有提前删除
                if time.monotonic() - started < 2:
                    raise RuntimeError("AGE 命中续期后过早删除")
                break
            if current["returncode"] or rule_identity(current["stdout"], rid).get("generation") != generation:
                raise RuntimeError("AGE 规则查询失败或被其他请求替换")
            if time.monotonic() >= deadline:
                raise RuntimeError("AGE 规则闲置后没有自动从正式账本删除")
            time.sleep(0.1)
        if not not_found(ctl("count", rid, generation, required=False)):
            raise RuntimeError("AGE 删除后 COUNT 仍可查询旧规则")
        del owned[rid]
        mark(report, "age", "passed", "真实 AGE 查询、两轮硬件命中续期、闲置删除与旧版本 COUNT 不存在均已观察",
             ["age-first-hit/", "age-renew-hit/", "ctl-*.json"])
    finally:
        errors = _cleanup_rules(ctl, owned)
        report["age_cleanup"] = {"rule_cleanup_errors": errors}
        if errors:
            raise RuntimeError("AGE 阶段清理失败: " + str(errors))


def run_shared_stage(config, report, ctl, send_batch):
    owned, shared_generation, shared_created = {}, None, False
    ports, rid = config["ports"], config["rule_id"]
    try:
        if not not_found(ctl("get", rid + 1, required=False)):
            raise Blocked("共享阶段第二条专用规则 ID 未确认空闲")
        if not not_found(ctl("shared-count-get", rid, "any", required=False)):
            raise Blocked("共享阶段对象 ID 未确认空闲")
        created = ctl("shared-count-create", rid, ports["install"]["port_id"], "transfer", required=False)
        if created["returncode"]:
            mark(report, "shared_count", "rejected", created["stderr"])
            return
        shared_created = True
        shared_generation = fields(created["stdout"])["generation"]
        graph = [rid, ports["install"]["port_id"], ports["source"]["port_id"], ports["destination"]["port_id"],
                 config["template"], "bidir"]
        if not _create_graph(ctl, graph, [f"shared-count:{rid}:{shared_generation}",
                                         "priority:" + str(config.get("priority", 10))], owned, report, "shared_count"):
            return
        references = fields(ctl("shared-count-get", rid, shared_generation)["stdout"])
        if references.get("references") != "2":
            raise RuntimeError("双向共享对象引用数不是二")
        refused = ctl("shared-count-delete", rid, shared_generation, required=False)
        if refused["returncode"] == 0 or not re.search(r"\(-16\)\s*$", refused["stderr"]):
            raise RuntimeError("在用共享对象没有被 EBUSY 拒绝删除")
        for tag, reverse in (("shared-forward", False), ("shared-reverse", True)):
            before = _count(ctl, "shared-count-query", rid, shared_generation)
            batch = send_batch(tag, reverse)
            _check_batch(batch, before, _count(ctl, "shared-count-query", rid, shared_generation))
        removed = ctl("delete", rid + 1, owned[rid + 1])
        if fields(removed["stdout"]).get("deleted") != "yes":
            raise RuntimeError("共享反向规则没有被精确删除")
        del owned[rid + 1]
        if fields(ctl("shared-count-get", rid, shared_generation)["stdout"]).get("references") != "1":
            raise RuntimeError("删除一条共享规则后引用没有减到一")
        before = _count(ctl, "shared-count-query", rid, shared_generation)
        remaining = send_batch("shared-after-one-delete", False)
        _check_batch(remaining, before, _count(ctl, "shared-count-query", rid, shared_generation))
        errors = _cleanup_rules(ctl, owned)
        if errors:
            raise RuntimeError("共享规则清理失败: " + str(errors))
        if fields(ctl("shared-count-get", rid, shared_generation)["stdout"]).get("references") != "0":
            raise RuntimeError("共享规则清空后引用没有归零")
        destroyed = ctl("shared-count-delete", rid, shared_generation)
        if fields(destroyed["stdout"]).get("shared-count") != "deleted":
            raise RuntimeError("共享对象没有被精确销毁")
        shared_generation = None
        shared_created = False
        mark(report, "shared_count", "passed", "双向硬件汇总、在用删除拒绝、删一边继续累计与精确销毁均已观察",
             ["shared-forward/", "shared-reverse/", "shared-after-one-delete/", "ctl-*.json"])
    finally:
        errors = _cleanup_rules(ctl, owned)
        if shared_created:
            try:
                if shared_generation is None:
                    current = ctl("shared-count-get", rid, "any")
                    shared_generation = fields(current["stdout"])["generation"]
                removed = ctl("shared-count-delete", rid, shared_generation, required=False)
                if removed["returncode"] or fields(removed["stdout"]).get("shared-count") != "deleted":
                    errors.append("共享对象精确清理失败: " + removed["stderr"])
            except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
                errors.append(str(error))
        report["shared_cleanup"] = {"cleanup_errors": errors}
        if errors:
            raise RuntimeError("共享阶段清理失败: " + str(errors))


def live_run(root, build, destination, config, report):
    require_integer(config.get("daemon_pid"), 2, (1 << 31) - 1, "daemon_pid")
    socket_path = Path(config.get("socket", ""))
    if not socket_path.is_absolute():
        raise Blocked("需要绝对路径的管理 socket")
    compiled = execute(["meson", "compile", "-C", str(build)], destination / "compile.json", 120)
    if compiled["returncode"] != 0:
        raise RuntimeError("严格构建失败")
    report["binding"] = build_binding(root, build)
    identity = process_identity(config["daemon_pid"], build, socket_path)
    report["daemon"] = identity
    if any(item not in identity["cmdline"] for item in config["eal_devargs"]):
        raise Blocked("指定 EAL devargs 未在实际 daemon 命令行观察到")
    if any(item == "--no-pci" or item.startswith("--vdev") for item in identity["cmdline"]):
        raise Blocked("实际 daemon 使用了虚拟设备或禁用 PCI")
    sequence = 0

    def ctl(*arguments, required=True, timeout=8):
        nonlocal sequence
        sequence += 1
        record = execute([str(build / "dppctl"), "--socket", str(socket_path)] + [str(item) for item in arguments],
                         destination / f"ctl-{sequence:03d}.json", timeout)
        if required and record["returncode"]:
            raise RuntimeError("管理命令失败: " + str(arguments) + " " + record["stderr"])
        return record

    runtime_port_identities(config, ctl)

    def prepare_reset(tag):
        stage = destination / ("reset-" + tag)
        stage.mkdir()
        current = process_identity(identity["pid"], build, socket_path)
        if current["start_time"] != identity["start_time"]:
            raise Blocked("物理复位前 daemon 已被替换")
        status, ports, snapshot = reset_preflight(config, report, ctl, current, stage)
        return status, ports, snapshot, stage

    if "planned_reset" in config.get("stages", []):
        initial, _, _, _ = prepare_reset("dedicated-daemon")
        check_running_device(initial, config)
    request = {"operation": "inspect", "run_id": report["run_id"],
               "tx_interface": config["peer"]["tx_interface"], "rx_interface": config["peer"]["rx_interface"]}
    external = peer(config, request, destination)
    local_id = hashlib.sha256(Path("/etc/machine-id").read_bytes()).hexdigest()
    if external["inventory"]["machine_id"] == local_id:
        raise Blocked("对端与 DUT 是同一个系统，不能作为外部实机证据")
    report["peer_inventory"] = external["inventory"]
    mark(report, "discovery", "passed", "独立 PCI、实际 PMD/固件/交换域、管理保护和外部端点均已核对",
         ["inventory.json", "peer-inspect-command.json"])
    rid, template = config["rule_id"], config["template"]
    existing = ctl("get", rid, required=False)
    if not not_found(existing):
        raise Blocked("专用规则 ID 已存在或无法确认不存在")
    ports = config["ports"]
    graph = [rid, ports["install"]["port_id"], ports["source"]["port_id"], ports["destination"]["port_id"],
             template, "oneway"]
    plan = ctl("transfer-plan", *graph, "count", "priority:" + str(config.get("priority", 10)), required=False)
    if plan["returncode"]:
        mark(report, "transfer_" + template, "rejected", plan["stderr"], [f"ctl-{sequence:03d}.json"])
        raise Blocked("真实拓扑/能力明确拒绝此模板，不能记为硬件通过")
    generation = fields(plan["stdout"])["repository-generation"]
    created = False
    stop_attempted = False
    pidfd = None
    installed_generation = None
    try:
        pidfd = pin_process(identity, build, socket_path)
        apply = ctl("transfer-apply", *graph, generation, "count", "priority:" + str(config.get("priority", 10)),
                    required=False)
        if apply["returncode"]:
            mark(report, "transfer_" + template, "rejected", apply["stderr"], [f"ctl-{sequence:03d}.json"])
            raise Blocked("真实 PMD 拒绝安装，拒绝记录不等于硬件正向验收")
        created = True
        installed_generation = fields(apply["stdout"])["generation"]
        status = fields(ctl("rule-status", rid, installed_generation)["stdout"])
        if status.get("backend") != "rte_flow":
            raise RuntimeError("规则的实际后端不是硬件 rte_flow")
        before = {key: int(number) for key, number in fields(ctl("count", rid, installed_generation)["stdout"]).items()
                  if key in ("hits", "bytes")}
        workers_before = {key: int(number) for key, number in fields(ctl("stats")["stdout"]).items()}
        if fields(ctl("health")["stdout"]).get("ready") != "yes":
            raise RuntimeError("复位前管理/worker 不就绪")
        # 使用本轮不可复用 nonce，旧 PCAP 或离线文件不能冒充这次收发
        frames = []
        peer_ports = {item["interface"]: item for item in external["inventory"]["interfaces"]}
        rx_mac = bytes.fromhex(peer_ports[config["peer"]["rx_interface"]]["mac"].replace(":", ""))
        tx_mac = bytes.fromhex(peer_ports[config["peer"]["tx_interface"]]["mac"].replace(":", ""))
        if len(rx_mac) != 6 or len(tx_mac) != 6:
            raise Blocked("对端真实接口 MAC 不完整")
        for index in range(2):
            payload = ("DPPD-M3-HW:" + report["run_id"] + ":" + str(index)).encode()
            frame = rx_mac + tx_mac + b"\x88\xb5" + payload
            frames.append(frame.ljust(96 + index * 32, b"\x5a"))
        (destination / "frame-spec.json").write_text(json.dumps({"run_id": report["run_id"],
            "frames_hex": [frame.hex() for frame in frames], "packets_per_frame": 8}, indent=2))
        pid = identity["pid"]
        if process_identity(pid, build, socket_path)["start_time"] != identity["start_time"]:
            raise RuntimeError("daemon PID 已被复用")
        stop_attempted = True
        signal.pidfd_send_signal(pidfd, signal.SIGSTOP, None, 0)
        stopped_threads = wait_stopped(pid)
        (destination / "paused-tasks.json").write_text(json.dumps(stopped_threads, indent=2))
        request.update(operation="traffic", frames_hex=[frame.hex() for frame in frames], packets_per_frame=8,
                       duration_seconds=2)
        traffic = peer(config, request, destination)
        (destination / "paused-tasks-after.json").write_text(json.dumps(all_tasks_stopped(pid), indent=2))
        if (traffic.get("run_id") != report["run_id"] or traffic.get("simulation") is not False or
                traffic.get("capture_origin") != "live_AF_PACKET" or
                not same_peer(traffic["inventory"], external["inventory"])):
            raise RuntimeError("对端本轮身份或实时收发来源变化")
        tx = base64.b64decode(traffic["tx_pcap_base64"], validate=True)
        rx = base64.b64decode(traffic["rx_pcap_base64"], validate=True)
        (destination / "external-tx.pcap").write_bytes(tx)
        (destination / "external-rx.pcap").write_bytes(rx)
        signal.pidfd_send_signal(pidfd, signal.SIGCONT, None, 0)
        stop_attempted = False
        if process_identity(pid, build, socket_path)["start_time"] != identity["start_time"]:
            raise RuntimeError("恢复后 daemon 身份变化")
        after = {key: int(number) for key, number in fields(ctl("count", rid, installed_generation)["stdout"]).items()
                 if key in ("hits", "bytes")}
        time.sleep(0.05)
        workers_after = {key: int(number) for key, number in fields(ctl("stats")["stdout"]).items()}
        report["packet_evidence"] = verify_packet_evidence(tx, rx, frames, 8, before, after,
                                                          workers_before, workers_after)
        for stage in ("transfer_" + template, "count", "cpu_bypass"):
            mark(report, stage, "passed", "本轮外部字节/包数、硬件 COUNT 与全部线程暂停期间的真实转发一致",
                 ["external-tx.pcap", "external-rx.pcap", "paused-tasks.json"])
        family = "mlx5_hardware" if "mlx5" in ports["install"]["pmd"].lower() else "non_mlx5_hardware"
        mark(report, family, "passed", "只代表本设备本轮已列出的基础阶段，不能推断其余硬件或动作支持",
             ["external-rx.pcap", "paused-tasks.json"])
        report["status"] = "partial"
        report["limitations"].append("能力仅覆盖本轮已记录的阶段和模板，未执行的模板与真实移除仍缺实机证据")
        stages = config.get("stages", ["basic", "age", "shared_count"])
        if any(stage in stages for stage in ("age", "shared_count", "planned_reset")):
            removed = ctl("delete", rid, installed_generation)
            if fields(removed["stdout"]).get("deleted") != "yes":
                raise RuntimeError("基础规则没有被精确删除，不能开始其他可能重叠的阶段")
            created = False

            def send_batch(tag, reverse):
                nonlocal stop_attempted
                stage = destination / tag
                stage.mkdir()
                source = config["peer"]["rx_interface" if reverse else "tx_interface"]
                target = config["peer"]["tx_interface" if reverse else "rx_interface"]
                src_mac = bytes.fromhex(peer_ports[source]["mac"].replace(":", ""))
                dst_mac = bytes.fromhex(peer_ports[target]["mac"].replace(":", ""))
                stage_frames = [(dst_mac + src_mac + b"\x88\xb5" +
                                 ("DPPD-M3-HW:" + report["run_id"] + ":" + tag + ":" + str(index)).encode()).ljust(
                                     96 + index * 32, b"\x5a") for index in range(2)]
                request = {"operation": "traffic", "run_id": report["run_id"], "tx_interface": source,
                           "rx_interface": target, "frames_hex": [frame.hex() for frame in stage_frames],
                           "packets_per_frame": 8, "duration_seconds": 0.2}
                (stage / "frame-spec.json").write_text(json.dumps(request, indent=2))
                workers_before = {key: int(number) for key, number in fields(ctl("stats")["stdout"]).items()}
                if process_identity(pid, build, socket_path)["start_time"] != identity["start_time"]:
                    raise RuntimeError("生命周期报文阶段前 daemon 身份变化")
                stop_attempted = True
                try:
                    signal.pidfd_send_signal(pidfd, signal.SIGSTOP, None, 0)
                    (stage / "paused-tasks.json").write_text(json.dumps(wait_stopped(pid), indent=2))
                    traffic = peer(config, request, stage)
                    (stage / "paused-tasks-after.json").write_text(json.dumps(all_tasks_stopped(pid), indent=2))
                finally:
                    signal.pidfd_send_signal(pidfd, signal.SIGCONT, None, 0)
                    stop_attempted = False
                if process_identity(pid, build, socket_path)["start_time"] != identity["start_time"]:
                    raise RuntimeError("生命周期报文阶段后 daemon 身份变化")
                if (traffic.get("run_id") != report["run_id"] or traffic.get("simulation") is not False or
                        traffic.get("capture_origin") != "live_AF_PACKET" or
                        not same_peer(traffic["inventory"], external["inventory"])):
                    raise RuntimeError("生命周期阶段对端身份或实时来源变化")
                tx = base64.b64decode(traffic["tx_pcap_base64"], validate=True)
                rx = base64.b64decode(traffic["rx_pcap_base64"], validate=True)
                (stage / "external-tx.pcap").write_bytes(tx)
                (stage / "external-rx.pcap").write_bytes(rx)
                time.sleep(0.05)
                workers_after = {key: int(number) for key, number in fields(ctl("stats")["stdout"]).items()}
                return {"tx": tx, "rx": rx, "frames": stage_frames, "packets": 8,
                        "workers_before": workers_before, "workers_after": workers_after}

            if "age" in stages:
                run_age_stage(config, report, ctl, send_batch)
            if "shared_count" in stages:
                run_shared_stage(config, report, ctl, send_batch)
            if "planned_reset" in stages:
                run_reset_stage(config, report, ctl, send_batch, prepare_reset)
    except BaseException as error:
        report["stage_error"] = str(error)
        raise
    finally:
        cleanup = {"daemon_resumed": not stop_attempted, "rule_deleted": not created, "health_ready": False,
                   "scene_preserved": report.get("planned_reset", {}).get("preserve_scene", False), "errors": []}
        if stop_attempted:
            try:
                signal.pidfd_send_signal(pidfd, signal.SIGCONT, None, 0)
                cleanup["daemon_resumed"] = True
            except (OSError, ValueError, RuntimeError) as error:
                cleanup["errors"].append("resume: " + str(error))
        if created:
            try:
                if process_identity(identity["pid"], build, socket_path)["start_time"] != identity["start_time"]:
                    raise RuntimeError("旧 daemon 已消失，不对替代进程删除规则")
                if installed_generation is None:
                    # 已确认本轮创建成功时重新读取精确版本，仍不能使用 any 进行清理
                    installed_generation = rule_identity(ctl("get", rid)["stdout"], rid).get("generation")
                    if installed_generation is None:
                        raise RuntimeError("规则创建响应缺少精确版本，无法安全清理")
                result = ctl("delete", rid, installed_generation, required=False)
                cleanup["rule_deleted"] = result["returncode"] == 0 and fields(result["stdout"]).get("deleted") == "yes"
            except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
                cleanup["errors"].append("delete: " + str(error))
        try:
            result = ctl("health", required=False)
            cleanup["health_ready"] = result["returncode"] == 0 and fields(result["stdout"]).get("ready") == "yes"
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
            cleanup["errors"].append("health: " + str(error))
        if pidfd is not None:
            try:
                os.close(pidfd)
            except OSError as error:
                cleanup["errors"].append("pidfd_close: " + str(error))
        report["cleanup"] = cleanup
        if (cleanup["errors"] or cleanup["scene_preserved"] or
                not all(cleanup[key] for key in ("daemon_resumed", "rule_deleted", "health_ready"))):
            raise RuntimeError("硬件阶段清理未完成，见 cleanup 与原始管理日志")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--report-dir", type=Path, default=Path("build/m3-hardware-acceptance"))
    parser.add_argument("--config", type=Path)
    parser.add_argument("--execute", action="store_true", help="预检全部通过后才安装专用规则并暂停指定测试 daemon")
    args = parser.parse_args(argv)
    root = Path(__file__).resolve().parents[1]
    destination, build = args.report_dir.resolve(), args.build_dir.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    report = {"schema_version": 1, "run_id": uuid.uuid4().hex, "scope": "physical_m3_hardware",
              "status": "blocked", "success": False, "complete_m3_validation": False,
              "hardware_acceptance": "not_run", "started_utc": datetime.now(timezone.utc).isoformat(),
              "capability_matrix": matrix(), "blockers": [], "limitations": [], "cleanup": {}}
    # 每轮证据使用新目录，旧 PCAP、命令日志或成功标记不能混入本轮报告
    artifacts = destination / report["run_id"]
    artifacts.mkdir()
    report["artifact_directory"] = str(artifacts)
    report["tool_sha256"] = {name: hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest()
                             for name in ("verify_m3_hardware.py", "m3_hardware_peer.py")}
    report["in_progress"], report["finished_utc"] = True, None
    (destination / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    previous_handlers = {}
    if args.execute and sys.platform == "linux":
        def interrupted(number, frame):
            del frame
            raise InterruptedError("硬件验收被信号中断: " + str(number))
        for number in (signal.SIGTERM, signal.SIGINT):
            previous_handlers[number] = signal.signal(number, interrupted)
    try:
        inventory = inspect_environment()
        (artifacts / "inventory.json").write_text(json.dumps(inventory, ensure_ascii=False, indent=2), encoding="utf-8")
        if not any(device["classification"]["kind"] == "physical_candidate"
                   for device in inventory["pci_scan"]["network_devices"]):
            raise Blocked("本机只观察到虚拟/未知网络设备，没有可用于实机验收的 PCI 候选")
        report["binding"] = build_binding(root, build)
        if args.config is None:
            raise Blocked("缺少明确的测试端口、独立管理口、外部 peer 和物理接线配置")
        config = json.loads(args.config.read_text(encoding="utf-8"))
        (artifacts / "config.json").write_text(json.dumps(config, ensure_ascii=False, indent=2), encoding="utf-8")
        report["config_sha256"] = canonical_digest(config)
        report["physical_links_declared"] = config.get("physical_links", [])
        report["protected_management_pci"] = hardware_gate(inventory, config)
        if not args.execute:
            raise Blocked("只读预检完成，尚未执行本轮真实报文/CPU 旁路，不能记为硬件通过")
        if sys.platform != "linux":
            raise Blocked("实际收发只支持 Linux")
        live_run(root, build, artifacts, config, report)
        if source_digest(root) != report["binding"]["source_sha256"]:
            raise RuntimeError("验收期间源码变化")
    except Blocked as error:
        report["blockers"].append(str(error))
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, subprocess.SubprocessError) as error:
        report["status"] = "failed"
        report["error"] = str(error)
    finally:
        report["in_progress"] = False
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        report["hardware_acceptance"] = "partial" if report["status"] == "partial" else "not_run"
        # 真实移除仍需人工接线操作，单机部分证据也不能满足两类物理硬件要求
        report["limitations"].append("真实设备移除没有自动接线 runner，单机本轮报告不能代表完整 M3 已完成")
        report["artifact_sha256"] = {path.relative_to(artifacts).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                                     for path in sorted(artifacts.rglob("*")) if path.is_file()}
        (destination / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        for number, handler in previous_handlers.items():
            signal.signal(number, handler)
    print(f"{report['status'].upper()} REPORT {destination / 'report.json'}")
    return {"failed": 1, "blocked": 2, "partial": 3}[report["status"]]


if __name__ == "__main__":
    raise SystemExit(main())
