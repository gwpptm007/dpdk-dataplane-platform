#!/usr/bin/env python3
"""验证真实 HTTP 解析、权限边界、资源限制与确定性的参数翻译"""

import hashlib
import http.client
import importlib.util
import json
import os
from pathlib import Path
import queue
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("dppd_northbound", ROOT / "app/dppd-api/main.py")
api = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = api
SPEC.loader.exec_module(api)


class FakeBackend:
    """只替换 daemon 调用，HTTP、JSON 和授权均运行生产代码"""

    def __init__(self):
        self.calls, self.configuration = [], None

    def execute(self, arguments):
        self.calls.append(arguments)
        if "flow-config-apply" in arguments:
            self.configuration = Path(arguments[-1]).read_text(encoding="utf-8")
        return {"api_version": "v1", "status": 0, "data": {"ready": True}}


class NorthboundTests(unittest.TestCase):
    def setUp(self):
        self.backend = FakeBackend()
        self.credentials = object.__new__(api.Credentials)
        self.tokens = {role: (role[0] * 64) for role in api.ROLES}
        self.credentials.tokens = [(hashlib.sha256(token.encode()).hexdigest(), api.Principal(role, str(index + 1)))
                                   for index, (role, token) in enumerate(self.tokens.items())]
        self.application = api.Application(self.backend)
        self.server = api.TCPServer(("127.0.0.1", 0), api.Handler)
        self.server.initialize(self.application, self.credentials, 2, 0.3)
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.02})
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.thread.join()
        self.server.server_close()

    def request(self, method="GET", path="/v1/status", body=None, role="reader", token=True, raw=None):
        connection = http.client.HTTPConnection(*self.server.server_address, timeout=2)
        headers = {"Authorization": "Bearer " + self.tokens[role]} if token else {}
        if body is not None or raw is not None:
            headers["Content-Type"] = "application/json"
        encoded = raw if raw is not None else json.dumps(body) if body is not None else None
        try:
            connection.request(method, path, body=encoded, headers=headers)
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        finally:
            connection.close()

    def test_authentication_and_roles(self):
        self.assertEqual(self.request(token=False)[0], 401)
        self.assertEqual(self.request()[0], 200)
        self.assertEqual(self.request("POST", "/v1/recovery/flush", {}, role="operator")[0], 403)
        self.assertEqual(self.request("POST", "/v1/recovery/flush", {}, role="admin")[0], 200)
        rule = {"sequence": "1", "table": "1", "queue": 0, "rule": {"id": "9", "port": 0, "kind": "drop"}}
        self.assertEqual(self.request("POST", "/v1/rules/create", rule)[0], 403)
        self.assertEqual(self.request("POST", "/v1/rules/create", rule, role="operator")[0], 200)
        self.assertEqual(self.backend.calls[-1], ["request-submit", "2", "1", "flow-create", "1", "0",
                                                "apply-drop", "9", "0", "0", "0", "software"])

    def test_duplicate_or_unrecognized_fields_never_dispatch(self):
        self.assertEqual(self.request("POST", "/v1/recovery/flush", raw='{"x":1,"x":2}', role="admin")[0], 400)
        self.assertEqual(self.request(path="/v1/rules?after=1&after=2")[0], 400)
        self.assertEqual(self.request(path="/v1/status?command=rm")[0], 400)
        self.assertEqual(self.request("POST", "/v1/recovery/flush", {"command": "bad"}, role="admin")[0], 400)
        self.assertEqual(self.request("DELETE")[0], 405)
        self.assertEqual(self.backend.calls, [])

    def test_request_identity_is_bound_to_credential(self):
        self.assertEqual(self.request(path="/v1/requests/9007199254740993", role="operator")[0], 200)
        self.assertEqual(self.backend.calls[-1], ["request-get", "2", "9007199254740993"])
        self.assertEqual(self.request(path="/v1/requests/1?client=3", role="operator")[0], 400)
        rule = {"sequence": 9007199254740993, "table": "1", "queue": 0,
                "rule": {"id": "9", "port": 0, "kind": "drop"}}
        self.assertEqual(self.request("POST", "/v1/rules/create", rule, role="operator")[0], 400)

    def test_configuration_is_typed_and_temporary_file_is_removed(self):
        body = {"sequence": "5", "revision": "2", "changes": [
            {"operation": "count_add", "id": "5", "port": 0, "domain": "ingress"},
            {"operation": "template_add", "key": "9", "capacity": 4, "dynamic_actions": 0,
             "example": {"id": "1", "port": 0, "kind": "drop", "shared_count": {"id": "5", "generation": "0"}}}]}
        self.assertEqual(self.request("POST", "/v1/configuration", body, role="operator")[0], 403)
        self.assertEqual(self.request("POST", "/v1/configuration", body, role="admin")[0], 200)
        self.assertEqual(self.backend.configuration,
                         "count-add 5 0 ingress\ntemplate-add 9 4 0 apply-drop 1 0 0 0 software shared:5:0\n")
        self.assertFalse(Path(self.backend.calls[-1][-1]).exists())
        body["changes"][0]["domain"] = "ingress\ntemplate-delete 1"
        self.assertEqual(self.request("POST", "/v1/configuration", body, role="admin")[0], 400)

    def test_ipv6_acl_validates_addresses_ports_and_shared_generations(self):
        rule = {"id": "1", "port": 0, "kind": "ipv6_acl", "protocol": "udp", "source": "2001:db8::1/64",
                "destination_port": "80-90", "count": True, "mark": 42, "action": "allow"}
        arguments = api.rule_arguments(rule, "7")
        self.assertEqual(arguments, ["acl6-set", "1", "0", "7", "0", "udp", "2001:db8::/64", "any",
                                     "any", "80-90", "allow", "count", "mark:42"])
        rule["source"] = "192.0.2.1/24"
        with self.assertRaises(api.Failure):
            api.rule_arguments(rule, "7")
        with self.assertRaises(api.Failure):
            api.rule_arguments({"id": "1", "port": 0, "kind": "drop", "count": True}, "0")

    def test_framing_and_header_limits(self):
        authorization = "Authorization: Bearer " + self.tokens["reader"] + "\r\n"
        requests = ["Content-Length: 0\r\nContent-Length: 0\r\n",
                    "Transfer-Encoding: chunked\r\n", "Content-Length: 65537\r\n",
                    "X-Large: " + "x" * 17000 + "\r\n"]
        for headers in requests:
            with socket.create_connection(self.server.server_address, timeout=2) as connection:
                connection.sendall(("GET /v1/status HTTP/1.1\r\nHost: localhost\r\n" + authorization + headers + "\r\n").encode())
                response = connection.recv(2048)
                self.assertTrue(response.startswith((b"HTTP/1.1 400", b"HTTP/1.1 431")), response)
        with socket.create_connection(self.server.server_address, timeout=2) as connection:
            connection.sendall(("GET /" + "a" * 17000 + " HTTP/1.1\r\n\r\n").encode())
            self.assertTrue(connection.recv(2048).startswith(b"HTTP/1.1 431"))
        self.assertEqual(self.backend.calls, [])

    def test_slow_connections_release_bounded_slots(self):
        connections = [socket.create_connection(self.server.server_address, timeout=2) for _ in range(2)]
        try:
            for connection in connections:
                connection.sendall(b"GET /v1/status HTTP/1.1\r\nHost: ")
            time.sleep(0.5)
            self.assertEqual(self.request()[0], 200)
            for connection in connections:
                self.assertEqual(connection.recv(1), b"")
        finally:
            for connection in connections:
                connection.close()

    def test_completed_responses_hold_slots_until_worker_cleanup(self):
        finished, release = queue.Queue(), threading.Event()

        class HeldFinish(api.Handler):
            def finish(self):
                super().finish()
                # 响应已送出不等于请求线程已退出，显式停在实际释放并发槽之前
                finished.put(None)
                release.wait(timeout=2)

        self.server.RequestHandlerClass = HeldFinish
        try:
            for _ in range(2):
                self.assertEqual(self.request()[0], 200)
                finished.get(timeout=2)
            # 两个槽都占用时准入直接关闭连接，不应创建第三个后端请求
            with self.assertRaises((http.client.RemoteDisconnected, ConnectionResetError)):
                self.request()
            self.assertEqual(len(self.backend.calls), 2)
        finally:
            release.set()
            self.server._threads.join()
        self.assertEqual(self.request()[0], 200)
        self.assertEqual(len(self.backend.calls), 3)

    @unittest.skipUnless(hasattr(os, "O_NOFOLLOW"), "权限文件校验需要 Linux 文件属性")
    def test_credentials_permissions_symlink_and_duplicate_hash(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "tokens"
            item = {"sha256": hashlib.sha256(self.tokens["reader"].encode()).hexdigest(), "role": "reader", "client": "1"}
            path.write_text(json.dumps({"version": 1, "tokens": [item]}), encoding="utf-8")
            path.chmod(0o600)
            self.assertEqual(api.Credentials(path).authenticate("Bearer " + self.tokens["reader"]).role, "reader")
            path.chmod(0o644)
            with self.assertRaises(api.Failure):
                api.Credentials(path)
            path.chmod(0o600)
            linked = path.with_name("link")
            linked.symlink_to(path)
            with self.assertRaises(OSError):
                api.Credentials(linked)
            path.write_text(json.dumps({"version": 1, "tokens": [item, item]}), encoding="utf-8")
            with self.assertRaises(api.Failure):
                api.Credentials(path)

    @unittest.skipUnless(os.name == "posix", "子进程管道限额使用 Linux selector")
    def test_backend_output_timeout_and_completed_business_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            program = Path(temporary) / "cli"

            def write(source):
                program.write_text("#!/usr/bin/env python3\n" + source, encoding="utf-8")
                program.chmod(0o700)

            backend = api.Backend(program, "/unopened-control", timeout=0.15)
            write("import time\ntime.sleep(2)\n")
            with self.assertRaises(api.Failure) as caught:
                backend.execute(["flow-status"])
            self.assertEqual(caught.exception.code, "backend_timeout_state_unknown")
            # 输出上限与 JSON 解析独立验收，不能复用故意设置得很短的超时来限制解释器启动
            backend = api.Backend(program, "/unopened-control", timeout=5)
            write("import sys\nsys.stdout.write('x' * 200000)\n")
            with self.assertRaises(api.Failure) as caught:
                backend.execute(["flow-status"])
            self.assertEqual(caught.exception.code, "backend_response_too_large")
            write("print('{bad}')\n")
            with self.assertRaises(api.Failure) as caught:
                backend.execute(["flow-status"])
            self.assertEqual(caught.exception.code, "invalid_backend_response")
            write("import json, sys\nprint(json.dumps({'api_version':'v1','status':0,"
                  "'data':{'state':'complete','status':-2,'durable':True}}))\nsys.exit(1)\n")
            result = backend.execute(["request-get", "1", "2"])
            self.assertEqual(result["data"]["status"], -2)

    @unittest.skipUnless(os.name == "posix", "FIFO 保护需要 Linux 文件属性")
    def test_credentials_fifo_is_rejected_without_waiting_for_writer(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "tokens-fifo"
            os.mkfifo(path, 0o600)
            command = [sys.executable, "-c", "import importlib.util,sys; "
                       "s=importlib.util.spec_from_file_location('api',sys.argv[1]); "
                       "m=importlib.util.module_from_spec(s);sys.modules['api']=m;s.loader.exec_module(m); "
                       "m.Credentials(sys.argv[2])", str(ROOT / "app/dppd-api/main.py"), str(path)]
            result = subprocess.run(command, capture_output=True, timeout=3)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"unsafe_credentials_file", result.stderr)

    @unittest.skipUnless(os.name == "posix", "日志管道验证需要 Linux 文件描述符")
    def test_full_or_closed_audit_pipe_does_not_hold_requests_or_shutdown(self):
        import fcntl
        import select

        # 子进程使用生产 HTTP 服务和审计器，只把 daemon 替换为不做 I/O 的查询桩
        source = """
import importlib.util, sys, signal, threading, json, hashlib
spec=importlib.util.spec_from_file_location('api',sys.argv[1])
api=importlib.util.module_from_spec(spec);sys.modules['api']=api;spec.loader.exec_module(api)
class Backend:
    def execute(self, args): return {'api_version':'v1','status':0,'data':{'ready':True}}
credentials=object.__new__(api.Credentials)
credentials.tokens=[(hashlib.sha256(b'r'*64).hexdigest(),api.Principal('reader','1'))]
class CompletedServer(api.TCPServer):
    def process_request_thread(self, request, client_address):
        super().process_request_thread(request, client_address)
        print('handled',flush=True)
server=CompletedServer(('127.0.0.1',0),api.Handler)
server.initialize(api.Application(Backend()),credentials,2,0.3)
def stop(*args): threading.Thread(target=server.shutdown,daemon=True).start()
signal.signal(signal.SIGTERM,stop)
print(server.server_address[1],flush=True)
try: server.serve_forever(poll_interval=0.02)
finally: server.server_close()
print('closed',flush=True)
"""
        for mode in ("full", "closed"):
            child = subprocess.Popen([sys.executable, "-c", source, str(ROOT / "app/dppd-api/main.py")],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                port = int(child.stdout.readline())
                if mode == "full":
                    fcntl.fcntl(child.stderr.fileno(), fcntl.F_SETPIPE_SZ, 4096)
                else:
                    child.stderr.close()

                def call(auth):
                    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
                    headers = {"Authorization": "Bearer " + "r" * 64} if auth else {}
                    connection.request("GET", "/v1/health", headers=headers)
                    response = connection.getresponse()
                    result = response.status, json.loads(response.read())
                    connection.close()
                    # 明确等待生产线程释放槽，日志背压测试不能把正常准入拒绝误认为日志阻塞
                    # ACK 仅在完整线程返回后发出，不重试请求或放宽每条 401/200 的断言
                    self.assertTrue(select.select([child.stdout], [], [], 2)[0], "request cleanup did not complete")
                    self.assertEqual(child.stdout.readline(), b"handled\n")
                    return result

                for _ in range(220 if mode == "full" else 4):
                    self.assertEqual(call(False)[0], 401)
                status, result = call(True)
                self.assertEqual(status, 200)
                metrics = result["data"]["gateway"]
                self.assertGreater(int(metrics["audit_dropped"]), 0)
                if mode == "closed":
                    self.assertTrue(metrics["audit_sink_failed"])
                child.terminate()
                self.assertEqual(child.wait(timeout=3), 0)
                self.assertEqual(child.stdout.read(), b"closed\n")
            finally:
                if child.poll() is None:
                    child.kill()
                    child.wait(timeout=3)
                child.stdout.close()
                if not child.stderr.closed:
                    child.stderr.close()


if __name__ == "__main__":
    unittest.main()
