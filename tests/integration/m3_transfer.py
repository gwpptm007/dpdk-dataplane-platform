#!/usr/bin/env python3
"""在正式 net_ring 进程上验收 M3 拒绝路径，不能计为真实硬件 transfer 验收"""

import argparse
import errno
from pathlib import Path
import socket
import struct
import subprocess
import tempfile

from batch_update import check, fields
from health_readiness import clean_environment, daemon_command
from recovery_isolation import stop, wait_for


def reject_old_protocol(build, directory, target, environment):
    """截取正式 CLI 请求并只改版本号，避免手写整个 C 结构体布局"""
    proxy = directory / "legacy.sock"
    client = None
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as listener:
            listener.bind(str(proxy))
            listener.listen(1)
            listener.settimeout(5)
            client = subprocess.Popen([str(build / "dppctl"), "--socket", str(proxy), "ping"],
                                      env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(5)
                request = bytearray(connection.recv(1024 * 1024))
                check(len(request) >= 16, "CLI request has no complete header")
                version, operation, size, request_id = struct.unpack_from("=HHIQ", request)
                check(version == 24 and operation == 1 and size == len(request), "unexpected current request header")
                # 保留真实请求长度和请求编号，只把版本退回 v23
                struct.pack_into("=H", request, 0, 23)
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as upstream:
                    upstream.settimeout(5)
                    upstream.connect(str(target))
                    upstream.sendall(request)
                    response = upstream.recv(1024 * 1024)
                check(len(response) >= 24, "old protocol response has no complete header")
                reply_version, reply_operation, reply_size, reply_id, status, _ = struct.unpack_from("=HHIQiI", response)
                check(reply_version == 24 and reply_operation == operation and reply_size == len(response) and
                      reply_id == request_id and status == -errno.EPROTO, "old protocol was not explicitly rejected")
                connection.sendall(response)
            stdout, stderr = client.communicate(timeout=5)
            check(client.returncode == 1 and f"({-errno.EPROTO})" in stderr and
                  "request failed" in stderr and not stdout, stdout + stderr)
    finally:
        if client is not None and client.poll() is None:
            client.kill()
            client.communicate(timeout=5)
        if proxy.exists():
            proxy.unlink()


def run_case(build):
    """只创建进程自己的 ring、socket 和快照，普通用户也能运行"""
    with tempfile.TemporaryDirectory(prefix="dppd-m3-transfer-") as temporary:
        directory = Path(temporary)
        (directory / "storage").mkdir()
        sock, state = directory / "ctl.sock", directory / "storage/state.bin"
        environment = clean_environment()
        command = daemon_command(build, directory, 1)
        software_id, transfer_id = 31000, 32000
        logs = []

        def ctl(*tokens, error=None, message=None):
            """业务拒绝必须带 errno 和可读原因，不能只靠非零退出码蒙混通过"""
            result = subprocess.run([str(build / "dppctl"), "--socket", str(sock), *map(str, tokens)],
                                    env=environment, capture_output=True, text=True, timeout=5)
            check(result.returncode == (0 if error is None else 1), result.stdout + result.stderr)
            if error is not None:
                check(f"({-error})" in result.stderr and "request failed" in result.stderr,
                      result.stdout + result.stderr)
            if message is not None:
                check("dppctl: transfer: " + message in result.stderr, result.stderr)
                check("transfer=created" not in result.stdout and "transfer=planned" not in result.stdout,
                      "rejected transfer printed a successful graph")
            return result.stdout

        def start(name):
            """每次重启保存独立日志，失败时能核对启动和缓冲池清理"""
            path = directory / name
            logs.append(path)
            with path.open("w") as output:
                process = subprocess.Popen(command, env=environment, stdout=output, stderr=subprocess.STDOUT)
            try:
                wait_for(process, sock.exists, "management socket")
                wait_for(process, lambda: fields(ctl("health"))["ready"] == "yes", "worker readiness")
            except Exception:
                stop(process)
                raise
            return process

        def check_ports():
            """虚拟端口没有交换域和可信角色，输出必须保留这个事实"""
            for port in (0, 1):
                raw = ctl("port-show", port)
                info = fields(raw)
                check(info["kind"] == "ethdev" and info["driver"] == "net_ring" and
                      info["switch-domain"] == "none", raw)
                check(info["role"] == "unknown" and info["role-source"] == "unknown" and
                      int(info["role-error"]) == -errno.ENOTSUP and info["ifindex"] == "0", raw)
                check(info["device"] == f"net_ring{port}" and "interface= " in raw and "phys-port-name=\n" in raw,
                      "virtual port fabricated physical identity: " + raw)
                check(ctl("port-show", port) == raw, "read-only port identity changed")
            ctl("port-show", 9, error=errno.ENOENT)

        def unchanged(generation, total, saved, count=None):
            """拒绝操作不能改变账本、COUNT、驱动校验次数或磁盘快照"""
            ping = fields(ctl("ping"))
            check(ping["version"] == "24" and ping["generation"] == str(generation) and
                  ping["rules"] == str(total), str(ping))
            health = fields(ctl("health"))
            check(health["live"] == "yes" and health["ready"] == "yes" and
                  health["current-generation"] == str(generation) and health["rules"] == str(total), str(health))
            page = fields(ctl("list").splitlines()[0])
            check(page["total"] == str(total) and page["repository-generation"] == str(generation), str(page))
            persistence = fields(ctl("persistence-status"))
            check(persistence["dirty"] == "no" and persistence["persisted-generation"] == str(generation),
                  str(persistence))
            for port in (0, 1):
                check(fields(ctl("capability-show", port))["validations"] == "0",
                      "topology rejection unexpectedly reached PMD validation")
            for rule_id in (transfer_id, transfer_id + 1):
                ctl("get", rule_id, error=errno.ENOENT)
                ctl("count", rule_id, "any", error=errno.ENOENT)
            if count is not None:
                check(ctl("count", software_id, 1) == count, "rejected transfer changed existing COUNT")
            check(state.read_bytes() == saved, "rejected transfer changed durable snapshot")

        def negative_round(generation, total, count=None):
            """覆盖单双向、COUNT、全部命名模板和重复请求，始终保持硬件验收未执行"""
            saved = state.read_bytes()
            for direction in ("oneway", "bidir"):
                for _ in range(3):
                    ctl("transfer-plan", transfer_id, 0, 0, 1, "pair", direction, "count", "priority:7",
                        error=errno.ENOTSUP, message="transfer-domain-missing")
                    ctl("transfer-apply", transfer_id, 0, 0, 1, "pair", direction, generation, "count", "priority:7",
                        error=errno.ENOTSUP, message="transfer-domain-missing")
                for scenario in ("uplink-vf", "uplink-sf", "pf-vf", "pf-sf", "vf-vf", "sf-sf"):
                    ctl("transfer-plan", transfer_id, 0, 0, 1, scenario, direction,
                        error=errno.ENOTSUP, message="template requires verified endpoint roles")
                    ctl("transfer-apply", transfer_id, 0, 0, 1, scenario, direction, generation,
                        error=errno.ENOTSUP, message="template requires verified endpoint roles")
            ctl("transfer-plan", transfer_id, 0, 0, 9, "pair", "oneway",
                error=errno.ENOENT, message="source or destination is absent from topology")
            ctl("transfer-apply", transfer_id, 9, 0, 1, "pair", "oneway", generation,
                error=errno.ENOENT, message="endpoint-not-found")
            ctl("transfer-apply", transfer_id, 0, 0, 1, "pair", "bidir", generation + 1,
                error=errno.ESTALE, message="repository changed; build a fresh transfer plan")
            reject_old_protocol(build, directory, sock, environment)
            unchanged(generation, total, saved, count)

        daemon = None
        try:
            daemon = start("initial.log")
            check_ports()
            negative_round(0, 0)
            ctl("apply-filter", software_id, 0, 0, "tcp", "any", "any", "any", 54321,
                "drop", "count", "software")
            original = ctl("get", software_id)
            status = fields(ctl("rule-status", software_id, 1))
            check(status["backend"] == "software" and status["count"] == "yes", str(status))
            count = ctl("count", software_id, 1)
            check("hits=0 bytes=0" in count, count)
            negative_round(1, 1, count)
            saved = state.read_bytes()
            stop(daemon)
            check(daemon.returncode == 0 and not sock.exists(), logs[-1].read_text())

            # 被拒绝的 transfer 不会重启后冒出来，已有软件规则则必须完整重放
            daemon = start("replayed.log")
            check_ports()
            check(ctl("get", software_id) == original, "replay changed existing software rule")
            check(state.read_bytes() == saved, "replay rewrote snapshot")
            check(ctl("count", software_id, 1) == count, "replay changed empty COUNT")
            check(fields(ctl("rule-status", software_id, 1))["backend"] == "software", "replay changed backend")
            negative_round(1, 1, count)
            ctl("delete", software_id, 1)
            ctl("get", software_id, error=errno.ENOENT)
            saved = state.read_bytes()
            unchanged(2, 0, saved)
            stop(daemon)
            check(daemon.returncode == 0 and not sock.exists(), logs[-1].read_text())

            # 删除也要持久化，不能只验证本次进程的内存账本
            daemon = start("deleted.log")
            ctl("get", software_id, error=errno.ENOENT)
            unchanged(2, 0, saved)
            stop(daemon)
            check(daemon.returncode == 0 and not sock.exists(), logs[-1].read_text())
            for log in logs:
                check("available=1024 capacity=1024 in-use=0" in log.read_text(), "process leaked mbufs: " + log.read_text())
        except Exception:
            for log in logs:
                print(log.read_text(), flush=True)
            raise
        finally:
            if daemon is not None:
                stop(daemon)
        print("PASS virtual-negative-acceptance net_ring: v24 identity evidence, transfer plan/apply refusal, "
              "all templates, no desired/COUNT/snapshot/PMD side effects, repeated requests, health, "
              "software lifecycle/replay, v23 rejection, cleanup; hardware-acceptance=not-run", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    args = parser.parse_args()
    run_case(args.build_dir.resolve())


if __name__ == "__main__":
    main()
