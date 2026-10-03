"""连接测试实例自己的 DPDK telemetry v2，不探测其他进程或主机服务"""

import json
from pathlib import Path
import re
import socket
import time

from batch_update import check


class Telemetry:
    """按 EAL 日志中的运行目录定位 socket，用完整 JSON 报文读取每次回应"""

    def __init__(self, daemon, log):
        deadline = time.monotonic() + 10
        self.socket = None
        while self.socket is None:
            check(daemon.poll() is None, "daemon exited before telemetry connection")
            match = re.search(r"Multi-process socket (.+)/mp_socket", log.read_text())
            if match:
                self.path = Path(match[1]) / "dpdk_telemetry.v2"
                if self.path.exists():
                    client = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                    client.settimeout(3)
                    try:
                        client.connect(str(self.path))
                        greeting = json.loads(client.recv(65536))
                    except (OSError, ValueError):
                        client.close()
                    else:
                        check(greeting["pid"] == daemon.pid, "connected to a different daemon")
                        self.socket = client
                        self.max_length = greeting["max_output_len"]
                        break
            check(time.monotonic() < deadline, "telemetry socket was not ready")
            time.sleep(0.02)
        self.wait(lambda: all(name in self.query("/") for name in (
            "/dppd/stats", "/dppd/rules", "/dppd/rule", "/dppd/rule_failures")))

    def query(self, command, *parameters):
        """逗号是 telemetry v2 参数分隔符，错误回调由 DPDK 编码成 null"""
        request = ",".join([command, *map(str, parameters)])
        self.socket.sendall(request.encode("ascii"))
        response = json.loads(self.socket.recv(self.max_length))
        check(command in response, "unexpected telemetry response: " + str(response))
        return response[command]

    def wait(self, predicate):
        """管理回应可能早于副本发布，等待指定完成态，不能把调度时差当成错误"""
        deadline = time.monotonic() + 5
        while not predicate():
            check(time.monotonic() < deadline, "telemetry publication timeout")
            time.sleep(0.01)

    def close(self):
        """显式释放测试连接，避免测试退出时还占用服务器客户端槽位"""
        if self.socket is not None:
            self.socket.close()
            self.socket = None
