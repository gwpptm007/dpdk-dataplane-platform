"""连接测试实例自己的 DPDK telemetry v2，不探测其他进程或主机服务"""

import json
from pathlib import Path
import re
import socket
import time

from batch_update import check, fields


LATENCY_SCOPES = ("software", "software_batch", "rte_flow")
LATENCY_BOUNDS = (1000, 5000, 10000, 50000, 100000, 500000, 1000000, 5000000,
                  10000000, 50000000, 100000000, 500000000, 1000000000,
                  5000000000, 10000000000, 18446744073709551615)


def check_latency(report):
    """按公开统计契约检查单次 JSON，包括区间总数、有效标志和分位数上界"""
    check(report["bucket_count"] == 16, str(report))
    check(tuple(report[f"bucket_{index}_upper_ns"] for index in range(16)) == LATENCY_BOUNDS,
          "latency bucket boundaries changed")
    scopes = LATENCY_SCOPES if report["scope"] == "all" else (report["scope"],)
    for scope in scopes:
        check(scope in LATENCY_SCOPES, str(report))
        samples = report[f"{scope}_samples"]
        check(samples == sum(report[f"{scope}_bucket_{index}"] for index in range(16)), str(report))
        quantiles = samples > 0 and not report[f"{scope}_counters_saturated"]
        mean = quantiles and not report[f"{scope}_total_saturated"]
        check(report[f"{scope}_mean_available"] == mean and
              report[f"{scope}_quantiles_available"] == quantiles, str(report))
        if mean:
            check(report[f"{scope}_mean_ns"] == report[f"{scope}_total_ns"] // samples, str(report))
        else:
            check(report[f"{scope}_mean_ns"] == 0, str(report))
        if quantiles:
            low, high = report[f"{scope}_min_ns"], report[f"{scope}_max_ns"]
            check(low <= report[f"{scope}_p50_upper_ns"] <= report[f"{scope}_p95_upper_ns"] <=
                  report[f"{scope}_p99_upper_ns"] <= high, str(report))
        else:
            check(all(report[f"{scope}_p{rank}_upper_ns"] == 0 for rank in (50, 95, 99)), str(report))


def read_latency(ctl, telemetry):
    """管理 CLI 与完整 telemetry 发布逐字段比较，等待发布仅处理正常调度时差"""
    summaries, buckets = {}, {}
    for line in ctl("rule-latency").splitlines():
        value = fields(line)
        scope = value.pop("scope")
        if line.startswith("rule-latency "):
            summaries[scope] = {name.replace("-", "_"): int(token == "yes")
                                if token in ("yes", "no") else int(token)
                                for name, token in value.items()}
        else:
            index = int(value["index"])
            check(int(value["upper-ns"]) == LATENCY_BOUNDS[index], line)
            check(index not in buckets.setdefault(scope, {}), "duplicate CLI bucket")
            buckets[scope][index] = int(value["samples"])
    check(set(summaries) == set(LATENCY_SCOPES) and
          all(set(buckets[scope]) == set(range(16)) for scope in LATENCY_SCOPES), "incomplete CLI history")
    expected = {f"{scope}_{name}": value for scope, summary in summaries.items()
                for name, value in summary.items()}
    expected.update({f"{scope}_bucket_{index}": value for scope, entries in buckets.items()
                     for index, value in entries.items()})
    def published():
        """一份完整回应必须同时满足全部字段，不能拼接多次查询得到的不同发布"""
        report = telemetry.query("/dppd/rule_latency")
        return all(report.get(name) == value for name, value in expected.items())

    telemetry.wait(published)
    report = telemetry.query("/dppd/rule_latency")
    check_latency(report)
    check(all(report[name] == value for name, value in expected.items()), "CLI and telemetry history differ")
    # publication 属于整个规则镜像，失败指标或清理变化可更新它，历史值本身保持独立
    return {name: value for name, value in report.items() if name != "publication"}


def check_latency_commits(report, software, software_batch, rte_flow, batch_rules=0):
    """成功提交总数包含计时未知项，整批的规则数独立于其一次发布样本数"""
    for scope, commits in zip(LATENCY_SCOPES, (software, software_batch, rte_flow)):
        check(report[f"{scope}_samples"] + report[f"{scope}_unavailable"] == commits, str(report))
        check(report[f"{scope}_rules"] == (batch_rules if scope == "software_batch" else commits),
              str(report))


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
            "/dppd/stats", "/dppd/rules", "/dppd/rule", "/dppd/rule_failures", "/dppd/rule_latency")))

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
