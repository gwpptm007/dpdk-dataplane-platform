# V3 路线图

## M0：可验证 baseline

交付：真实 DPDK 构建、双端口/多队列软件转发、parser 单测、统计、拓扑发现、同步 `rte_flow` 原语。

验收：目标 Linux 主机编译无告警；双向包内容一致；持续压力下无 mbuf 泄漏；退出顺序稳定；unsupported capability 返回明确错误。

## M1：能力规划与规则控制面

当前进度：rule repository、generation、幂等 CRUD、第一版 planner、transaction/rollback engine、真实 rte_flow adapter、software adapter、immutable classifier snapshot + DPDK QSBR、generation replacement、单规则 control service、受限批量创建/精确批量删除/跨规则原子更新与本机 management v7 API，以及 snapshot v2 写路径、fail-closed 启动重放、v1 单端口迁移和进程内 recovery isolation 已落地；跨进程 reconciliation、degraded recovery 与多端口迁移尚未实现。纯软件跨规则更新已支持一次快照发布、满表替换、分配失败保留旧表及并发分类验证。跨规则更新已通过 Linux + DPDK 21.11.9 构建、15 项单测及 net_ring 进程间验证，详见 [批量更新](todo_batch_update.md)。

持久化方面已完成 [versioned snapshot v2](persistence_snapshot_v2.md)、install port 保存、
control mutation 后的完整保存、dirty state、管理状态查询、`--state-path` 和全量硬件
事务重放；rollback 自身失败时可保留 handle 进入隔离模式并重试删除。`dppd-snapshot-migrate`
可将确认使用同一安装端口的 v1 文件显式转换为 v2。跨进程 residual flow reconciliation
与多端口 v1 迁移仍属于 M1 待办。

已登记延期项：[虚拟测试端口的规则执行能力](todo_virtual_flow_backend.md)。当前
`net_ring` 不实现 rte_flow；后续按 TAP 基础 flow、平台 software backend、物理
NIC/SmartNIC 验收三个阶段推进。

交付：

- capability profile 与 PMD probe cache；
- rule repository、稳定 ID、generation 与幂等增删改查；
- hardware/software plan 和可解释 fallback reason；
- 多规则 validate/prepare/commit/rollback；
- telemetry 中的规则状态、安装延迟和失败分类。

验收：部分创建失败可回滚；进程重启可重放期望状态；`REQUIRE_HARDWARE` 永不静默降级；`PREFER_HARDWARE` 只有语义等价时才降级。

## M2：可组合软件 pipeline

交付：exact match、ACL、LPM、rewrite、neighbor、conntrack/NAT 独立 stage；批量计数；IPv6 和常用 tunnel parser。

其中 software backend 需优先覆盖 Ethernet/IPv4/UDP/TCP、DROP/MARK/COUNT；
QUEUE 是否存在软件等价语义必须先完成设计，不能静默模拟。

验收：每个 stage 有独立单元/属性测试；更新时无读写竞争；fragment、checksum、多段 mbuf 和 session aging 有明确语义；软件结果可与 rule IR 对照。

## M3：SmartNIC/DPU transfer

交付：representor 角色解析、同 switch-domain 拓扑校验、transfer rule graph、uplink/PF/VF/SF 场景模板、counter/age/indirect action。

验收：至少在 NVIDIA mlx5/BlueField 和一个非 mlx5 PMD 上形成可复现实机报告；证明 offloaded packets 不经过 CPU worker；端点移除和 device reset 后可 reconciliation。

## M4：规模化 flow API

交付：template table、async flow queue、资源配额、批量操作、aged flow 回收、shared action、背压与 admission control。

验收：定义并达到规则安装速率、规则规模、P99 控制面延迟和故障恢复时间目标；资源耗尽不会产生半提交状态。

## M5：生产化

交付：northbound API、鉴权、持久化、滚动升级、health/readiness、结构化日志、端口/队列/rule 指标、NUMA 自动布局和性能回归门禁。

验收：升级与回滚不破坏已声明策略；故障注入覆盖 PMD 错误、设备消失、控制面重启和 DPU 重启；发布包可复现。
