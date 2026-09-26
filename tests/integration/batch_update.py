#!/usr/bin/env python3
"""Exercise the real daemon/CLI with net_ring; no NIC binding or sudo required."""

import argparse
import errno
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time


def fields(text):
    return dict(re.findall(r"([\w-]+)=([^\s]+)", text))


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def run_case(build, lcores, count, capacity):
    with tempfile.TemporaryDirectory(prefix="dppd-batch-") as temporary:
        directory = Path(temporary)
        sock = directory / "control.sock"
        log = directory / "daemon.log"
        prefix = directory.name
        command = [str(build / "dppd"), "-l", lcores, "--no-huge", "--no-pci",
                   "-m", "64", "--file-prefix=" + prefix,
                   "--vdev=net_ring0", "--vdev=net_ring1", "--",
                   "--ports", "0,1", "--queues", "1", "--mbufs", "1024",
                   "--cache", "64", "--rule-capacity", str(capacity),
                   "--control-socket", str(sock), "--state-path", str(directory / "state.bin")]
        environment = dict(os.environ, LC_ALL="C")
        with log.open("w") as output:
            daemon = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                      env=environment)
        successful = False
        try:
            deadline = time.monotonic() + 15
            while not sock.exists():
                check(daemon.poll() is None, "daemon exited during startup")
                check(time.monotonic() < deadline, "daemon startup timed out")
                time.sleep(0.05)

            def ctl(*args, error=None):
                result = subprocess.run([str(build / "dppctl"), "--socket", str(sock),
                                         *map(str, args)], capture_output=True, text=True,
                                        timeout=5, env=environment)
                if error is None:
                    check(result.returncode == 0, result.stderr)
                else:
                    check(result.returncode != 0 and f"({-error})" in result.stderr,
                          f"expected errno {error}: {result.stdout}{result.stderr}")
                return result.stdout

            ids = list(range(700, 700 + count))
            created = ctl("apply-drop-batch", 0, *ids)
            rows = [fields(line) for line in created.splitlines()[1:]]
            check(len(rows) == count, created)
            check(all(row["backend"] == "0" for row in rows), created)
            versions = [int(row["generation"]) for row in rows]
            pairs = [value for pair in zip(ids, versions) for value in pair]
            updated = ctl("update-drop-batch", 0, 20, "prefer", *pairs)
            rows = [fields(line) for line in updated.splitlines()[1:]]
            check(len(rows) == count and len({row["transaction"] for row in rows}) == 1,
                  updated)
            check(rows[0]["transaction"] != "0", updated)
            for index, row in enumerate(rows):
                check(row["rule"] == str(ids[index]) and row["backend"] == "0" and
                      int(row["generation"]) == count + index + 1, updated)
                rule = fields(ctl("get", ids[index]).splitlines()[0])
                check(rule["priority"] == "20" and
                      rule["generation"] == row["generation"], str(rule))
            ctl("update-drop-batch", 0, 30, "prefer", *pairs, error=errno.ESTALE)
            pairs = [value for pair in zip(ids, range(count + 1, count * 2 + 1))
                     for value in pair]
            # Same contents still allocate a fresh consecutive generation range.
            repeated = ctl("update-drop-batch", 0, 20, "prefer", *pairs)
            rows = [fields(line) for line in repeated.splitlines()[1:]]
            check([int(row["generation"]) for row in rows] ==
                  list(range(count * 2 + 1, count * 3 + 1)), repeated)
            pairs = [value for pair in zip(ids, range(count * 2 + 1, count * 3 + 1))
                     for value in pair]
            ctl("delete-batch", *pairs)
            page = fields(ctl("list").splitlines()[0])
            check(page["total"] == "0", str(page))
            expected_revision = count * 4
            check(page["repository-generation"] == str(expected_revision), str(page))
            persistence = fields(ctl("persistence-status"))
            check(persistence["enabled"] == "yes" and persistence["dirty"] == "no",
                  str(persistence))
            successful = True
        except Exception:
            print(log.read_text(), flush=True)
            raise
        finally:
            if daemon.poll() is None:
                daemon.send_signal(signal.SIGINT)
                try:
                    daemon.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    daemon.kill()
                    daemon.wait()
                    raise AssertionError("daemon did not stop within 10 seconds")
            if successful:
                check(daemon.returncode == 0, log.read_text())
                check(not sock.exists(), "daemon left its control socket behind")
        print(f"PASS net_ring count={count} capacity={capacity}: lifecycle, snapshot, cleanup")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--lcores", default="0,1", help="two available EAL lcores")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    for count, capacity in [(2, 4), (4, 8), (2, 2), (4, 4)]:
        run_case(build, args.lcores, count, capacity)


if __name__ == "__main__":
    main()
