#!/usr/bin/env python3
"""Exercise removal events/probes, callback lifetime, failed startup and snapshot replay."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from batch_update import check
from recovery_isolation import stop, wait_for


def run_case(build, mode, port=1):
    with tempfile.TemporaryDirectory(prefix="dppd-removal-") as temporary:
        directory = Path(temporary)
        sock, state, trigger = directory / "ctl.sock", directory / "state.bin", directory / "fault"
        cpus = sorted(os.sched_getaffinity(0))
        check(len(cpus) >= 2, "two CPUs required")
        command = [str(build / "dppd"), f"--lcores=0@{cpus[0]},1@{cpus[1]}",
                   "--main-lcore=0", "--no-huge", "--no-pci", "-m", "64", "--no-telemetry",
                   f"--file-prefix={directory.name}", "--vdev=net_ring0", "--vdev=net_ring1",
                   "--", "--ports", "0,1", "--queues", "1", "--rule-capacity", "2",
                   "--mbufs", "1024", "--cache", "0", "--control-socket", str(sock),
                   "--state-path", str(state)]
        clean_env = {key: value for key, value in os.environ.items()
                     if key != "LD_PRELOAD" and not key.startswith("DPPD_TEST_")}
        clean_env["LC_ALL"] = "C"
        fault_env = dict(clean_env,
                         LD_PRELOAD=str(build / "tests/integration/libdppd_removal_faults.so"),
                         DPPD_TEST_REMOVAL_FILE=str(trigger), DPPD_TEST_REMOVAL_MODE=mode,
                         DPPD_TEST_REMOVAL_PORT=str(port))

        def ctl(*tokens):
            return subprocess.run([str(build / "dppctl"), "--socket", str(sock), *map(str, tokens)],
                                  check=True, capture_output=True, text=True, timeout=5,
                                  env=clean_env).stdout

        def start(environment, log_name):
            with (directory / log_name).open("w") as output:
                return subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                        env=environment)

        saved_state = None
        daemon = start(fault_env, "failed.log")
        try:
            if mode in ("startup-event", "register-error"):
                daemon.wait(timeout=10)
                check(not sock.exists() and not state.exists(), "failed startup exposed management/state")
            else:
                wait_for(daemon, sock.exists, "management socket")
                ctl("apply-drop", 15001, 0, 0, 0, "software")
                ctl("apply-filter", 15002, 1, 0, "tcp", "192.168.100.2/32",
                    "192.168.100.1/32", "any", 10000, "drop", "count", "software")
                saved_rules = {rule_id: ctl("get", rule_id) for rule_id in (15001, 15002)}
                saved_state = state.read_bytes()
                if mode.endswith("no-link"):
                    check("link=unsupported " in ctl("port-show", port), "link API not unsupported")
                trigger.touch()
                daemon.wait(timeout=10)
                check(not sock.exists() and state.read_bytes() == saved_state,
                      "removal changed snapshot/socket")
            log = (directory / "failed.log").read_text()
            check(daemon.returncode == 1, log)
            check("available=1024 capacity=1024 in-use=0" in log, log)
            if mode == "register-error":
                check(f"port={port} removal callback registration failed: -12" in log, log)
                check("[removal-fixture] unregister port=0 rc=0" in log, log)
            else:
                check(f"[removal-fixture] unregister port={port} rc=0" in log, log)
                if mode.startswith("callback") or mode == "startup-event":
                    check(f"[removal-fixture] callback port={port} delivered" in log, log)
                    check(f"[removal-fixture] dispatch port={port} finished" in log, log)
                if mode.startswith("callback"):
                    check(f"[removal-fixture] unregister port={port} busy" in log,
                          "in-flight callback was not observed: " + log)
                if saved_state is not None:
                    check("device monitoring failed; stopping workers" in log, log)
            check("link query failed" not in log, "removal was detected only through link error")
        except Exception:
            print((directory / "failed.log").read_text(), flush=True)
            raise
        finally:
            stop(daemon)

        daemon = start(clean_env, "restored.log")
        try:
            wait_for(daemon, sock.exists, "restored management socket")
            if saved_state is not None:
                for rule_id, saved_rule in saved_rules.items():
                    check(ctl("get", rule_id) == saved_rule, "restored rule changed")
                check(state.read_bytes() == saved_state, "restart changed snapshot")
                check("dirty=no persisted-generation=2 current-generation=2" in ctl("persistence-status"),
                      "restart persistence is not clean")
                check("hits=0 bytes=0" in ctl("count", 15002, 2), "COUNT did not reset on restart")
            else:
                check("total=0 " in ctl("list"), "failed startup left rules")
            for current_port in (0, 1):
                check("link=up " in ctl("port-show", current_port), "restarted port not up")
        finally:
            stop(daemon)
        restored_log = (directory / "restored.log").read_text()
        check(daemon.returncode == 0 and not sock.exists(), restored_log)
        check("available=1024 capacity=1024 in-use=0" in restored_log, restored_log)
        print(f"PASS removal mode={mode} port={port}: exit=1, callbacks drained, mbufs returned, restart=clean")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    args = parser.parse_args()
    build = args.build_dir.resolve()
    for mode, port in [("callback", 0), ("callback", 1), ("callback-no-link", 1),
                       ("probe", 1), ("probe-no-link", 1), ("startup-event", 0),
                       ("startup-event", 1), ("register-error", 1)]:
        run_case(build, mode, port)


if __name__ == "__main__":
    main()
