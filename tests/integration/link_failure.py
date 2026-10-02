#!/usr/bin/env python3
"""Verify fail-stop/restart after explicitly injected link-query failures."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from batch_update import check
from recovery_isolation import stop, wait_for


def run_case(build, error):
    with tempfile.TemporaryDirectory(prefix="dppd-link-failure-") as temporary:
        directory = Path(temporary)
        sock, state, trigger = directory / "ctl.sock", directory / "state.bin", directory / "fault"
        cpus = sorted(os.sched_getaffinity(0))
        check(len(cpus) >= 2, "two CPUs required")
        command = [str(build / "dppd"), f"--lcores=0@{cpus[0]},1@{cpus[1]}",
                   "--main-lcore=0", "--no-huge", "--no-pci", "-m", "64", "--no-telemetry",
                   f"--file-prefix={directory.name}", "--vdev=net_ring0", "--vdev=net_ring1",
                   "--", "--ports", "0,1", "--queues", "1", "--mbufs", "1024", "--cache", "0",
                   "--control-socket", str(sock), "--state-path", str(state)]
        clean_env = dict(os.environ, LC_ALL="C")
        clean_env.pop("LD_PRELOAD", None)
        fault_env = dict(clean_env, LD_PRELOAD=str(build / "tests/integration/libdppd_link_faults.so"),
                         DPPD_TEST_LINK_FAULT_FILE=str(trigger), DPPD_TEST_LINK_ERROR=error)

        def ctl(*tokens):
            return subprocess.run([str(build / "dppctl"), "--socket", str(sock), *map(str, tokens)],
                                  check=True, capture_output=True, text=True, timeout=5,
                                  env=clean_env).stdout

        with (directory / "failed.log").open("w") as output:
            daemon = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT, env=fault_env)
        try:
            wait_for(daemon, sock.exists, "management socket")
            ctl("apply-drop", 15001, 0, 0, 0, "software")
            saved_rule, saved_state = ctl("get", 15001), state.read_bytes()
            trigger.touch()
            daemon.wait(timeout=10)
            log = (directory / "failed.log").read_text()
            check(daemon.returncode == 1, log)
            check("device monitoring failed; stopping workers" in log, log)
            expected_error = -19 if error == "ENODEV" else -5
            check(f"link query failed: {expected_error}" in log, log)
            check("available=1024 capacity=1024 in-use=0" in log, log)
            check(not sock.exists() and state.read_bytes() == saved_state, "fault changed snapshot/socket")
        except Exception:
            print((directory / "failed.log").read_text(), flush=True)
            raise
        finally:
            stop(daemon)

        with (directory / "restored.log").open("w") as output:
            daemon = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT, env=clean_env)
        try:
            wait_for(daemon, sock.exists, "restored management socket")
            check(ctl("get", 15001) == saved_rule, "restored rule changed")
            check(state.read_bytes() == saved_state, "restart changed snapshot")
            check("dirty=no persisted-generation=1 current-generation=1" in ctl("persistence-status"),
                  "restart persistence is not clean")
            check("link=up " in ctl("port-show", 0), "restarted port not up")
        finally:
            stop(daemon)
        check(daemon.returncode == 0 and not sock.exists(), "restart exit/socket cleanup failed")
        check("available=1024 capacity=1024 in-use=0" in (directory / "restored.log").read_text(),
              "restart mbufs not returned")
        print(f"PASS link error={error}: exit=1, workers stopped, mbufs returned, snapshot unchanged, restart=clean")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    args = parser.parse_args()
    for error in ("ENODEV", "EIO"):
        run_case(args.build_dir.resolve(), error)


if __name__ == "__main__":
    main()
