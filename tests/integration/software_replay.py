#!/usr/bin/env python3
"""Verify nonempty software snapshots across actual daemon stop/start cycles."""

import argparse
import errno
import os
from pathlib import Path
import subprocess
import tempfile

from batch_update import check, fields
from recovery_isolation import stop, wait_for


def run_case(build, count, policy):
    with tempfile.TemporaryDirectory(prefix="dppd-replay-") as temporary:
        directory = Path(temporary)
        sock, state = directory / "ctl.sock", directory / "state.bin"
        cpus = sorted(os.sched_getaffinity(0))
        check(len(cpus) >= 2, "two CPUs required")
        command = [str(build / "dppd"), f"--lcores=0@{cpus[0]},1@{cpus[1]}",
                   "--main-lcore=0", "--no-huge", "--no-pci", "-m", "64",
                   f"--file-prefix={directory.name}", "--vdev=net_ring0",
                   "--vdev=net_ring1", "--", "--ports", "0,1", "--queues", "1",
                   "--mbufs", "1024", "--cache", "64", "--rule-capacity", str(count),
                   "--control-socket", str(sock), "--state-path", str(state)]
        environment = dict(os.environ, LC_ALL="C")
        environment.pop("LD_PRELOAD", None)

        def ctl(*args, error=None):
            result = subprocess.run([str(build / "dppctl"), "--socket", str(sock),
                                     *map(str, args)], capture_output=True, text=True,
                                    timeout=5, env=environment)
            if error is None:
                check(result.returncode == 0, result.stderr)
            else:
                check(result.returncode != 0 and f"({-error})" in result.stderr,
                      result.stdout + result.stderr)
            return result.stdout

        saved = None
        ids = list(range(800, 800 + count))
        pairs = [value for pair in zip(ids, range(count + 1, 2 * count + 1)) for value in pair]
        for stage in range(3):
            log = directory / f"daemon-{stage}.log"
            with log.open("w") as output:
                daemon = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                          env=environment)
            successful = False
            try:
                wait_for(daemon, sock.exists, "management socket", seconds=15)
                ctl("ping")
                if stage == 0:
                    ctl("apply-drop-batch", 0, *ids)
                    initial = [value for pair in zip(ids, range(1, count + 1)) for value in pair]
                    ctl("update-drop-batch", 1, 25, policy, *initial)
                    saved = state.read_bytes()
                else:
                    check(state.read_bytes() == saved, "restore changed snapshot bytes")
                    page = fields(ctl("list").splitlines()[0])
                    check(page["total"] == str(count) and
                          page["repository-generation"] == str(2 * count), str(page))
                    for rule_id, version in zip(ids, range(count + 1, 2 * count + 1)):
                        rule = fields(ctl("get", rule_id).splitlines()[0])
                        check(rule["generation"] == str(version) and rule["install-port"] == "1"
                              and rule["priority"] == "25" and rule["fallback"] ==
                              ("software-only" if policy == "software" else "prefer-hardware"),
                              str(rule))
                        ctl("count", rule_id, version, error=errno.ENODATA)
                    persistence = fields(ctl("persistence-status"))
                    check(persistence["dirty"] == "no" and
                          persistence["persisted-generation"] == str(2 * count), str(persistence))
                    check(fields(ctl("reconcile-status"))["state"] == "ready", "not ready")
                if stage == 2:
                    stale = [value for pair in zip(ids, range(1, count + 1)) for value in pair]
                    ctl("update-drop-batch", 1, 30, policy, *stale, error=errno.ESTALE)
                    ctl("update-drop-batch", 0, 30, policy, *pairs)
                    rows = [fields(line) for line in ctl("list").splitlines()[1:]]
                    check([int(row["generation"]) for row in rows] ==
                          list(range(2 * count + 1, 3 * count + 1)), str(rows))
                successful = True
            except Exception:
                print(log.read_text(), flush=True)
                raise
            finally:
                stop(daemon)
                if successful:
                    check(daemon.returncode == 0, log.read_text())
                    check(not sock.exists(), "socket was not removed")
        damaged = bytearray(state.read_bytes())
        damaged[-1] ^= 1
        state.write_bytes(damaged)
        log = directory / "corrupt.log"
        with log.open("w") as output:
            daemon = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                      env=environment)
        try:
            daemon.wait(timeout=15)
            check(daemon.returncode != 0 and not sock.exists(), "corrupt snapshot accepted")
            check("snapshot restore failed" in log.read_text(), log.read_text())
            check(state.read_bytes() == damaged, "failure overwrote snapshot")
        finally:
            stop(daemon)
        print(f"PASS software replay count={count} policy={policy}: two restarts, versions, ports, "
              "post-restart update, corrupt snapshot rejection")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    args = parser.parse_args()
    for count in (2, 4):
        for policy in ("software", "prefer"):
            run_case(args.build_dir.resolve(), count, policy)


if __name__ == "__main__":
    main()
