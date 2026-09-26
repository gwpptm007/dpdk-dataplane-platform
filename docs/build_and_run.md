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

## 普通双端口运行

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

## 本机规则管理

`dppd` 启动后，使用同一次构建生成的 `dppctl` 访问管理 socket：

```bash
./build/dppctl ping
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
./build/dppctl delete 100 1
```

`apply-drop` 参数依次为 rule id、安装 ethdev port id、期望 generation、可选 priority 和 `prefer|require`。创建新规则时期望 generation 使用 `0`；更新、删除应传上次响应的 generation；需要无条件操作时可显式写 `any`。非默认路径通过 `--socket PATH` 指定。

`apply-filter-drop` 在上述三个并发参数后增加协议、源/目的 CIDR、源/目的端口。协议支持 `ipv4|udp|tcp`；CIDR 和 L4 端口均可写 `any`。`ipv4` 不包含 L4 item，因此两个端口必须写 `any`，防止参数看似生效却被忽略。地址和端口会转换为 DPDK flow item 要求的网络字节序。

`apply-filter` 使用相同的 pattern 参数，随后指定 fate action：`drop` 或 `queue:N`。可选 modifier 支持 `count`、`mark:N`、`priority:N` 和 `prefer|require|software`，顺序不限但不能重复。`software` 显式选择平台软件 backend，仅可用于已实现等价语义的 DROP/MARK/COUNT；QUEUE 与 transfer 不会降级。客户端会将动作规范化为 `MARK → COUNT → fate`，因此不同参数排列不会产生不同的 canonical rule。

`apply-drop-batch PORT RULE_ID RULE_ID [RULE_ID [RULE_ID]]` 一次创建 2–4 条此前不存在的 Ethernet ingress DROP 规则。首版统一使用 `prefer`，每条 expected generation 固定为 `0`，并在一个 transaction 内完成全量校验、prepare、commit 和失败回滚；输出中的每条 generation 连续递增，transaction 相同。跨规则更新、删除及复杂 pattern/action 暂不属于该命令的语义范围。

`update-drop-batch PORT PRIORITY prefer|require|software RULE_ID GENERATION RULE_ID GENERATION [RULE_ID GENERATION [RULE_ID GENERATION]]` 一次完整替换 2–4 条已有规则为 Ethernet ingress DROP；旧 match/action 不会保留。每条必须提供精确非零 generation，不接受 `any`。例如最近 `list` 显示规则 200/201 的版本为 3/4 时：

```bash
./build/dppctl update-drop-batch 0 20 prefer 200 3 201 4
```

成功返回按输入顺序分配的连续新版本和同一个 transaction ID；即使内容相同也推进版本。旧规则和新计划全为软件时一次发布整批快照，满表也可更新，每个报文使用完整旧表或新表；发布前失败保留旧表和计数，新版本 COUNT 从零开始。每个 backend 的本地容量仍为 `--rule-capacity + 1`。硬件或混合更新仍需要额外空槽并使用失败补偿，只保证管理端 desired state 整批发布。`prefer` 保守检查硬件空间，允许整表软件替换的情况在 PMD 校验后若变成混合计划，还会复查软件临时空间，不足返回 `ENOSPC`。snapshot 落盘失败返回 `EUCLEAN` 时整批可能已经生效，应检查 `persistence-status`。详见 [批量更新](todo_batch_update.md)。管理协议为 v7，daemon 和 CLI 必须一起更新。

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
普通管理操作返回 `EUCLEAN`，仅允许 `reconcile-status` 和 `reconcile-retry`。修复 PMD/
设备问题后执行 retry；全部 residual 对象删除成功时 daemon 自动退出，随后应重新启动
以执行完整 snapshot 重放。该 retry 只能处理当前进程仍持有 handle 的对象；进程已崩溃
或被强制终止时，应使用目标 PMD/设备支持的复位或清理流程，不能直接使用全端口 flush。

管理 socket 权限为 `0600`。v4 使用本机 C ABI，只用于同主机、同版本的 `dppd/dppctl`，不应直接暴露为网络协议。daemon 不会擅自删除启动前已存在的路径；异常退出后的残留 socket 应在确认旧进程不存在后由部署脚本清理。

## Telemetry

应用运行后，使用 DPDK 自带 telemetry 客户端连接对应 file-prefix 的 socket：

```bash
dpdk-telemetry.py
--> /dppd/stats
```

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
