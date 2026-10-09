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


def history_events(report):
    """将公开的索引前缀转为便于比较的记录列表，不参与服务端的分页计算"""
    return [{name[len(prefix):]: value for name, value in report.items() if name.startswith(prefix)}
            for prefix in (f"event_{index}_" for index in range(report["returned"]))]


def check_history_last(event, metrics):
    """历史末条和既有最近失败指标必须保留相同身份、原因、补偿及生效状态"""
    for name in ("operation", "stage", "kind", "rule_id", "generation", "transaction_id",
                 "rule_count", "port", "port_known", "backend", "backend_known", "cause_error",
                 "response_error", "compensation_error", "compensation_rule_id"):
        check(event[name] == metrics[name], f"history field {name} differs: {event}")
    check(event["sequence"] == metrics["last_sequence"] and event["applied"] == metrics["last_applied"],
          "history changed operation sequence or publication outcome")


def check_history(report):
    """检查一份 JSON 的窗口与游标关系，失败 ID 连续，操作序号允许成功请求留下间隔"""
    check(report["capacity"] == 64 and 0 <= report["returned"] <= min(4, report["total"]), str(report))
    check(report["total"] <= 64 and report["overwritten"] + report["total"] == report["revision"], str(report))
    if report["total"]:
        check(report["oldest_event_id"] == report["overwritten"] + 1 and
              report["newest_event_id"] == report["revision"], str(report))
    else:
        check(report["oldest_event_id"] == report["newest_event_id"] == 0, str(report))
    check(report["gap"] == bool(report["total"] and
          report["after_event_id"] < report["oldest_event_id"] - 1), str(report))
    events = history_events(report)
    if events:
        first = max(report["after_event_id"] + 1, report["oldest_event_id"])
        check([event["id"] for event in events] == list(range(first, first + len(events))), str(report))
        check(report["next_after"] == events[-1]["id"], str(report))
    else:
        check(report["next_after"] == report["after_event_id"], str(report))
    check(report["has_more"] == (report["next_after"] < report["newest_event_id"]), str(report))


def read_history(ctl, telemetry, after=0, revision=None):
    """真实 CLI 与 JSON 逐字段对齐，包括身份、原始/补偿/最终错误和未知标志"""
    parameters = (after,) if revision is None else (after, revision)
    lines = ctl("rule-history", *parameters).splitlines()
    aliases = {"oldest": "oldest_event_id", "newest": "newest_event_id", "after": "after_event_id",
               "more": "has_more", "rule": "rule_id", "transaction": "transaction_id",
               "rules": "rule_count", "compensation-rule": "compensation_rule_id"}

    def converted(line):
        """JSON 用下划线和 0/1，CLI 的 yes/no 仅作表示转换，不改字段语义"""
        return {aliases.get(name, name.replace("-", "_")):
                int(value == "yes") if value in ("yes", "no") else
                value if name in ("operation", "stage", "kind", "backend") else int(value)
                for name, value in fields(line).items()}

    expected = converted(lines[0])
    check(len(lines) == expected["returned"] + 1, "incomplete CLI history page")
    for index, line in enumerate(lines[1:]):
        expected.update({f"event_{index}_{name}": value for name, value in converted(line).items()})

    def published():
        """指定版本尚未发布时允许暂时 null，等到整页字段一致才认定完成"""
        report = telemetry.query("/dppd/rule_history", *parameters)
        return report is not None and all(report.get(name) == value for name, value in expected.items())

    telemetry.wait(published)
    report = telemetry.query("/dppd/rule_history", *parameters)
    check_history(report)
    check(all(report[name] == value for name, value in expected.items()), "CLI and JSON history differ")
    return {name: value for name, value in report.items() if name != "publication"}


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
            "/dppd/stats", "/dppd/rules", "/dppd/rule", "/dppd/rule_failures", "/dppd/rule_latency",
            "/dppd/rule_history")))

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
