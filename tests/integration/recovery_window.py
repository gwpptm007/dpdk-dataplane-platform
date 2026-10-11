"""真实查询窗口内注入修改和同索引重建，所有资源均由调用方测试创建"""

import errno
import fcntl
import os
from pathlib import Path
import signal
import struct
import subprocess

from batch_update import check
from health_readiness import clean_environment
from recovery_isolation import wait_for


def run_window(build, guard, revision, mutate, expected_error=errno.ESTALE, mode="pause", changed="tc"):
    """正式 CLI 的测试入口在首次查询后暂停，恢复后必须保留文件并撤销失效候选"""
    state = guard.parent / "storage/state.bin"
    before = guard.read_bytes(), state.read_bytes()
    process = subprocess.Popen(
        [str(build / "tests/integration/test_recovery_plan_window"), "plan", str(guard), str(revision)],
        env=dict(clean_environment(), DPPD_TEST_PLAN_WINDOW=mode),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    try:
        wait_for(process, lambda: "State:\tT" in Path(f"/proc/{process.pid}/status").read_text(), "plan window pause")
        mutate()
        process.send_signal(signal.SIGCONT)
        output, error = process.communicate(timeout=15)
        check(process.returncode != 0 if expected_error else process.returncode == 0, output + error)
        check((guard.read_bytes(), state.read_bytes()) == before, "observation window changed persisted evidence")
        check("cleanup-confirmed=no executable=no" in output and "history-covered=no writer-exclusion=no" in output,
              output)
        if expected_error:
            check(f"({-expected_error})" in error and "candidates=0" in output and "decision=candidate" not in output,
                  output + error)
            status = "unavailable" if mode == "lost" else "changed"
            check(f"observation-window status={status} error={-expected_error}" in output, output)
            if mode != "lost":
                check(f"{changed}-changed=yes" in output, output)
        else:
            check("observation-window status=no-change-observed error=0" in output and "candidates=4" in output, output)
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate(timeout=10)


def verify_plan_windows(build, guard, revision, interfaces, owned, run, filters):
    """修改后恢复原值仍必须失效，无关接口的普通链路通知则不误伤候选"""
    name = interfaces[0]
    extra = f"dw{os.getpid()}x"
    check(not Path(f"/sys/class/net/{extra}").exists(), "window test interface already exists")
    run("ip", "tuntap", "add", "dev", extra, "mode", "tap", "multi_queue")
    owned[extra] = Path(f"/sys/class/net/{extra}/ifindex").read_text()

    def alias_roundtrip(target):
        old = Path(f"/sys/class/net/{target}/ifalias").read_text().strip()
        run("ip", "link", "set", "dev", target, "alias", "dppd-window-test")
        run("ip", "link", "set", "dev", target, "alias", old)

    run_window(build, guard, revision, lambda: None, expected_error=0)
    run_window(build, guard, revision, lambda: alias_roundtrip(extra), expected_error=0)
    run_window(build, guard, revision, lambda: alias_roundtrip(name), changed="link")
    row = filters(name)[0]
    cookie = row["options"]["actions"][0]["cookie"]
    coordinate = ["dev", name, "parent", "1:", "protocol", row["protocol"], "pref", row["pref"],
                  "handle", row["options"]["handle"], "flower"]

    def replace_roundtrip():
        run("tc", "filter", "replace", *coordinate, "action", "skbedit", "queue_mapping", 1, "cookie", cookie)
        run("tc", "filter", "replace", *coordinate, "action", "drop", "cookie", cookie)

    run_window(build, guard, revision, replace_roundtrip)
    action_index = filters(name)[0]["options"]["actions"][0]["index"]

    def action_roundtrip():
        run("tc", "actions", "change", "action", "gact", "pass", "index", action_index, "cookie", cookie)
        run("tc", "actions", "change", "action", "gact", "drop", "index", action_index, "cookie", cookie)

    run_window(build, guard, revision, action_roundtrip)

    def unrelated_tc_roundtrip():
        run("tc", "qdisc", "add", "dev", extra, "root", "handle", "1:", "multiq")
        run("tc", "qdisc", "del", "dev", extra, "root")

    run_window(build, guard, revision, unrelated_tc_roundtrip)
    run_window(build, guard, revision, lambda: None, expected_error=errno.ENOBUFS, mode="lost")
    fresh = run(build / "dppd-recovery", "plan", guard, revision)
    check("candidates=4 rejected=0 excluded=1" in fresh, "roundtrip did not restore original content: " + fresh)
    print("PASS recovery observation windows: quiet, unrelated link, link ABA, filter ABA, direct action ABA, "
          "namespace TC changes, lost notifications", flush=True)


def verify_same_index_recreation(build, guard, revision, name, owned, run):
    """删除测试自有接口后显式复用旧索引，证明相同名字和索引并不代表同一实例"""
    original_index = owned[name]

    def recreate():
        check(Path(f"/sys/class/net/{name}/ifindex").read_text() == original_index, "interface replaced externally")
        run("ip", "link", "delete", "dev", name)
        del owned[name]
        fd = os.open("/dev/net/tun", os.O_RDWR | os.O_CLOEXEC)
        try:
            # Linux TUNSETIFINDEX / TUNSETIFF / TUNSETPERSIST，EXCL 禁止接管并发出现的同名接口
            fcntl.ioctl(fd, 0x400454DA, struct.pack("I", int(original_index)))
            request = struct.pack("16sH22x", name.encode(), 0x0002 | 0x0100 | 0x1000 | 0x8000)
            fcntl.ioctl(fd, 0x400454CA, request)
            fcntl.ioctl(fd, 0x400454CB, 1)
            owned[name] = Path(f"/sys/class/net/{name}/ifindex").read_text()
            check(owned[name] == original_index, "kernel did not reuse requested index")
        finally:
            os.close(fd)

    run_window(build, guard, revision, recreate, changed="link")
    fresh = run(build / "dppd-recovery", "inspect", guard, revision)
    check("status=coordinates-match complete=yes error=0 filters=0" in fresh, fresh)
    print("PASS same-name same-ifindex TAP recreation: window invalidated, later coordinate check alone "
          "cannot prove historical continuity", flush=True)
