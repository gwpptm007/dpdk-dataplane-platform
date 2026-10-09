# dpdk-dataplane-platform

面向现代 DPDK 用户态数据面、SmartNIC/DPU、`rte_flow` 与 representor 的演进型平台基线。

V3 不再把“软件、硬件、transfer”误建模成逐包执行模式：软件路径处理送达 CPU RX queue 的包；`rte_flow` 和 embedded-switch transfer rule 属于控制面下发的硬件规则，硬件成功处理的包通常不会进入软件 worker；representor 是 PMD 暴露的 ethdev 拓扑端点，而不是一个软件转发后端。

当前可运行基线提供：

- DPDK 21.11+ 的 Meson 构建，不提供会掩盖问题的 mock 模式；
- 每队列一个 lcore 的 run-to-completion 软件路径；
- RSS、多 NUMA socket mbuf pool、burst RX/TX 与多段 mbuf 能力处理；
- Ethernet、双 VLAN、ARP、IPv4、UDP/TCP 的边界安全解析；
- immutable forwarding snapshot 和 per-worker/port/queue 原子统计；
- ethdev/representor/switch-domain 拓扑发现；
- 统一 rule IR 到 `rte_flow` 的真实 validate/create/query/destroy/flush；
- 带稳定 ID、generation、幂等更新和乐观并发控制的 rule repository；
- versioned rule snapshot v2、install port、CRC32、原子替换、dirty 写路径、fail-closed 启动重放、v1 单端口离线迁移和进程内 recovery isolation；
- 可解释的软硬件 planner，以及 validate/prepare/commit/rollback transaction engine；
- 连接 desired repository 与真实 `rte_flow` 对象仓库、支持 generation replacement 的进程内 control service；
- 版本化 Unix `SOCK_SEQPACKET` 管理接口和 `dppctl`，支持端口能力查询、generation 稳定分页、完整规则详情、单规则 apply/delete 及硬件 COUNT 查询；
- DPDK telemetry 的报文统计、完整规则副本、规则安装状态与失败分类，以及可选 pdump 服务；
- 本机 `rule-metrics` 保留原始错误、补偿错误、失败阶段与已生效标志，详见 [规则失败与 Telemetry](docs/rule_telemetry.md)；
- 本机 `rule-latency` 和 telemetry 汇总历史成功提交耗时，分别统计软件单条、软件整批与驱动创建，详见 [历史耗时分布](docs/rule_latency.md)；
- 本机 `rule-history` 和 telemetry 保留最近 64 次完整失败，提供分页、版本校验和覆盖缺口提示，详见 [失败事件历史](docs/rule_history.md)；
- 可选 `--recovery-path` 在硬件安装前保存恢复线索，异常退出后阻止直接重放，提供离线核对与外部清理确认，详见 [跨进程恢复保护](docs/recovery_guard.md)；
- 离线 `dppd-recovery inspect` 核对 TAP 本地实际 TC 规则和内核接口身份，保持未知归属与恢复保护，详见 [TAP 残留核对](docs/recovery_inspection.md)；
- 启用恢复保护时逐次持久保存安装意图、驱动结果、删除结果和 TAP 候选坐标，详见 [逐次安装记录](docs/recovery_attempts.md)；
- 本机网卡能力画像、完整规则探测和五秒诊断缓存，正式安装仍重新校验，详见 [能力画像与探测](docs/capability_probe.md)；
- 本机 `rule-status` 查询实际后端、安装端口、COUNT 配置和后端提交耗时，详见 [规则安装状态](docs/rule_status.md)；
- 本机 `health/ready`、全部未就绪原因、工作线程实际状态和异常结束监控，详见 [健康与就绪](docs/health_readiness.md)。

当前软件路径在 port-pair 基线上已支持 Ethernet/IPv4/UDP/TCP 的 DROP、MARK、COUNT：`prefer` 仅在硬件 validate 失败且语义等价时回退，`software` 可显式强制软件 backend；QUEUE 与 transfer 不会被错误降级。软件 classifier 以不可变 snapshot 原子发布，worker 每轮完整收包扫描后报告 DPDK QSBR 静默点，旧规则集在宽限期后异步回收，不阻塞逐包分类。管理 CLI 可构造 DROP/QUEUE/MARK/COUNT 规则，并提供 `apply-drop-batch`（2–4 条同端口 Ethernet DROP 新规则的原子创建）、`delete-batch`（2–4 条精确 generation 规则的原子删除）和 `update-drop-batch`（2–4 条精确旧 generation 规则完整替换为 Ethernet DROP，管理协议 v15）；旧规则和新计划全在软件后端时，批量更新通过一次快照发布完成，每个报文使用完整旧表或新表，满表也可更新；硬件及混合路径仍只保证规则账本整批发布。可用 `dppctl stats [PORT|all [QUEUE|all]]` 查询软件收发及丢弃原因，接收归入口、发送归出口。具体硬件规则仍以 PMD 的 `rte_flow_validate()` 为准。`--state-path` 可启用 snapshot v2 自动保存与 fail-closed 启动重放；确认所有旧规则来自同一 ethdev 时，可用 `dppd-snapshot-migrate --install-port` 离线转换 v1 文件。回滚删除失败时 daemon 开放 `reconcile-status/reconcile-retry` 和只读的 `health/ready/capability-show/rule-metrics/rule-latency/rule-history` 与规则 telemetry，清除已知 handle 后退出重启。默认不指定路径时保持禁用。跨进程自动 reconciliation、degraded recovery 和 flow template 是下一阶段工作，详见 [实现状态](docs/implementation_status.md) 与 [路线图](docs/roadmap_v3.md)。

## 快速开始

管理协议 v15 增加 `rule-history`，保留 `rule-latency`、`rule-metrics`、`capability-show`、`probe-drop/probe-filter` 和 `probe-cache-clear`，
保留 `rule-status` 的实际后端与提交耗时、`health/ready` 的线程状态和 `port-show` 的链路状态。
daemon 持续检测链路，出口断开时释放
需要转发的报文并记录 egress drops，恢复后沿用原规则、COUNT 和 snapshot 自动
继续转发；链路查询异常或设备移除通知触发停止收发、等待线程与回调结束后失败退出，
修复后可从快照重启。进程内重新枚举和热重配仍未实现，物理热拔插尚未验收。
daemon 与 CLI 需要一起更新，旧版本客户端返回 EPROTO。

通过 `dppctl rule-metrics` 可查询最近失败和本进程累计结果。Telemetry 提供
`/dppd/rule_failures`、`/dppd/rules` 和 `/dppd/rule,ID`，按完整副本发布规则状态，
驱动或保存操作尚未完成时仍可读取上一份完整状态。启动重放失败进入隔离时也可查询。
`dppctl rule-latency` 与 `/dppd/rule_latency[,software|software_batch|rte_flow]` 提供本进程
成功提交的次数、均值、区间和 P50/P95/P99 上界；删除或回滚保留历史，重启重新累计。
`dppctl rule-history [AFTER_EVENT_ID [EXPECTED_REVISION]]` 与 `/dppd/rule_history` 每页
最多返回 4 条完成态失败，包含原始/补偿/最终错误和生效标志；隔离可读，重启清空。
同时指定 `--state-path` 与 `--recovery-path` 可启用跨进程安装保护。上次硬件活动未确认
清理时启动失败，使用 `dppd-recovery show` 离线查看；完成外部设备清理后才能确认并重放。
保护默认禁用。TAP 可用 `dppd-recovery inspect PATH REVISION` 只读核对本地残留；
新恢复文件 v3 同时保存逐次安装线索，最多 256 个记录槽位，只有成功删除的槽位可复用。
候选坐标仍不能证明所有权；其他 PMD 的枚举、驱动原生归属标识和自动清理仍待实现。

```bash
bash scripts/build.sh
sudo ./build/dppd -l 0-2 -n 4 -- --ports 0,1 --queues 2 --burst 64 --promisc
```

`-l/-n/-a` 等参数属于 EAL；`--` 后属于 dppd。`--ports 0,1,2,3` 表示建立 `0↔1`、`2↔3` 两组双向端口对。每个 queue 需要一个 EAL worker lcore，主 lcore 不参与轮询。

完整环境准备、运行、telemetry 与抓包方式见 [构建与运行](docs/build_and_run.md)。架构语义和扩展边界见 [V3 架构](docs/architecture_v3.md)。

## 仓库边界

```text
app/dppd/            进程入口、信号与主循环
app/dppctl/          本机管理命令行客户端
include/dppd/        稳定的模块接口与数据模型
lib/runtime/         配置和生命周期
lib/device/          ethdev、NUMA、队列与拓扑
lib/control/         immutable snapshot、rule IR 校验
lib/management/      Unix socket 管理协议与服务端
lib/dataplane/       parser、software pipeline、worker
lib/offload/         rte_flow 编译与对象生命周期
lib/observability/   per-worker stats、telemetry
tests/unit/          无网卡单元测试
tests/integration/   双端口和 SmartNIC/DPU 验收说明
```

## 重要约束

- 当前兼容基线是 Linux + DPDK 21.11 LTS；升级版本通过独立构建矩阵验证。
- 是否支持 match/action、transfer、counter、representor 由 PMD 与设备决定；`rte_flow_validate()` 的失败是能力结果，不是可忽略告警。
- transfer rule 的两个端点必须处于兼容的 embedded-switch domain，不能仅凭 ethdev port id 猜测。
- 本仓库尚未在当前 Windows 工作区完成真实 DPDK 编译或线速硬件验证；请以目标 Linux 主机的构建与测试结果为准。
