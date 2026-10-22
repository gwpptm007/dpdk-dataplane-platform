#!/usr/bin/env python3
"""在正式 daemon 上验收北向鉴权、稳定 JSON、持久重试、TLS 与重启恢复"""

import argparse
from datetime import datetime, timezone
import hashlib
import http.client
import json
import os
from pathlib import Path
import secrets
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from verify_m2 import source_digest


class UnixConnection(http.client.HTTPConnection):
    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.host)


def wait(check, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(0.03)
    raise RuntimeError("北向验收等待超时")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--report-dir", type=Path, required=True)
    args = parser.parse_args()
    build, output = args.build_dir.resolve(), args.report_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binaries = [build / name for name in ("dppd", "dppctl", "dppd-flow-state")]
    report = {"success": False, "scope": "M5_formal_daemon_authenticated_northbound_v1", "source_sha256": source_digest(ROOT),
              "binary_sha256": {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in binaries},
              "steps": [], "hardware_acceptance": "not_run", "started_utc": datetime.now(timezone.utc).isoformat()}
    children, logs = [], []
    try:
        cpus = sorted(os.sched_getaffinity(0))
        if len(cpus) < 2:
            raise RuntimeError("至少需要两个可用 CPU")
        with tempfile.TemporaryDirectory(prefix="dppd-api-") as temporary:
            directory = Path(temporary)
            state, control, api_socket = directory / "state", directory / "control", directory / "api"
            tokens = {role: secrets.token_hex(32) for role in ("reader", "operator", "admin")}
            credentials = directory / "tokens.json"
            credentials.write_text(json.dumps({"version": 1, "tokens": [
                {"sha256": hashlib.sha256(token.encode()).hexdigest(), "role": role, "client": str(index + 1)}
                for index, (role, token) in enumerate(tokens.items())]}), encoding="utf-8")
            credentials.chmod(0o600)
            subprocess.run([str(binaries[2]), "init-software", str(state), "0", "16", "2", "16"],
                           check=True, capture_output=True, timeout=20)

            def launch(command, label):
                log = (output / (label + ".log")).open("w")
                logs.append(log)
                child = subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
                children.append(child)
                return child

            sequence = 0

            def daemon():
                nonlocal sequence
                sequence += 1
                return launch([str(binaries[0]), f"--lcores=0@{cpus[0]},1@{cpus[1]}", "--main-lcore=0",
                               "--no-huge", "--no-pci", "--no-shconf", "--no-telemetry", "-m", "128",
                               f"--file-prefix=m5-api-{os.getpid()}-{sequence}", "--vdev=net_ring0", "--vdev=net_ring1",
                               "--", "--ports", "0,1", "--queues", "1", "--mbufs", "2048", "--cache", "0",
                               "--control-socket", str(control), "--flow-state", str(state)], f"daemon-{sequence}")

            process = daemon()
            gateway = [sys.executable, str(ROOT / "app/dppd-api/main.py"), "--socket", str(control), "--dppctl",
                       str(binaries[1]), "--tokens", str(credentials)]
            api_process = launch([*gateway, "--unix", str(api_socket)], "api-unix")

            def request(method="GET", path="/v1/status", body=None, role="reader", auth=True, tls=None, raw=None):
                if tls is None:
                    connection = UnixConnection(str(api_socket), timeout=10)
                else:
                    port, context = tls
                    connection = http.client.HTTPSConnection("127.0.0.1", port, context=context, timeout=10)
                headers = {"Authorization": "Bearer " + tokens[role]} if auth else {}
                if body is not None or raw is not None:
                    headers["Content-Type"] = "application/json"
                connection.request(method, path, json.dumps(body) if raw is None and body is not None else raw, headers)
                response = connection.getresponse()
                value = json.loads(response.read())
                code = response.status
                connection.close()
                assert value["api_version"] == "v1", value
                return code, value

            def ready():
                if process.poll() is not None or api_process.poll() is not None:
                    raise RuntimeError("正式 daemon 或网关提前退出")
                if not api_socket.exists():
                    return False
                return request(path="/v1/ready")[0] == 200

            def complete(number, role="operator"):
                code, value = request(path=f"/v1/requests/{number}", role=role)
                if code == 202:
                    return None
                assert code == 200, (code, value)
                result = value["data"]
                if result["state"] != "complete":
                    return None
                assert result["durable"] is True and result["status"] == 0 and result["blocked"] is False, result
                return result

            wait(ready)
            assert api_socket.stat().st_mode & 0o777 == 0o600
            assert request(auth=False)[0] == 401
            assert request("POST", "/v1/recovery/flush", {}, role="operator")[0] == 403
            assert request("POST", "/v1/recovery/flush", raw='{"x":1,"x":2}', role="admin")[0] == 400
            assert request(path="/v1/status?command=remove")[0] == 400
            body = {"sequence": "1", "table": "1", "queue": 0,
                    "rule": {"id": "9007199254740993", "port": 0, "kind": "drop"}}
            assert request("POST", "/v1/rules/create", body)[0] == 403
            assert request("POST", "/v1/rules/create", body, role="operator")[0] in (200, 202)
            first = wait(lambda: complete(1))
            assert first["rules"][0]["id"] == "9007199254740993", first
            assert request("POST", "/v1/rules/create", body, role="operator")[1]["data"] == first
            changed = dict(body, rule=dict(body["rule"], id="2"))
            assert request("POST", "/v1/rules/create", changed, role="operator")[0] == 409
            assert request(path="/v1/requests/1", role="reader")[0] == 404
            report["steps"].append("authenticated_roles_typed_rules_and_persistent_idempotency")

            process.kill()
            assert process.wait(timeout=20) == -signal.SIGKILL
            control.unlink()
            assert request(path="/v1/ready")[0] == 503
            process = daemon()
            wait(ready)
            assert complete(1) == first
            assert request("POST", "/v1/rules/create", body, role="operator")[1]["data"] == first
            code, value = request(path="/v1/rules/9007199254740993")
            assert code == 200 and value["data"]["rule"]["generation"] == first["rules"][0]["generation"]
            report["steps"].append("daemon_sigkill_replay_and_gateway_reconnect_without_duplicate_mutation")

            configuration = request(path="/v1/configuration")[1]["data"]
            revision = configuration["revision"]
            definitions = {"sequence": "1", "revision": revision, "changes": [
                {"operation": "count_add", "id": "900", "port": 0, "domain": "ingress"},
                {"operation": "template_add", "key": "2", "capacity": 8, "dynamic_actions": 0,
                 "example": {"id": "1", "port": 0, "kind": "drop", "shared_count": {"id": "900", "generation": "0"}}}]}
            assert request("POST", "/v1/configuration", definitions, role="operator")[0] == 403
            assert request("POST", "/v1/configuration", definitions, role="admin")[0] in (200, 202)
            configured = wait(lambda: complete(1, "admin"))
            configuration = request(path="/v1/configuration")[1]["data"]
            shared = next(item for item in configuration["counts"] if item["id"] == "900")
            assert configured["applied"] and configuration["table_total"] == 2
            update = dict(body, sequence="2", table="2", generation=first["rules"][0]["generation"],
                          rule=dict(body["rule"], shared_count={"id": "900", "generation": shared["generation"]}))
            assert request("POST", "/v1/rules/update", update, role="operator")[0] in (200, 202)
            updated = wait(lambda: complete(2))
            count = request(path="/v1/counts/900?generation=" + shared["generation"])[1]["data"]
            assert count["hits"] == "0" and count["references"] > 0
            remove = {"sequence": "3", "queue": 0, "id": body["rule"]["id"], "generation": updated["rules"][0]["generation"]}
            assert request("POST", "/v1/rules/remove", remove, role="operator")[0] in (200, 202)
            wait(lambda: complete(3))
            assert request(path="/v1/rules/9007199254740993")[0] == 404
            assert request(path="/v1/stats?port=0&queue=0")[0] == 200
            report["steps"].append("admin_configuration_transaction_shared_rule_migration_exact_delete_and_metrics")

            certificate, key = directory / "tls.pem", directory / "tls.key"
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                            "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1", "-keyout", str(key),
                            "-out", str(certificate)], check=True, capture_output=True, timeout=20)
            key.chmod(0o600)
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                port = probe.getsockname()[1]
            tls_process = launch([*gateway, "--listen", "127.0.0.1", "--port", str(port), "--tls-cert", str(certificate),
                                  "--tls-key", str(key)], "api-tls")
            context = ssl.create_default_context(cafile=str(certificate))

            def tls_ready():
                assert tls_process.poll() is None
                try:
                    return request(path="/v1/ready", tls=(port, context))[0] == 200
                except ConnectionRefusedError:
                    return False

            wait(tls_ready)
            assert request(tls=(port, context), auth=False)[0] == 401
            try:
                request(tls=(port, ssl.create_default_context()))
                raise AssertionError("不受信任的服务器证书被意外接受")
            except ssl.SSLCertVerificationError:
                pass
            denied = subprocess.run([*gateway, "--listen", "0.0.0.0", "--port", str(port)],
                                    capture_output=True, timeout=10)
            assert denied.returncode != 0
            report["steps"].append("verified_tls_authentication_untrusted_certificate_and_remote_plaintext_rejection")
            for child in (tls_process, api_process, process):
                child.terminate()
                assert child.wait(timeout=20) == 0
            for log in logs:
                log.flush()
            for log in output.glob("*.log"):
                text = log.read_text(errors="replace")
                assert not any(token in text for token in tokens.values()), "审计日志泄露认证令牌"
            assert source_digest(ROOT) == report["source_sha256"], "验收期间源码改变"
            report["success"] = True
    except (OSError, ValueError, KeyError, AssertionError, RuntimeError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
        print(str(error), file=sys.stderr)
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.kill()
                child.wait(timeout=10)
        for log in logs:
            log.close()
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        path = output / "report.json"
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(path)
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
