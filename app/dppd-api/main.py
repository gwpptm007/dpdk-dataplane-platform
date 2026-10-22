#!/usr/bin/env python3
"""M5 版本化 HTTP/JSON 北向入口，使用配套 CLI 访问唯一的正式 daemon"""

import argparse
from dataclasses import dataclass
import hashlib
import hmac
import http.server
import ipaddress
import json
import os
from pathlib import Path
import queue
import re
import selectors
import signal
import socket
import socketserver
import ssl
import stat
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import parse_qs, urlsplit

MAX_BODY = 65536
MAX_OUTPUT = 131072
MAX_HEADER_BYTES = 16384
U64 = (1 << 64) - 1
ROLES = {"reader": 0, "operator": 1, "admin": 2}


class Failure(Exception):
    """失败码稳定且不携带路径、令牌或客户端提交的任意文本"""

    def __init__(self, status, code):
        super().__init__(code)
        self.status, self.code = status, code


def fields(value, required=(), optional=()):
    if not isinstance(value, dict) or not set(required) <= value.keys() or value.keys() - set(required) - set(optional):
        raise Failure(400, "invalid_fields")
    return value


def integer(value, minimum=0, maximum=U64 - 1):
    """64 位身份必须使用十进制字符串，普通小整数也允许 JSON number"""
    if isinstance(value, bool) or not isinstance(value, (str, int)):
        raise Failure(400, "invalid_integer")
    if isinstance(value, str) and not re.fullmatch(r"0|[1-9][0-9]{0,19}", value):
        raise Failure(400, "invalid_integer")
    if isinstance(value, int) and abs(value) > (1 << 53) - 1:
        raise Failure(400, "integer_requires_string")
    number = int(value)
    if not minimum <= number <= maximum:
        raise Failure(400, "integer_out_of_range")
    return str(number)


def choice(value, allowed):
    if not isinstance(value, str) or value not in allowed:
        raise Failure(400, "invalid_choice")
    return value


def strict_json(raw):
    """重复字段会让鉴权层与业务层理解不同，因此在解码时直接拒绝"""
    def pairs(items):
        value = {}
        for key, item in items:
            if key in value:
                raise Failure(400, "duplicate_field")
            value[key] = item
        return value

    def constant(_):
        raise Failure(400, "invalid_json")

    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=pairs, parse_constant=constant)
    except (UnicodeDecodeError, ValueError, RecursionError):
        raise Failure(400, "invalid_json") from None


@dataclass(frozen=True)
class Principal:
    role: str
    client: str


class Credentials:
    """只读启动凭据的 SHA-256，权限文件不得被其他用户读写，轮换时重启网关"""

    def __init__(self, path):
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK)
        try:
            info = os.fstat(descriptor)
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077 or info.st_nlink != 1:
                raise Failure(500, "unsafe_credentials_file")
            with os.fdopen(descriptor, "rb", closefd=False) as stream:
                raw = stream.read(MAX_BODY + 1)
            if len(raw) > MAX_BODY:
                raise Failure(500, "credentials_too_large")
            value = fields(strict_json(raw), ("version", "tokens"))
            if type(value["version"]) is not int or value["version"] != 1 or not isinstance(value["tokens"], list) or not 1 <= len(value["tokens"]) <= 64:
                raise Failure(500, "invalid_credentials")
            self.tokens = []
            seen = set()
            for item in value["tokens"]:
                fields(item, ("sha256", "role", "client"))
                digest = item["sha256"]
                if not isinstance(digest, str) or not re.fullmatch("[0-9a-f]{64}", digest) or digest in seen:
                    raise Failure(500, "invalid_credentials")
                seen.add(digest)
                self.tokens.append((digest, Principal(choice(item["role"], ROLES), integer(item["client"], 1))))
        finally:
            os.close(descriptor)

    def authenticate(self, authorization):
        if not isinstance(authorization, str) or not authorization.startswith("Bearer "):
            raise Failure(401, "authentication_required")
        token = authorization[7:]
        if not re.fullmatch(r"[A-Za-z0-9_-]{32,128}", token):
            raise Failure(401, "invalid_credentials")
        digest = hashlib.sha256(token.encode("ascii")).hexdigest()
        found = None
        for expected, principal in self.tokens:
            if hmac.compare_digest(digest, expected):
                found = principal
        if found is None:
            raise Failure(401, "invalid_credentials")
        return found


def require(principal, role):
    if ROLES[principal.role] < ROLES[role]:
        raise Failure(403, "permission_denied")


def rule_arguments(value, expected):
    """把有类型的规则字段翻译为固定参数，不接受客户端给出的命令名或文件路径"""
    fields(value, ("id", "port", "kind"), ("priority", "protocol", "source", "destination", "source_port",
           "destination_port", "action", "count", "mark", "shared_count"))
    ident, port = integer(value["id"], 1), integer(value["port"], 0, 65534)
    priority = integer(value.get("priority", 0), 0, (1 << 32) - 1)
    kind = choice(value["kind"], ("drop", "ipv4_acl", "ipv6_acl"))
    if kind == "drop":
        if set(value) & {"protocol", "source", "destination", "source_port", "destination_port", "action", "count", "mark"}:
            raise Failure(400, "invalid_drop_fields")
        result = ["apply-drop", ident, port, expected, priority, "software"]
    else:
        version = 4 if kind == "ipv4_acl" else 6
        protocol = value.get("protocol", f"ipv{version}")
        if isinstance(protocol, str) and re.fullmatch(r"ip:[0-9]{1,3}", protocol):
            protocol = "ip:" + integer(protocol[3:], 0, 255)
        else:
            protocol = choice(protocol, (f"ipv{version}", "tcp", "udp"))

        def network(name):
            text = value.get(name, "any")
            if text == "any":
                return text
            if not isinstance(text, str) or len(text) > 64:
                raise Failure(400, "invalid_address")
            try:
                parsed = ipaddress.ip_network(text, strict=False)
            except ValueError:
                raise Failure(400, "invalid_address") from None
            if parsed.version != version:
                raise Failure(400, "address_family_mismatch")
            return str(parsed)

        def ports(name):
            text = value.get(name, "any")
            if text == "any":
                return text
            if isinstance(text, (str, int)) and not isinstance(text, bool):
                parts = str(text).split("-")
                if 1 <= len(parts) <= 2:
                    numbers = [integer(item, 0, 65535) for item in parts]
                    if len(numbers) == 1 or int(numbers[0]) <= int(numbers[1]):
                        return "-".join(numbers)
            raise Failure(400, "invalid_port_range")

        source_port, destination_port = ports("source_port"), ports("destination_port")
        if protocol not in ("udp", "tcp") and (source_port != "any" or destination_port != "any"):
            raise Failure(400, "ports_require_transport")
        result = ["acl-set" if version == 4 else "acl6-set", ident, port, expected, priority, protocol,
                  network("source"), network("destination"), source_port, destination_port,
                  choice(value.get("action", "drop"), ("drop", "allow"))]
        counted = value.get("count", False)
        if not isinstance(counted, bool):
            raise Failure(400, "invalid_boolean")
        if counted:
            result.append("count")
        if "mark" in value:
            result.append("mark:" + integer(value["mark"], 0, (1 << 32) - 1))
    if "shared_count" in value:
        shared = fields(value["shared_count"], ("id", "generation"))
        result.append("shared:" + integer(shared["id"], 1) + ":" + integer(shared["generation"]))
    return result


def configuration_lines(changes):
    if not isinstance(changes, list) or not 1 <= len(changes) <= 8:
        raise Failure(400, "invalid_changes")
    lines = []
    for item in changes:
        if not isinstance(item, dict):
            raise Failure(400, "invalid_changes")
        operation = item.get("operation")
        if operation == "count_add":
            fields(item, ("operation", "id", "port", "domain"))
            arguments = ["count-add", integer(item["id"], 1), integer(item["port"], 0, 65534),
                         choice(item["domain"], ("ingress", "egress", "transfer"))]
        elif operation == "count_delete":
            fields(item, ("operation", "id", "generation"))
            arguments = ["count-delete", integer(item["id"], 1), integer(item["generation"], 1)]
        elif operation == "template_delete":
            fields(item, ("operation", "key"))
            arguments = ["template-delete", integer(item["key"], 1)]
        elif operation == "template_add":
            fields(item, ("operation", "key", "capacity", "dynamic_actions", "example"))
            arguments = ["template-add", integer(item["key"], 1), integer(item["capacity"], 1, 65536),
                         integer(item["dynamic_actions"], 0, (1 << 32) - 1), *rule_arguments(item["example"], "0")]
        else:
            raise Failure(400, "invalid_operation")
        lines.append(" ".join(arguments))
    return "\n".join(lines) + "\n"


class Backend:
    def __init__(self, program, control_socket, timeout=10.0):
        self.program, self.control_socket, self.timeout = str(program), str(control_socket), timeout

    def execute(self, arguments):
        """输出与执行时间都有上限，子进程的原始错误不回传，超时不表示写入已取消"""
        command = [self.program, "--json", "--socket", self.control_socket, *arguments]
        try:
            child = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, close_fds=True)
        except OSError:
            raise Failure(503, "backend_unavailable") from None
        output = bytearray()
        total = 0
        deadline = time.monotonic() + self.timeout
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(child.stdout, selectors.EVENT_READ, True)
                selector.register(child.stderr, selectors.EVENT_READ, False)
                while selector.get_map():
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise Failure(504, "backend_timeout_state_unknown")
                    for key, _ in selector.select(remaining):
                        chunk = os.read(key.fd, 4096)
                        if not chunk:
                            selector.unregister(key.fileobj)
                            continue
                        total += len(chunk)
                        if total > MAX_OUTPUT:
                            raise Failure(502, "backend_response_too_large")
                        if key.data:
                            output.extend(chunk)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise Failure(504, "backend_timeout_state_unknown")
            try:
                child.wait(timeout=remaining)
            except subprocess.TimeoutExpired:
                raise Failure(504, "backend_timeout_state_unknown") from None
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()
            child.stdout.close()
            child.stderr.close()
        if not output:
            raise Failure(503, "backend_unavailable_state_unknown")
        try:
            value = strict_json(output)
            fields(value, ("api_version", "status", "data"))
            if value["api_version"] != "v1" or type(value["status"]) is not int or not isinstance(value["data"], dict):
                raise Failure(502, "invalid_backend_response")
            if child.returncode != 0 and value["status"] == 0 and not (
                    value["data"].get("state") == "complete" and type(value["data"].get("status")) is int
                    and value["data"]["status"] != 0):
                raise Failure(502, "backend_failed_state_unknown")
        except Failure:
            raise Failure(502, "invalid_backend_response") from None
        return value


class Application:
    def __init__(self, backend):
        self.backend = backend

    def dispatch(self, method, target, principal, body):
        if len(target) > 2048:
            raise Failure(414, "target_too_long")
        try:
            url = urlsplit(target)
        except ValueError:
            raise Failure(400, "invalid_target") from None
        if url.scheme or url.netloc or url.fragment or "%" in url.path or "//" in url.path:
            raise Failure(400, "invalid_target")
        try:
            query = parse_qs(url.query, keep_blank_values=True, strict_parsing=True, max_num_fields=8) if url.query else {}
        except ValueError:
            raise Failure(400, "invalid_query") from None
        if any(len(value) != 1 for value in query.values()):
            raise Failure(400, "duplicate_query")
        query = {key: value[0] for key, value in query.items()}
        path = url.path
        arguments, content = None, None
        if method == "GET":
            if body:
                raise Failure(400, "unexpected_body")
            simple = {"/v1/health": "health", "/v1/ready": "health", "/v1/status": "flow-status",
                      "/v1/persistence": "persistence-status"}
            if path in simple:
                fields(query)
                arguments = [simple[path]]
            elif path == "/v1/stats":
                fields(query, (), ("port", "queue"))
                arguments = ["stats", *["all" if query.get(name, "all") == "all" else integer(query[name], 0, 65534)
                                        for name in ("port", "queue")]]
            elif path == "/v1/rules":
                fields(query, (), ("after", "revision"))
                arguments = ["list", integer(query.get("after", "0"))]
                if "revision" in query:
                    arguments.append(integer(query["revision"]))
            elif path == "/v1/configuration":
                fields(query, (), ("revision", "after_table", "after_count"))
                if ("after_table" in query or "after_count" in query) and "revision" not in query:
                    raise Failure(400, "revision_required")
                arguments = ["flow-config-get"]
                if "revision" in query:
                    arguments += [integer(query["revision"]), integer(query.get("after_table", "0")),
                                  integer(query.get("after_count", "0"))]
            elif re.fullmatch(r"/v1/rules/[0-9]+", path):
                fields(query)
                arguments = ["get", integer(path.rsplit("/", 1)[1], 1)]
            elif re.fullmatch(r"/v1/rules/[0-9]+/count", path):
                fields(query, ("generation",))
                arguments = ["count", integer(path.split("/")[3], 1), integer(query["generation"], 1)]
            elif re.fullmatch(r"/v1/counts/[0-9]+", path):
                fields(query, ("generation",))
                arguments = ["shared-count-query", integer(path.rsplit("/", 1)[1], 1), integer(query["generation"], 1)]
            elif re.fullmatch(r"/v1/requests/[0-9]+", path):
                fields(query)
                arguments = ["request-get", principal.client, integer(path.rsplit("/", 1)[1], 1)]
            else:
                raise Failure(404, "not_found")
        elif method == "POST":
            fields(query)
            value = strict_json(body)
            if path in ("/v1/recovery/flush", "/v1/recovery/retry"):
                require(principal, "admin")
                fields(value)
                arguments = ["persistence-flush" if path.endswith("flush") else "flow-retry"]
            elif path == "/v1/configuration":
                require(principal, "admin")
                fields(value, ("sequence", "revision", "changes"))
                content = configuration_lines(value["changes"])
                arguments = ["request-submit", principal.client, integer(value["sequence"], 1), "flow-config-apply",
                             integer(value["revision"])]
            elif path in ("/v1/rules/create", "/v1/rules/update"):
                require(principal, "operator")
                fields(value, ("sequence", "table", "queue", "rule"), ("generation",))
                update = path.endswith("update")
                if update != ("generation" in value):
                    raise Failure(400, "generation_required_for_update")
                expected = integer(value["generation"], 1) if update else "0"
                rule = rule_arguments(value["rule"], expected)
                if "shared_count" in value["rule"] and integer(value["rule"]["shared_count"]["generation"]) == "0":
                    raise Failure(400, "shared_generation_required")
                arguments = ["request-submit", principal.client, integer(value["sequence"], 1),
                             "flow-update" if update else "flow-create", integer(value["table"], 1),
                             integer(value["queue"], 0, 7), *rule]
            elif path == "/v1/rules/remove":
                require(principal, "operator")
                fields(value, ("sequence", "queue", "id", "generation"))
                arguments = ["request-submit", principal.client, integer(value["sequence"], 1), "flow-remove",
                             integer(value["queue"], 0, 7), integer(value["id"], 1), integer(value["generation"], 1)]
            else:
                raise Failure(404, "not_found")
        else:
            raise Failure(405, "method_not_allowed")
        if content is not None:
            descriptor, name = tempfile.mkstemp(prefix="dppd-api-config-")
            try:
                with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
                    stream.write(content)
                result = self.backend.execute([*arguments, name])
            finally:
                os.unlink(name)
        else:
            result = self.backend.execute(arguments)
        code = result["status"]
        status = {0: 200, -2: 404, -13: 403, -17: 409, -116: 409, -16: 409, -22: 400, -95: 422,
                  -28: 507, -11: 202, -117: 503}.get(code, 503 if code else 200)
        if code == 0 and result["data"].get("state") == "pending":
            status = 202
        if path == "/v1/ready" and not result["data"].get("ready", False):
            status = 503
        return status, result


class BoundedReader:
    """请求头按累计字节限额读取，不能用很多短行绕过单行长度限制"""

    def __init__(self, stream):
        self.stream, self.used, self.headers_done = stream, 0, False

    def readline(self, limit=-1):
        remaining = MAX_HEADER_BYTES + 1 - self.used
        if remaining <= 0:
            raise Failure(431, "headers_too_large")
        line = self.stream.readline(min(limit if limit >= 0 else remaining, remaining))
        self.used += len(line)
        if self.used > MAX_HEADER_BYTES:
            raise Failure(431, "headers_too_large")
        return line

    def read(self, length):
        return self.stream.read(length)

    def close(self):
        self.stream.close()


class Audit:
    """审计只尽力送出，管道堵塞不能占住管理请求或阻止进程退出"""

    def __init__(self, descriptor, capacity=128):
        self.queue = queue.Queue(maxsize=capacity)
        self.lock = threading.Lock()
        self.dropped, self.failed = 0, False
        self.stopped = threading.Event()
        self.descriptor = os.dup(descriptor)
        self.thread = threading.Thread(target=self.run, name="dppd-api-audit", daemon=True)
        self.thread.start()

    def emit(self, record):
        payload = (json.dumps(record, separators=(",", ":")) + "\n").encode("utf-8")
        with self.lock:
            if self.failed or self.stopped.is_set() or len(payload) > 1024:
                self.dropped += 1
                return
        try:
            self.queue.put_nowait(payload)
        except queue.Full:
            with self.lock:
                self.dropped += 1

    def snapshot(self):
        with self.lock:
            return {"audit_dropped": str(self.dropped), "audit_queue_depth": self.queue.qsize(),
                    "audit_sink_failed": self.failed}

    def run(self):
        try:
            while not self.stopped.is_set():
                try:
                    payload = self.queue.get(timeout=0.05)
                except queue.Empty:
                    continue
                try:
                    view = memoryview(payload)
                    while view:
                        written = os.write(self.descriptor, view)
                        if written <= 0:
                            raise OSError("audit write made no progress")
                        view = view[written:]
                except OSError:
                    with self.lock:
                        self.failed = True
                        self.dropped += 1
                finally:
                    self.queue.task_done()
        finally:
            os.close(self.descriptor)

    def close(self):
        self.stopped.set()
        while True:
            try:
                self.queue.get_nowait()
                self.queue.task_done()
                with self.lock:
                    self.dropped += 1
            except queue.Empty:
                break
        # 守护写线程可能仍在内核管道中等待，不等待其完成，也不触碰 Python 的 stderr 缓冲锁
        self.thread.join(timeout=0.1)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "dppd-api"
    sys_version = ""

    def setup(self):
        self.request.settimeout(self.server.request_timeout)
        super().setup()
        self.rfile = BoundedReader(self.rfile)

    def handle(self):
        """总时限关闭整个连接，持续缓慢发送字节也不能无限占用线程"""
        def expire():
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

        timer = threading.Timer(self.server.request_timeout, expire)
        timer.daemon = True
        timer.start()
        try:
            super().handle()
        except (OSError, Failure, ValueError):
            pass
        finally:
            timer.cancel()

    def handle_one_request(self):
        self.requestline, self.request_version, self.command = "", "HTTP/1.1", None
        self.close_connection = True
        try:
            super().handle_one_request()
        except Failure as error:
            self.reply(error.status, {"api_version": "v1", "error": {"code": error.code}})
        self.close_connection = True

    def handle_expect_100(self):
        self.reply(400, {"api_version": "v1", "error": {"code": "unsupported_framing"}})
        return False

    def log_message(self, *_):
        """不输出未经校验的 URL 或认证头，审计日志由完成后的固定字段组成"""

    def send_error(self, code, message=None, explain=None):
        self.reply(code, {"api_version": "v1", "error": {"code": "invalid_http"}})

    def reply(self, status, value):
        body = json.dumps(value, ensure_ascii=True, separators=(",", ":")).encode("ascii")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Connection", "close")
        if status == 401:
            self.send_header("WWW-Authenticate", "Bearer")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def request(self):
        started = time.monotonic()
        principal = None
        status = 500
        try:
            for name in ("Authorization", "Content-Length", "Content-Type", "Host"):
                if len(self.headers.get_all(name, [])) > 1:
                    raise Failure(400, "duplicate_header")
            if self.headers.get("Transfer-Encoding") is not None or self.headers.get("Expect") is not None:
                raise Failure(400, "unsupported_framing")
            principal = self.server.credentials.authenticate(self.headers.get("Authorization"))
            length = int(integer(self.headers.get("Content-Length", "0"), 0, MAX_BODY))
            if self.command == "POST" and self.headers.get("Content-Type", "").lower() != "application/json":
                raise Failure(415, "json_required")
            body = self.rfile.read(length)
            if len(body) != length:
                raise Failure(400, "incomplete_body")
            status, value = self.server.application.dispatch(self.command, self.path, principal, body)
        except Failure as error:
            status, value = error.status, {"api_version": "v1", "error": {"code": error.code}}
        except (OSError, ValueError):
            status, value = 503, {"api_version": "v1", "error": {"code": "request_interrupted_state_unknown"}}
        if self.path in ("/v1/health", "/v1/ready") and "data" in value:
            value["data"]["gateway"] = self.server.audit.snapshot()
        self.reply(status, value)
        record = {"event": "api_request", "time_unix_ns": str(time.time_ns()), "status": status,
                  "role": principal.role if principal else "anonymous",
                  "client": principal.client if principal else None, "elapsed_ms": int((time.monotonic() - started) * 1000)}
        if "data" in value and value["data"].get("state") in ("pending", "complete"):
            record["sequence"] = value["data"].get("sequence")
            record["operation"] = value["data"].get("operation")
        self.server.audit.emit(record)

    do_GET = request
    do_POST = request
    do_PUT = request
    do_DELETE = request
    do_PATCH = request


class BoundedServer(socketserver.ThreadingMixIn):
    daemon_threads = False
    block_on_close = True
    request_queue_size = 16

    def initialize(self, application, credentials, concurrency, timeout, tls=None):
        self.application, self.credentials, self.request_timeout, self.tls = application, credentials, timeout, tls
        self.slots = threading.BoundedSemaphore(concurrency)
        self.audit = Audit(sys.stderr.fileno())

    def server_close(self):
        try:
            super().server_close()
        finally:
            if hasattr(self, "audit"):
                self.audit.close()

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self.slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            if self.tls is not None:
                request.settimeout(self.request_timeout)
                request = self.tls.wrap_socket(request, server_side=True, do_handshake_on_connect=False)
                request.do_handshake()
            super().process_request_thread(request, client_address)
        except (OSError, ssl.SSLError):
            self.shutdown_request(request)
        finally:
            self.slots.release()


class TCPServer(BoundedServer, http.server.HTTPServer):
    allow_reuse_address = True


class UnixServer(BoundedServer, socketserver.TCPServer):
    address_family = getattr(socket, "AF_UNIX", -1)

    def server_bind(self):
        super().server_bind()
        os.chmod(self.server_address, 0o600)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True, help="正式 daemon 的本机管理 socket")
    parser.add_argument("--dppctl", type=Path, required=True, help="同发布包的 dppctl，可指向 current 符号链接")
    parser.add_argument("--tokens", type=Path, required=True, help="0600 的令牌哈希与角色文件")
    endpoint = parser.add_mutually_exclusive_group(required=True)
    endpoint.add_argument("--unix", type=Path)
    endpoint.add_argument("--listen", help="显式 IP 地址，非回环地址必须配置 TLS")
    parser.add_argument("--port", type=int, default=8443)
    parser.add_argument("--tls-cert", type=Path)
    parser.add_argument("--tls-key", type=Path)
    parser.add_argument("--concurrency", type=int, default=8)
    parser.add_argument("--timeout", type=float, default=15)
    args = parser.parse_args()
    if not hasattr(os, "O_NOFOLLOW"):
        parser.error("生产网关需要 Linux 的文件权限与 Unix socket 支持")
    if not args.dppctl.is_absolute() or not 1 <= args.concurrency <= 32 or not 2 <= args.timeout <= 60:
        parser.error("dppctl 必须是绝对路径，并发数为 1～32，总时限为 2～60 秒")
    credentials = Credentials(args.tokens)
    application = Application(Backend(args.dppctl, args.socket, args.timeout - 1))
    tls = None
    if bool(args.tls_cert) != bool(args.tls_key):
        parser.error("TLS 证书和私钥必须同时提供")
    if args.tls_cert:
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.minimum_version = ssl.TLSVersion.TLSv1_2
        tls.load_cert_chain(args.tls_cert, args.tls_key)
    if args.unix:
        if not args.unix.is_absolute() or len(os.fsencode(args.unix)) > 107:
            parser.error("Unix socket 必须是长度不超过 107 字节的绝对路径")
        server = UnixServer(str(args.unix), Handler)
    else:
        try:
            address = ipaddress.ip_address(args.listen)
        except ValueError:
            parser.error("listen 只能是显式 IP 地址")
        if not address.is_loopback and tls is None:
            parser.error("非回环监听必须启用 TLS")
        if not 1 <= args.port <= 65535:
            parser.error("端口必须为 1～65535")
        if address.version == 6:
            TCPServer.address_family = socket.AF_INET6
        server = TCPServer((str(address), args.port), Handler)
    server.initialize(application, credentials, args.concurrency, args.timeout, tls)
    identity = os.stat(args.unix) if args.unix else None

    def stop(_signal, _frame):
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        server.serve_forever(poll_interval=0.1)
    finally:
        server.server_close()
        if args.unix:
            try:
                current = os.stat(args.unix, follow_symlinks=False)
                if (current.st_dev, current.st_ino) == (identity.st_dev, identity.st_ino):
                    args.unix.unlink()
            except FileNotFoundError:
                pass


if __name__ == "__main__":
    try:
        main()
    except (OSError, Failure) as error:
        print(json.dumps({"event": "api_start_failed", "error": error.code if isinstance(error, Failure)
                          else "io_error"}), file=sys.stderr)
        raise SystemExit(1)
