# 本机健康与就绪查询

2026-10-03 最初随管理协议 v10 实现。当前协议为 v12，支持 [规则安装状态](rule_status.md)
和 [能力画像与探测](capability_probe.md)。daemon 与 CLI 必须来自同一次构建，v11 及更旧版本返回 `EPROTO`。
仍采用同机 C 结构体协议，不提供 HTTP 或远程健康接口。

## 使用方式

```bash
./build/dppctl --socket /tmp/dppd-control.sock health
./build/dppctl --socket /tmp/dppd-control.sock ready
```

两个命令读取同一个只读快照。`health` 在 daemon 能响应时返回 0，即使当前
`ready=no`；`ready` 只有 `ready=yes` 才返回 0，未就绪时打印同样的诊断并返回 1。
无法连接、超时、协议不兼容或请求失败时，两者都返回 1，不会输出 `live=yes`。

正常响应示例：

```text
health live=yes ready=yes blockers=0x0 reasons=none workers=1/1 workers-failed=0 ports=2/2 links-up=2 links-down=0 links-unknown=0 links-unsupported=0 recovery=ready persistence-enabled=yes persistence-dirty=no persisted-generation=2 current-generation=2 rules=2 persistence-error=0 recovery-error=0
```

`live=yes` 表示管理主线程成功处理了这次请求。`ready=yes` 表示配置要求的线程与
端口已运行，已知链路没有断开或未知状态，设备未移除，规则无需恢复隔离，已启用的
快照也没有未保存状态。未指定 `--state-path` 不阻止就绪，结果显示
`persistence-enabled=no`。

## 就绪条件与诊断

| 原因 | 含义 |
|---|---|
| `no-runtime` | 没有可用于判断转发状态的运行实例 |
| `stop-requested` | 主线程已经发出停止请求 |
| `device-removed` | 任一端口或设备组收到移除标记 |
| `workers-not-running` | 启动未完成、线程数量不足或线程已停止 |
| `worker-failed` | 工作线程初始化或返回结果失败 |
| `ports-not-started` | 端口数量不足，或任一端口尚未完成配置和启动 |
| `link-down` | 至少一个端口报告链路断开 |
| `link-unknown` | 至少一个端口的链路状态尚未确认 |
| `recovery-required` | 规则处于 reconciliation-required 或 restart-required |
| `persistence-dirty` | 已启用的快照尚有内存状态未成功保存 |

所有原因可同时返回，`blockers` 为对应的位集合。`workers=实际运行数/配置要求数`，
运行数取自线程完成 QSBR 读者注册后发布的状态，不使用成功提交的启动请求数。
`ports=完成配置并启动数/配置要求数`。端口编号可以不连续，统计按设备记录遍历。

端口首次链路查询返回 `ENOTSUP` 时，保留原有转发行为，并计入
`links-unsupported`，不计入 `links-up`，也不单独阻止就绪。此时链路未经验证，
`ready=yes` 不能当作物理连接已建立的证明。

`current-generation` 是内存中的完整规则账本版本，`persisted-generation` 是最近
成功保存的版本。保存失败后规则可能已经生效，但 `ready=no`，诊断包含
`persistence-dirty` 和具体存储错误。修复存储并执行 `persistence-flush` 后重新查询。
探针不会自行写盘、删除规则或触发清理重试。

恢复隔离期间允许 `health/ready` 查询，主线程仍可响应，转发服务保持未就绪。
普通规则操作继续返回 `EUCLEAN`；残留对象仍需通过 `reconcile-status/reconcile-retry`
检查与清理。探针不会退出隔离，也不会启动工作线程。

## 工作线程故障监控

主线程成功提交远程启动请求后，工作线程仍可能在 QSBR 注册阶段失败。线程发布
`FAILED` 状态，主循环检查到异常后停止其他线程、等待返回，再按既有顺序清理规则
和设备，最终返回退出码 1。没有停止或移除请求时意外进入结束状态，也按故障处理。

正常停止和设备移除保留各自的处理流程，普通停止后可以重新启动线程；设备移除后
仍拒绝进程内再次启动，须重启并重建运行实例。状态在生命周期切换时发布，不增加
逐包的状态写入。

该检查没有线程心跳、包级进展检查或主动网络探测，不能发现一直没有返回的卡死
线程，也不证明端到端可达、吞吐或硬件卸载正确。链路和线程为独立原子采样，查询
描述当时观察到的状态，不保证这些条件持续不变。

## 验证记录

环境：Ubuntu 22.04.5、GCC 11.4、Meson 0.61.2、DPDK 21.11.9。

- `-Werror` 全量构建和 19/19 单测通过；新增测试覆盖线程未完成启动、实际注册失败、
  正常停止、端口未配置、数量不符、移除、down/unknown/unsupported、dirty、恢复隔离
  及 v9 拒绝。失败注册使用正式 worker 入口，不自行模拟其状态发布。
- 单队列 `net_ring`、双队列 `net_null` 和单队列双 TAP 的三个正式 daemon 场景通过：
  只读探针不改变 generation、COUNT 或快照；保存失败返回 `ready=no`，修复并 flush
  后就绪恢复；CLI 退出码、两规则重启恢复、缓冲池与 socket 清理均正确。
- 双 TAP 通过实际接口 down/up 验证 `ready=yes → no → yes`，断开期间线程保持
  运行，快照字节不变，退出后两个临时 TAP 均不存在。TAP 预分配接收描述符，因此
  使用 8191 个 mbuf；虚拟 ring/null 场景使用 1024，退出时均全部归还。
- 一队列和两队列的两个线程注册故障进程通过：远程启动成功后第一线程注册失败，
  正式主循环发现异常，停止并等待其余线程，退出码 1，缓冲池全部归还、socket 删除。
  包装函数只链接到测试入口，不链接到正式 daemon。
- 四组正式 worker 收发通过，每组完成 1001 次更新，运行中就绪、停止后未就绪，
  重放、计数、丢弃原因、移除自行结束和 2048 个 mbuf 回收均通过。
- 两组恢复隔离、八组设备移除和两组链路查询错误进程回归通过。隔离的健康查询
  可响应但未就绪；链路查询不支持场景明确返回未验证链路数量。

`net_ring` 不支持 RSS，正式 daemon 的双队列能力要求仍然有效。双线程进程状态
验证使用 `net_null`，实际双队列收发使用已有夹具显式配置的 ring 队列。这些结果不
作为物理 NIC RSS 分布、物理热拔插或真实硬件 flow 故障验收。

复现命令：

```bash
meson setup build -Dtests=true -Dwerror=true
meson compile -C build
meson test -C build --print-errorlogs
python3 tests/integration/health_readiness.py --build-dir build
sudo python3 tests/integration/health_readiness.py --build-dir build --tap
python3 tests/integration/software_traffic.py --build-dir build
python3 tests/integration/recovery_isolation.py --build-dir build
python3 tests/integration/removal_failure.py --build-dir build
python3 tests/integration/link_failure.py --build-dir build
```

基础探针与故障场景可用普通用户运行，需至少两个可用 CPU；有三个可用 CPU 时
自动增加双线程检查。`--tap` 需要 root，只操作本测试创建的两个临时接口，不修改
管理网卡或物理数据口。故障库仅向独立测试 daemon 显式加载。
