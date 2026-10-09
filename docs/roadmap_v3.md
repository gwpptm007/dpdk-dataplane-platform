# V3 路线图

## M0：可验证 baseline

交付：真实 DPDK 构建、双端口/多队列软件转发、parser 单测、统计、拓扑发现、同步 `rte_flow` 原语。

验收：目标 Linux 主机编译无告警；双向包内容一致；持续压力下无 mbuf 泄漏；退出顺序稳定；unsupported capability 返回明确错误。

2026-10-03 已补齐链路 down/up 的出口保护和自动恢复，以及设备移除通知/状态触发
的失败退出与快照重启。移除回调仅发布原子状态，worker 自行结束收发，主线程等待
线程和在途回调后清理。8 组事件/查询/启动故障测试及四组 worker 收发验证通过；
物理热拔插、总线访问保护、运行中重新枚举和队列重建仍待实现及验收。

## M1：能力规划与规则控制面

当前进度：rule repository、generation、幂等 CRUD、第一版 planner、transaction/rollback engine、真实 rte_flow adapter、software adapter、immutable classifier snapshot + DPDK QSBR、generation replacement、单规则 control service、受限批量创建/精确批量删除/跨规则原子更新与本机 management v15 API，以及 snapshot v2 写路径、fail-closed 启动重放、v1 单端口迁移和进程内 recovery isolation 已落地；跨进程自动 reconciliation、degraded recovery 与多端口迁移尚未实现。纯软件跨规则更新已支持一次快照发布、满表替换、分配失败保留旧表及并发分类验证。跨规则更新已通过 Linux + DPDK 21.11.9 构建、15 项单测及 net_ring 进程间验证，详见 [批量更新](todo_batch_update.md)。

持久化方面已完成 [versioned snapshot v2](persistence_snapshot_v2.md)、install port 保存、
control mutation 后的完整保存、dirty state、管理状态查询、`--state-path` 和全量硬件
事务重放；rollback 自身失败时可保留 handle 进入隔离模式并重试删除。`dppd-snapshot-migrate`
可将确认使用同一安装端口的 v1 文件显式转换为 v2。跨进程恢复已落地可选安装保护和
外部清理确认，支持 TAP 本地 TC 只读核对、逐次线索保存和可选原生标识关联；完整规则内容与作用范围证明、其他 PMD 枚举、
自动 reconciliation 与多端口 v1 迁移仍属于 M1 待办。

已登记延期项：[虚拟测试端口的规则执行能力](todo_virtual_flow_backend.md)。当前
`net_ring` 不实现 rte_flow；后续按 TAP 基础 flow、平台 software backend、物理
NIC/SmartNIC 验收三个阶段推进。

2026-10-03 已实现本机 management v11 的 `rule-status`，查询实际后端、安装端口、COUNT
配置和提交耗时；纯软件批量更新报告共享的整批耗时，查询只读，重放重新计时。
20/20 单测、两组 ring 状态场景、TAP 的真实 rte_flow 生命周期、四组 worker 收发及
隔离/健康回归通过，见 [规则安装状态](rule_status.md)。当时尚未提供规则 telemetry 和
失败分类，已在下述 v13 补齐；历史耗时分布已在下述 v14 实现。

2026-10-03 已补齐 management v12 的设备/固件/DPDK 画像、描述符信息和完整规则诊断
缓存，64 个共享槽位、五秒有效期、强制刷新、临时错误不缓存；正式安装仍新校验，
创建/删除尝试使全端口旧结果失效。21/21 单测、ring/TAP 进程、隔离/健康、四组
worker 收发与三个相关单测的 AddressSanitizer/UBSan 通过，见 [能力画像与探测](capability_probe.md)。
资源容量、任意 flow 组合矩阵、AGE/indirect/template/async 探测未完成。
2026-10-03 已完成 management v13 的规则失败分类、请求累计指标和完整规则 telemetry
副本。原始错误、补偿错误、最终错误与发布标志分别保留；修复单规则补偿失败未隔离
的缺口。23/23 单测、ring/TAP 查询与并发更新、三组在线/启动隔离、既有状态/健康/
能力/worker 回归及三个单测的 AddressSanitizer/UBSan 通过，见
[规则失败与 Telemetry](rule_telemetry.md)。

2026-10-04 已完成 management v14 的历史成功提交耗时分布和只读 telemetry。软件单条、
软件整批和驱动创建分开累计，整批只记一个样本；固定十六区间提供均值与 P50/P95/P99
上界，未知时钟和整数溢出显式标记。历史包含后来删除或被撤销的成功提交，重启重新累计。
24/24 单测、ring/TAP CLI/JSON 对齐与并发更新、三组恢复故障、状态/健康/四组 worker
收发，以及四个单测的 AddressSanitizer/UBSan 通过，见 [历史耗时分布](rule_latency.md)。

2026-10-09 已完成 management v15 的失败事件历史与只读 telemetry。最近 64 次完整
失败按独立事件 ID 分页，每页 4 条，精确版本、覆盖缺口和编号耗尽显式标记；隔离可读，
成功不清除，重启重新记录。25/25 单测、ring/TAP 的 130 次失败及 16 页完整对齐、三组
恢复故障、状态/健康/四组 worker 回归，以及四个单测的 AddressSanitizer/UBSan 通过，见
[失败事件历史](rule_history.md)。

2026-10-09 完成跨进程恢复的第一阶段：可选 `--recovery-path` 保存安装前端口线索，
同一文件持有生命周期锁，异常退出后阻止直接重放，外部清理完成后通过离线工具按精确
版本确认。正常退出还会清理创建报错但仍留下 handle 的对象。26/26 单测、ring 夹具与
真实 TAP 的 SIGKILL/恢复流程、三组隔离、健康和四组 worker 回归、四项 sanitizer
通过，见 [跨进程恢复保护](recovery_guard.md)。

2026-10-09 增加 TAP 本地只读残留核对和恢复记录 v2。内核接口、启动标识与命名空间
在安装前持久保存，离线工具按修订号读取 TC 规则；旧 v1 身份未知，干净后才自动升级。
27/27 单测、持久 TAP 崩溃后的内核列表对齐、接口替换/命名空间错配、ring/TAP 保护回归
和四项 sanitizer 通过，见 [TAP 残留核对](recovery_inspection.md)。

2026-10-09 增加恢复记录 v3，持久保存每次创建意图、结果、删除结果和 TAP 候选坐标。
256 个槽位只复用成功删除项，编号不复用；创建前后崩溃都保留不确定状态。28/28 单测、
三种真实创建窗口故障、同坐标外部规则替换、旧格式兼容和五项 sanitizer 通过，见
[逐次安装记录](recovery_attempts.md)。本地 TAP 的原生标识已在下述阶段补齐。

2026-10-09 完成本地 TAP DROP/QUEUE 原生标识：独立 DPDK 21.11.9 补丁与可选开关将
先落盘的随机 cookie 随规则一次创建，回读和离线检查均拒绝歧义；恢复记录 v4 兼容
v1/v2/v3。两套构建各 29/29 单测、真实崩溃窗口、同坐标替换、复制标识与六项 sanitizer
通过，见 [原生标识](tap_owner_cookie.md)。下一项是完整规则内容和作用范围核验；
remote、其他 PMD、自动精确清理和物理复位验收尚未完成。

交付：

- capability profile 与 PMD probe cache（本机诊断基础已完成）；
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

2026-10-03 已实现本机 `health/ready` 与工作线程异常结束监控。管理协议 v11 可查询
实际运行线程、端口/链路、设备移除、规则恢复隔离与快照 dirty，并返回全部未就绪
原因；隔离期间保留存活查询。19/19 单测、三个正式进程状态/存储恢复场景、两组
线程故障、四组 worker 收发和既有故障回归通过。远程探针、卡死心跳、主动网络探测
与其余生产化交付项仍待实现，见 [验证记录](health_readiness.md)。

验收：升级与回滚不破坏已声明策略；故障注入覆盖 PMD 错误、设备消失、控制面重启和 DPU 重启；发布包可复现。
