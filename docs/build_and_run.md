# 构建与运行

## 依赖

- Linux；
- Meson 0.61+、Ninja、pkg-config；
- DPDK 21.11+，且 `pkg-config --modversion libdpdk` 可见；
- 至少一个主 lcore 和 `--queues` 个 worker lcore；
- VFIO/IOMMU、hugepage 和目标 PMD 所需固件/驱动。

本项目不提供 mock 构建。缺少 DPDK 会直接配置失败，以免把“能编译占位代码”误判为数据面可用。

## 构建与单元测试

```bash
bash scripts/build.sh
meson test -C build --print-errorlogs
```

手工方式：

```bash
meson setup build -Dtests=true
meson compile -C build
meson test -C build --print-errorlogs
```

## 端口和队列统计

```bash
./build/dppctl stats          # 全部端口、全部队列
./build/dppctl stats 0        # 端口 0 的全部队列
./build/dppctl stats 0 1      # 端口 0、队列 1
./build/dppctl stats all 1    # 全部端口的队列 1
```

管理协议为 v15，daemon 和 CLI 必须一起更新，v14 及更旧客户端返回 EPROTO。不存在的端口或
队列返回 ENOENT。队列编号是运行配置中的 RX/TX queue id，不是物理 CPU/lcore id。
这些计数记录本项目软件工作线程实际处理的报文，不是网卡硬件计数；硬件卸载后未进入
CPU 的报文不会计入。运行时初始化后从零开始，不跨进程重启保存，不提供清零操作。

接收包数、字节、畸形报文及策略丢弃归入口端口；发送和发送失败归出口端口。
`policy_drops` 分为 `rule_drops`、`no_route_drops`、`egress_drops`；`tx_drops` 分为
`tx_linearize_drops` 和 `tx_queue_drops`。畸形报文单独记录为 `rx_malformed`。
`rx_unsupported` 是协议未深入解析的数量，不代表报文一定被丢弃。
控制台和 telemetry `/dppd/stats` 保留整体统计，telemetry 同时返回新增丢弃原因。
各字段独立原子采样，运行中不同字段及多次查询可能处于不同瞬间；停止收包后可核对
端口、队列汇总与整体值。热路径不获取管理锁，按收包批次累计后更新计数器。

## 网卡能力画像与规则探测

```bash
./build/dppctl capability-show 0
./build/dppctl probe-drop 100 0 10
./build/dppctl probe-filter 101 0 tcp any any any 443 drop count refresh
./build/dppctl probe-cache-clear 0
```

画像读取启动设备信息和累计校验记录，不重新访问驱动。探测只校验完整候选规则，
不会安装对象、占用规则版本或保存快照；相同规则可使用最多五秒的诊断缓存，最后
添加 `refresh` 可强制重新校验。不支持答复仍表示诊断成功，脚本应检查输出中的
`hardware`；`software` 单独说明平台是否存在等价实现。正式安装始终重新校验。
恢复隔离期间可读画像，探测与清缓存被拒绝。字段及失效规则见 [详细说明](capability_probe.md)。

## 失败事件历史

```bash
./build/dppctl rule-history
./build/dppctl rule-history 10 70    # 在历史版本 70 中读取失败 ID 10 之后的一页
```

保留本进程最近 64 次完成态失败，每页最多 4 条；后续页使用上一页 `next-after` 和
首个回应的 `revision`，出现 `ESTALE` 时从零重读。`gap=yes` 表示游标后有记录已被覆盖。
成功请求不会清除历史，隔离期间仍可查询；重启后重新记录，不能复用旧进程游标。
Telemetry 使用 `/dppd/rule_history[,AFTER_EVENT_ID[,EXPECTED_REVISION]]`。
字段、覆盖和错误边界见 [失败事件历史](rule_history.md)。

## 历史规则安装耗时

```bash
./build/dppctl rule-latency
```

命令输出软件单条、软件整批和驱动创建的历史成功提交，包含有效/未知测量次数、涉及
规则数、均值、最小/最大值、十六个区间和 P50/P95/P99 上界。整批更新只记一个耗时，
已删除或回滚的成功提交保留，重启从本次安装重新累计。隔离期间仍可查询。
Telemetry 使用 `/dppd/rule_latency`，可附加一个分组名称。计时与只读边界、分位数
上界和饱和标志见 [历史耗时分布](rule_latency.md)。

## 普通双端口运行

daemon 启动时和主循环持续查询端口链路；`dppctl port-show PORT` 的 `link` 为
`up/down/unknown/unsupported`。观察到出口 down 后，worker 继续处理入口规则，
需要转发到该出口的报文释放并计入入口的 `policy_drops/egress_drops`；恢复 up 后
自动继续转发，规则、版本、COUNT 和 snapshot 不重建。主循环通常约 100 ms 一轮，
管理请求与系统调度会影响检测延迟；切换时已进入发送调用的报文仍可能完成。

初次链路查询返回 ENOTSUP 的 PMD 标记 unsupported，保留既有转发且不再查询其链路。
其他查询错误或已监控端口后来返回 ENOTSUP 时，daemon 停止并等待 worker，清理后
以失败码退出；修复设备问题后，从同一 snapshot 重启恢复。软件 worker 的出口保护
不约束已经硬件卸载的规则。

设备移除单独处理：为运行端口注册 `RTE_ETH_EVENT_INTR_RMV`，PMD 声明支持时启用
移除中断，同时轮询 `rte_eth_dev_is_removed` 和端口有效性。因此 `link=unsupported`
不会关闭移除检测。通知回调只写原子状态；worker 在下一次收发检查时结束循环，
主线程等待所有 worker 注销 QSBR 并返回，再清理规则、注销回调、关闭设备及回收
缓冲池。正在执行的回调导致注销返回 EAGAIN 时，主线程每 1 ms 重试，防止提前
释放回调仍引用的设备集合；其他注销错误记录日志、停止设备资源回收并返回失败。
驱动 stop/close 错误同样回传，使退出状态反映清理失败；close 失败时保留所有缓冲池，
避免释放仍可能由驱动持有的对象。stop 失败但所有 close 成功时可回收池，仍报告失败。
移除状态在本进程内保持，链路再次 up 不会重新启动 worker，启动期间收到移除
事件也会失败退出。恢复隔离期间同样检测设备，避免继续保留故障端口。

移除依赖 PMD 提供通知或状态，正在执行的 burst 仍受驱动的移除支持约束；尚未
实现总线热拔插访问保护、进程内重新枚举和队列重建，也未通过物理热拔插验收。
恢复设备后，使用原 `--state-path` 重启，保留规则内容、安装端口和 generation，
COUNT 从零重新计数。设备移除过程不主动改写已保存的快照。

退出时 daemon 会在停止、关闭所有端口后，释放缓冲池前记录各 NUMA socket 的
`mbuf pool ... available=... capacity=... in-use=...`。在 PMD 已归还全部描述符
和缓存对象后，`available` 应等于 `capacity`、`in-use` 应为 0；这可用于核对
本次进程的 mbuf 回收，不是运行中的空闲容量或全部进程内存泄漏检查。

```bash
sudo ./build/dppd \
  -l 0-4 -n 4 \
  -a 0000:31:00.0 -a 0000:31:00.1 \
  -- \
  --ports 0,1 --queues 4 --burst 64 --promisc
```

相邻 port id 构成端口对。上例建立 `0↔1`。如使用 `--ports 0,1,2,3`，则同时建立 `2↔3`。port id 是 EAL 探测后的 ethdev id，不能用 PCI BDF 替代。

常用应用参数：

```text
--ports LIST       相邻项组成双向端口对
--queues N         每端口队列数，也是 worker 数
--burst N          1..256
--mbufs N          每个已使用 NUMA socket 的 mbuf 数
--cache N          每 lcore mempool cache
--stats-ms N       控制台统计周期
--duration N       N 秒后退出；0 表示等待信号
--promisc          开启混杂模式
--enable-pdump     注册 pdump IPC 服务
--control-socket P 管理 socket 路径，默认 /tmp/dppd-control.sock
--rule-capacity N  desired rule 最大条数，默认 1024
--state-path P     启用 snapshot v2 自动保存与启动重放；默认禁用
```

生产式启动应让 state path 的父目录预先存在并仅允许 daemon 用户访问，例如：

```bash
install -d -m 0700 /var/lib/dppd
sudo ./build/dppd -l 0-2 -n 4 -- \
  --ports 0,1 --queues 2 --state-path /var/lib/dppd/rules.snapshot
```

首次启动会原子创建空 snapshot。后续启动先加载并校验文件，再按其中保存的 install
port 对全部规则执行 plan/validate/prepare/commit；任一规则失败就回滚本轮对象并拒绝
启动。v1 因没有 install port 会返回 `EPROTONOSUPPORT`，不会猜测默认端口。

若已有确认全部规则使用同一 ethdev 的 v1 snapshot，先在 daemon 停止时离线迁移；
`--install-port` 是目标 ethdev id，不是 PCI BDF。输入不会被改写：

```bash
./build/dppd-snapshot-migrate \
  --input /var/lib/dppd/rules.v1 \
  --output /var/lib/dppd/rules.snapshot \
  --install-port 0
```

v1 没有每条规则的端口信息，多端口旧快照不能安全使用这个统一映射工具。

## 跨进程恢复保护

为已有启动命令同时配置两个不同文件：

```text
--state-path /var/lib/dppd/rules.snapshot --recovery-path /var/lib/dppd/hardware.recovery
```

恢复保护默认禁用。启用后，驱动 create 前先保存涉及端口的身份；上次未确认清理的
硬件活动会阻止运行时初始化和快照重放。此时管理服务尚未开放，使用离线工具查看：

```bash
sudo ./build/dppd-recovery show /var/lib/dppd/hardware.recovery
```

TAP 记录支持实际内核规则的只读核对，使用 `show` 返回的修订号：

```bash
sudo ./build/dppd-recovery inspect /var/lib/dppd/hardware.recovery 2
```

查询只覆盖本地 TAP 的 multiq/ingress，普通模式对象归属仍为未知；接口消失或空列表不会
解除恢复保护。旧 v1 记录缺少定位时明确返回未知，见 [TAP 残留核对](recovery_inspection.md)。

新恢复文件 v4 保存逐次安装意图、创建/删除结果及候选坐标，`show` 增加 `attempt` 行，
`inspect` 增加 `correlation` 行。记录最多 256 槽，仅成功删除项可复用，满时拒绝新的
硬件安装。创建失败且没有 handle 时也保留待核对状态，详见 [逐次安装记录](recovery_attempts.md)。

独立适配后的 DPDK 21.11.9 TAP 可加 `--tap-owner-cookie`，将先落盘的 16 字节随机
标识随本地 DROP/QUEUE 创建并回读；旧驱动和 remote 配置明确拒绝。离线工具可按
标识关联对象，规则内容仍未核验，也不自动清理。构建与完整步骤见
[TAP 原生标识](tap_owner_cookie.md)。

按 PMD/设备的受支持方法完成外部清理，再使用查询返回的精确版本确认，随后重新启动。
工具自身不执行硬件清理；文件锁、损坏记录、部署与确认流程见 [跨进程恢复保护](recovery_guard.md)。

## 本机规则管理

`dppd` 启动后，使用同一次构建生成的 `dppctl` 访问管理 socket：

```bash
./build/dppctl ping
./build/dppctl health
./build/dppctl ready
./build/dppctl persistence-status
./build/dppctl persistence-flush
./build/dppctl reconcile-status
./build/dppctl reconcile-retry
./build/dppctl port-show 0
./build/dppctl list
./build/dppctl apply-drop 100 0 0 10 require
./build/dppctl apply-drop-batch 0 200 201 202
./build/dppctl apply-filter-drop \
  101 0 0 tcp 10.10.0.0/16 192.168.1.10/32 any 443 20 require
./build/dppctl apply-filter \
  102 0 0 tcp 10.20.0.0/16 192.168.2.10/32 any 443 \
  queue:0 count mark:7 priority:30 require
./build/dppctl count 102 1
./build/dppctl get 100
./build/dppctl rule-status 100
./build/dppctl rule-status 100 1
./build/dppctl delete 100 1
```

`apply-drop` 参数依次为 rule id、安装 ethdev port id、期望 generation、可选 priority 和 `prefer|require`。创建新规则时期望 generation 使用 `0`；更新、删除应传上次响应的 generation；需要无条件操作时可显式写 `any`。非默认路径通过 `--socket PATH` 指定。

`rule-status RULE_ID [EXPECTED_GENERATION]` 查询当前规则的实际后端、安装端口、COUNT 配置、
后端提交耗时和整个账本的保存状态。版本默认 `any`，也可指定精确非零版本，过期返回
`ESTALE`。纯软件批量更新的各成员共享一次整批耗时，输出 `install-scope=batch` 和条数。
查询不访问驱动、不清零计数、不保存快照；时钟不可用时显示 `install-ns=unknown`。
规则状态和耗时不写入快照，重放会测量本次安装，详见 [规则安装状态](rule_status.md)。

`apply-filter-drop` 在上述三个并发参数后增加协议、源/目的 CIDR、源/目的端口。协议支持 `ipv4|udp|tcp`；CIDR 和 L4 端口均可写 `any`。`ipv4` 不包含 L4 item，因此两个端口必须写 `any`，防止参数看似生效却被忽略。地址和端口会转换为 DPDK flow item 要求的网络字节序。

`apply-filter` 使用相同的 pattern 参数，随后指定 fate action：`drop` 或 `queue:N`。可选 modifier 支持 `count`、`mark:N`、`priority:N` 和 `prefer|require|software`，顺序不限但不能重复。`software` 显式选择平台软件 backend，仅可用于已实现等价语义的 DROP/MARK/COUNT；QUEUE 与 transfer 不会降级。客户端会将动作规范化为 `MARK → COUNT → fate`，因此不同参数排列不会产生不同的 canonical rule。

`apply-drop-batch PORT RULE_ID RULE_ID [RULE_ID [RULE_ID]]` 一次创建 2–4 条此前不存在的 Ethernet ingress DROP 规则。首版统一使用 `prefer`，每条 expected generation 固定为 `0`，并在一个 transaction 内完成全量校验、prepare、commit 和失败回滚；输出中的每条 generation 连续递增，transaction 相同。跨规则更新、删除及复杂 pattern/action 暂不属于该命令的语义范围。

`update-drop-batch PORT PRIORITY prefer|require|software RULE_ID GENERATION RULE_ID GENERATION [RULE_ID GENERATION [RULE_ID GENERATION]]` 一次完整替换 2–4 条已有规则为 Ethernet ingress DROP；旧 match/action 不会保留。每条必须提供精确非零 generation，不接受 `any`。例如最近 `list` 显示规则 200/201 的版本为 3/4 时：

```bash
./build/dppctl update-drop-batch 0 20 prefer 200 3 201 4
```

成功返回按输入顺序分配的连续新版本和同一个 transaction ID；即使内容相同也推进版本。旧规则和新计划全为软件时一次发布整批快照，满表也可更新，每个报文使用完整旧表或新表；发布前失败保留旧表和计数，新版本 COUNT 从零开始。每个 backend 的本地容量仍为 `--rule-capacity + 1`。硬件或混合更新仍需要额外空槽并使用失败补偿，只保证管理端 desired state 整批发布。`prefer` 保守检查硬件空间，允许整表软件替换的情况在 PMD 校验后若变成混合计划，还会复查软件临时空间，不足返回 `ENOSPC`。snapshot 落盘失败返回 `EUCLEAN` 时整批可能已经生效，应检查 `persistence-status`。详见 [批量更新](todo_batch_update.md)。管理协议为 v15，daemon 和 CLI 必须一起更新。

`count RULE_ID EXPECTED_GENERATION` 查询已发布 generation 对应的 COUNT。它不会重置 counter；规则不存在、generation 已过期、规则没有 COUNT，以及当前 backend 不支持查询会分别返回错误。硬件规则的 hits/bytes 由目标 PMD 的 `rte_flow_query()` 决定；软件规则由 classifier 的原子计数器返回。

`list` 每页返回最多 4 条规则摘要，并给出 `repository-generation`、`more` 和下一页命令。例如：

```text
repository-generation=9 total=6 page-count=4 more=yes next=104
next-command: list 104 9
```

第一页不指定 generation；后续页必须携带第一页返回的 repository generation。若翻页期间发生任何规则增删改，daemon 返回 `ESTALE`，客户端应从第一页重新读取，不能拼接跨版本结果。列表按稳定 rule id 升序，与 repository 内部槽位无关。

`get RULE_ID` 输出完整 canonical rule，包括 install port、domain、fallback、group、priority、所有 match 的地址/掩码和 L4 端口，以及按顺序排列的 MARK/COUNT/fate action。IPv4 mask 按原始点分十进制输出，不强制伪装成连续 CIDR。

`port-show` 返回启动时保存的 PMD 能力快照，包括端口/peer、驱动、representor、switch domain、NUMA socket、最大队列、RETA、RSS 和 RX/TX offload。`*-cap` 是 PMD 报告的能力上限，`*-enabled` 是 dppd 实际启用的子集；该命令不能替代针对具体规则的 `rte_flow_validate()`。

`persistence-status` 查询 snapshot 是否启用、dirty 状态、最近持久化 generation、
当前 repository generation 和最近存储错误。`persistence-flush` 显式重写当前完整
repository，用于修复 `EUCLEAN`。未指定 `--state-path` 时 status 为 `enabled=no`，
flush 返回 `EINVAL`。显式启用后，
status 在 management socket 开放时已经是恢复完成的 clean generation。

启动恢复的回滚若遇到 PMD 删除失败，daemon 会进入 recovery isolation：不启动 worker，
普通管理操作返回 `EUCLEAN`，允许 `reconcile-status/reconcile-retry` 和只读的
`health/ready`；健康查询仍能响应，但报告未就绪。修复 PMD/
设备问题后执行 retry；全部 residual 对象删除成功时 daemon 自动退出，随后应重新启动
以执行完整 snapshot 重放。该 retry 只能处理当前进程仍持有 handle 的对象；进程已崩溃
或被强制终止时，应使用目标 PMD/设备支持的复位或清理流程，不能直接使用全端口 flush。
可选的 `--recovery-path` 为这种跨进程情况保存线索并阻止未确认的重放，详见上述离线流程。

管理 socket 权限为 `0600`。v15 使用本机 C ABI，只用于同主机、同版本的 `dppd/dppctl`，不应直接暴露为网络协议。daemon 不会擅自删除启动前已存在的路径；异常退出后的残留 socket 应在确认旧进程不存在后由部署脚本清理。

`health` 成功响应时退出码为 0，`live=yes` 证明管理线程能够响应；`ready` 只有
`ready=yes` 才返回 0，否则打印全部未就绪原因并返回 1。两者都不修改规则或快照。
就绪检查读取实际线程状态、端口、已知链路、移除标记、恢复隔离和持久化状态；未配置
持久化不阻止就绪。链路查询不支持时单独输出 `links-unsupported`，不冒充链路 up。
详细语义、限制与复现命令见 [健康与就绪](health_readiness.md)。

## Telemetry

应用运行后，使用 DPDK 自带 telemetry 客户端连接对应 file-prefix 的 socket：

```bash
dpdk-telemetry.py
--> /dppd/stats
--> /dppd/rule_failures
--> /dppd/rules
--> /dppd/rule,100
```

`dppctl rule-metrics` 提供同一组本进程累计值和最近失败，在恢复隔离期间也可读。
`/dppd/rules` 每页返回最多 64 个升序 ID；继续查询用
`/dppd/rules,AFTER_ID,REPOSITORY_GENERATION`，过期版本返回 JSON null。
每个规则回应带完整副本的发布号，查询不接触驱动或 COUNT，也不触发保存。
原始错误、补偿错误、最终错误和账本是否完整发布分别记录，详见
[规则失败与 Telemetry](rule_telemetry.md)。运行参数含 `--no-telemetry` 时没有此服务。

## 抓包

启动 dppd 时添加 `--enable-pdump`，另一个终端运行：

```bash
bash scripts/capture_pdump.sh 0 /tmp/dppd-port0.pcapng
```

捕获会复制包，只用于诊断，不应作为性能测试配置。

## Representor / DPU

representor 的 EAL devargs 取决于 PMD。先使用厂商和 DPDK PMD 文档创建 PF/VF/SF representor，再查看 dppd 启动时输出的 `kind=representor`、switch domain 与 switch port。只有处于兼容 embedded-switch domain 的端点才可以规划 transfer rule。

当前软件数据面仍只运行 port-pair baseline；管理 API 可动态安装单条硬件规则并查询 COUNT，是否支持具体 match/action/query 以目标 PMD 的 `rte_flow_validate()` 和 `rte_flow_query()` 结果为准。

## 验收顺序

1. 单元测试通过；
2. 使用 `net_null`/虚拟 PMD 做进程生命周期 smoke test；
3. 双物理口小流量验证包内容和双向转发；
4. 多队列验证 RSS 分布、无乱序需求和 NUMA locality；
5. 持续压测并核对 NIC xstats 与 dppd drop；
6. 在目标 SmartNIC/DPU 上逐条 validate representor/transfer rule，再做规则规模和恢复测试。
