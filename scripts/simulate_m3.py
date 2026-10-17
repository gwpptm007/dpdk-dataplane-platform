#!/usr/bin/env python3
"""不用实体网卡演练 M3 的规则、报文、计数和回滚，结果明确标记为功能模拟"""

import argparse
from datetime import datetime, timezone
import errno
import json
import os
from pathlib import Path
import shlex
import signal
import socket
import subprocess
import sys
import tempfile
import time

from verify_m2 import source_digest


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def fields(text):
    return dict(token.split("=", 1) for token in text.split() if "=" in token)


def exchange(path, request):
    """独立模拟通道只交换文本，不改变正式管理协议"""
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as connection:
        connection.settimeout(5)
        connection.connect(str(path))
        connection.sendall(request if isinstance(request, bytes) else request.encode("ascii"))
        response = connection.recv(8193)
    check(len(response) <= 8192, "模拟响应过长")
    return response.decode("ascii").strip()


def frame(length=64):
    check(14 <= length <= 2048, "演练帧长度应在 14 到 2048 字节之间")
    return bytes.fromhex("02000000000202000000000188b5") + bytes(i % 256 for i in range(length - 14))


class Fixture:
    """只管理自己创建的进程和私有目录，所有规则操作都经过真实 dppctl"""

    def __init__(self, build, directory, log, persistent=False):
        self.build, self.directory, self.log = build, directory, log
        self.control, self.io = directory / "ctl.sock", directory / "io.sock"
        self.persistent, self.process = persistent, None
        self.environment = {key: value for key, value in os.environ.items()
                            if key != "LD_PRELOAD" and not key.startswith("DPPD_TEST_")}

    def command(self):
        command = [str(self.build / "tests/integration/dppd_transfer_sim"),
                   "--socket", str(self.control), "--io-socket", str(self.io)]
        if self.persistent:
            command += ["--state-path", str(self.directory / "simulation-state.bin")]
        return command

    def start(self):
        check(self.process is None, "上一次模拟进程尚未结束")
        with self.log.open("a") as output:
            # 不另开会话，统一验收超时时能连同这个子进程一起清理
            self.process = subprocess.Popen(self.command(), stdout=output, stderr=subprocess.STDOUT,
                                            env=self.environment)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            check(self.process.poll() is None, "模拟进程提前退出：" + self.log.read_text())
            if self.control.exists() and self.io.exists():
                self.ctl("ping")
                return
            time.sleep(0.02)
        raise RuntimeError("等待模拟接口超时：" + self.log.read_text())

    def stop(self):
        if self.process is None:
            return
        process, self.process = self.process, None
        if process.poll() is None:
            process.terminate()
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
            raise RuntimeError("模拟进程未能正常结束：" + self.log.read_text())
        check(process.returncode == 0, "模拟进程退出失败：" + self.log.read_text())
        check(not self.control.exists() and not self.io.exists(), "模拟接口没有清理")

    def ctl(self, *arguments, error=None):
        result = subprocess.run([str(self.build / "dppctl"), "--socket", str(self.control),
                                 *map(str, arguments)], env=self.environment, capture_output=True,
                                text=True, timeout=5)
        check(result.returncode == (0 if error is None else 1), result.stdout + result.stderr)
        if error is not None:
            check(f"({-error})" in result.stderr, result.stdout + result.stderr)
        return result.stdout

    def io_request(self, text, error=None):
        response = exchange(self.io, text)
        if error is None:
            check(not response.startswith("error "), response)
        else:
            check(response.startswith("error ") and fields(response).get("code") == str(error), response)
        return response

    def status(self):
        return fields(self.io_request("status"))

    def install(self, rule_id, source, destination, template="pair", bidirectional=False, priority=10, error=None):
        direction = "bidir" if bidirectional else "oneway"
        plan = self.ctl("transfer-plan", rule_id, 0, source, destination, template, direction,
                        "count", f"priority:{priority}")
        generation = fields(plan.splitlines()[0])["repository-generation"]
        result = self.ctl("transfer-apply", rule_id, 0, source, destination, template, direction, generation,
                          "count", f"priority:{priority}", error=error)
        return [fields(line) for line in result.splitlines() if line.startswith("edge=")]

    def delete(self, edges):
        arguments = [part for edge in edges for part in (edge["id"], edge["generation"])]
        self.ctl("delete-batch" if len(edges) == 2 else "delete", *arguments)

    def inject(self, source, destination, rule_id, packet):
        result = self.io_request(f"inject {source} {packet.hex()}")
        values = fields(result)
        check(result.startswith("forward ") and values.get("source") == str(source) and
              values.get("destination") == str(destination) and values.get("rule") == str(rule_id) and
              values.get("length") == str(len(packet)) and values.get("frame") == packet.hex(), result)

    def count(self, edge, packets, length):
        result = fields(self.ctl("count", edge["id"], edge["generation"]))
        check(result.get("hits") == str(packets) and result.get("bytes") == str(length), str(result))


def exercise(simulator, passed):
    """通过命令行和模拟报文共同核对行为，不把写死的计数当成报文证据"""
    for port, role in enumerate(("uplink", "pf", "vf", "vf", "sf", "sf", "unknown", "uplink")):
        identity = fields(simulator.ctl("port-show", port))
        check(identity.get("role") == role and identity.get("driver") == "simulation", str(identity))
    original = simulator.status()
    simulator.ctl("transfer-plan", 900, 0, 0, 7, "pair", "oneway", error=errno.EXDEV)
    simulator.ctl("transfer-plan", 900, 0, 6, 2, "uplink-vf", "oneway", error=errno.ENOTSUP)
    check(simulator.status() == original, "角色或交换域拒绝产生了副作用")
    templates = (("pair", 6, 2), ("uplink-vf", 0, 2), ("uplink-sf", 0, 4), ("pf-vf", 1, 2),
                 ("pf-sf", 1, 4), ("vf-vf", 2, 3), ("sf-sf", 4, 5))
    for index, (template, source, destination) in enumerate(templates):
        for bidirectional in (False, True):
            rule_id = 1000 + index * 10 + int(bidirectional) * 2
            edges = simulator.install(rule_id, source, destination, template, bidirectional)
            simulator.inject(source, destination, rule_id, frame())
            simulator.count(edges[0], 1, 64)
            if bidirectional:
                simulator.inject(destination, source, rule_id + 1, frame(73))
                simulator.count(edges[1], 1, 73)
                simulator.count(edges[0], 1, 64)
            else:
                check(simulator.io_request(f"inject {destination} {frame().hex()}").startswith("drop "),
                      "单向规则错误地允许反向报文")
            simulator.delete(edges)
            check(simulator.status()["installed"] == "0", "删除后仍有模拟句柄")
        passed(f"{template} 单向/双向转发、逐字节帧内容和独立 COUNT")

    low = simulator.install(2000, 0, 2, priority=20)
    high = simulator.install(2001, 0, 3, priority=5)
    simulator.inject(0, 3, 2001, frame())
    simulator.count(low[0], 0, 0)
    simulator.count(high[0], 1, 64)
    simulator.delete(high)
    for length in (14, 2048):
        simulator.inject(0, 2, 2000, frame(length))
    simulator.count(low[0], 2, 2062)
    snapshot = simulator.status()
    invalid = ("inject -1 aa", f"inject 999 {frame().hex()}", "inject 0 gg", "inject 0 abc",
               f"inject 0 {frame().hex()} extra", "inject 0 " + "00" * 13,
               "inject 0 " + "00" * 2049, b"status\x00suffix", b"x" * 8193)
    for request in invalid:
        check(exchange(simulator.io, request).startswith("error "), "坏输入没有拒绝")
        check(simulator.status() == snapshot, "坏输入改变了模拟状态")
    # 客户端不等响应就离开，也不能让服务被 SIGPIPE 终止
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as connection:
        connection.connect(str(simulator.io))
        connection.sendall(b"status")
    check(simulator.status() == snapshot, "客户端提前离开改变了模拟状态")
    simulator.io_request("fault query 1")
    simulator.ctl("count", low[0]["id"], low[0]["generation"], error=errno.EIO)
    simulator.count(low[0], 2, 2062)
    simulator.io_request("clear-faults")
    simulator.count(low[0], 2, 2062)
    simulator.delete(low)
    passed("优先级切换、14/2048 字节边界、坏输入无副作用和查询故障不清计数")

    for operation, error in (("validate", errno.ENOTSUP), ("create", errno.ENOSPC)):
        before = simulator.status()
        simulator.io_request(f"fault {operation} 2")
        simulator.install(3000, 0, 2, bidirectional=True, error=error)
        state = simulator.status()
        check(state["installed"] == "0" and state["desired"] == "0", str(state))
        expected_creates, expected_removes = (0, 0) if operation == "validate" else (2, 1)
        check(int(state["creates"]) - int(before["creates"]) == expected_creates and
              int(state["removes"]) - int(before["removes"]) == expected_removes, str(state))
    passed("整批验证失败不创建、中途创建失败回滚且账本不发布")

    old = simulator.install(4000, 0, 2)
    simulator.inject(0, 2, 4000, frame())
    simulator.io_request("fault create 2")
    simulator.io_request("fault remove 1")
    simulator.install(5000, 4, 5, bidirectional=True, error=errno.EUCLEAN)
    state = simulator.status()
    check(int(state["installed"]) > int(state["desired"]) and state["desired"] == "1" and
          state["recovery"] != "0", str(state))
    simulator.io_request(f"inject 0 {frame().hex()}", error=errno.EUCLEAN)
    simulator.ctl("reconcile-retry")
    check(simulator.status()["installed"] == "0", "恢复重试没有清理句柄")
    simulator.io_request(f"inject 0 {frame().hex()}", error=errno.EUCLEAN)
    simulator.stop()
    simulator.start()
    check(simulator.status()["installed"] == "1", "重启没有恢复原有规则")
    simulator.count(old[0], 0, 0)
    simulator.inject(0, 2, 4000, frame())
    simulator.count(old[0], 1, 64)
    simulator.delete(old)
    passed("补偿失败隔离全部转发、重试后仍需重启、正式快照重放且 COUNT 归零")


def demo(args):
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    destination = args.report_dir.resolve() if args.report_dir else build / "m3-simulation"
    destination.mkdir(parents=True, exist_ok=True)
    report_path = destination / "report.json"
    report = {"started_utc": datetime.now(timezone.utc).isoformat(), "simulation": True,
              "hardware_acceptance": "not_run", "complete_m3_validation": False,
              "packet_engine": "test_fixture_flat_transfer", "source_sha256": source_digest(root),
              "passed": [], "success": False}
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")

    def passed(message):
        report["passed"].append(message)
        print("PASS [功能模拟] " + message, flush=True)

    try:
        metadata = json.loads((build / "meson-info/meson-info.json").read_text())
        check(Path(metadata["directories"]["source"]).resolve() == root,
              "构建目录对应另一份源码，不能为当前工作区出具模拟报告")
        for binary in (build / "dppctl", build / "tests/integration/dppd_transfer_sim"):
            check(binary.is_file(), "缺少测试工具，请先开启 tests 并构建：" + str(binary))
        with tempfile.TemporaryDirectory(prefix="dppd-m3-sim-") as temporary:
            simulator = Fixture(build, Path(temporary), destination / "simulator.log", persistent=True)
            try:
                simulator.start()
                exercise(simulator, passed)
            finally:
                simulator.stop()
        check(source_digest(root) == report["source_sha256"], "演练期间源码变化，不能报告同版本通过")
        report["success"] = True
    except (RuntimeError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
        print("FAIL [功能模拟] " + str(error), file=sys.stderr, flush=True)
    finally:
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print("报告：" + str(report_path), flush=True)
    return 0 if report["success"] else 1


def serve(args):
    directory = args.directory.resolve()
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    check(directory.stat().st_uid == os.getuid() and directory.stat().st_mode & 0o777 == 0o700,
          "请选择自己拥有、权限为 0700 的独立模拟目录")
    simulator = Fixture(args.build_dir.resolve(), directory, directory / "simulator.log", persistent=True)

    def request_stop(_number, _context):
        # 单独终止前端时也走正常清理，不能留下仍在监听的模拟子进程
        raise KeyboardInterrupt

    previous = signal.signal(signal.SIGTERM, request_stop)
    try:
        simulator.start()
        print("功能模拟已启动，端口和报文都为模拟数据，硬件验收未执行", flush=True)
        print("管理接口：" + shlex.join([str(simulator.build / "dppctl"), "--socket", str(simulator.control)]), flush=True)
        print("报文接口：" + str(simulator.io) + "；按 Ctrl+C 结束，模拟规则保留在该目录", flush=True)
        simulator.process.wait()
    except KeyboardInterrupt:
        pass
    finally:
        try:
            simulator.stop()
        finally:
            signal.signal(signal.SIGTERM, previous)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    automatic = commands.add_parser("demo", help="自动演练七种模板、报文计数、回滚与重启")
    automatic.add_argument("--build-dir", type=Path, default=Path("build"))
    automatic.add_argument("--report-dir", type=Path)
    server = commands.add_parser("serve", help="启动可供真实 dppctl 操作的独立模拟服务")
    server.add_argument("--build-dir", type=Path, default=Path("build"))
    server.add_argument("--directory", type=Path, required=True)
    for name in ("inject", "status", "fault", "clear-faults"):
        client = commands.add_parser(name)
        client.add_argument("--io-socket", type=Path, required=True)
        if name == "inject":
            client.add_argument("--source", type=int, required=True)
            client.add_argument("--frame", required=True, help="完整以太网帧的十六进制文本")
        if name == "fault":
            client.add_argument("operation", choices=("validate", "create", "remove", "query"))
            client.add_argument("after", type=int, help="该操作第几次后续调用时失败一次")
    args = parser.parse_args()
    if args.command == "demo":
        return demo(args)
    try:
        if args.command == "serve":
            return serve(args)
        request = args.command
        if request == "inject":
            request += f" {args.source} {args.frame}"
        elif request == "fault":
            request += f" {args.operation} {args.after}"
        response = exchange(args.io_socket, request)
        print(response)
        return 1 if response.startswith("error ") else 0
    except (RuntimeError, OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
