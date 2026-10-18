#!/usr/bin/env python3
"""正式工具必须能检查及恢复配置，同时拒绝覆盖、损坏和错误参数"""

import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    executable = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="dppd-state-cli-") as directory:
        path = Path(directory) / "state"

        def run(*arguments, success):
            result = subprocess.run([executable, *map(str, arguments)], capture_output=True, text=True, timeout=30)
            assert (result.returncode == 0) == success, (arguments, result.stdout, result.stderr)
            return result

        run("init-software", path, 7, 64, 2, 64, success=True)
        original = path.read_bytes()
        run("init-software", path, 7, 8, 1, 8, success=False)
        assert path.read_bytes() == original
        result = json.loads(run("check", path, success=True).stdout)
        assert result == {"mode": 1, "port": 7, "queues": 2, "depth": 64, "templates": 1,
                          "shared_counts": 0, "rules": 0, "revision": 0, "restored": False,
                          "hardware_acceptance": "not_run"}
        assert json.loads(run("restore-check", path, success=True).stdout)["restored"] is True
        assert path.read_bytes() == original
        path.write_bytes(original[:-1])
        run("check", path, success=False)
        run("restore-check", path, success=False)
        run("init-software", Path(directory) / "bad", 7, 0, 2, 64, success=False)
        run("init-software", Path(directory) / "bad", 7, 64, 9, 64, success=False)
        run("init-software", Path(directory) / "bad", "-1", 64, 2, 64, success=False)
    print("PASS M5 CLI initialization inspection empty replay overwrite corruption and bounds")


if __name__ == "__main__":
    main()
