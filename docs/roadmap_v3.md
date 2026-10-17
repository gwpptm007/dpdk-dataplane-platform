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
外部清理确认，支持 TAP 本地 TC 只读核对、逐次线索保存、可选原生标识关联及内容/当前范围核验；接口实例连续性与删除时一致性、其他 PMD 枚举、
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
通过，见 [原生标识](tap_owner_cookie.md)。本地内容与当前作用范围核验已在下述阶段补齐；
remote、其他 PMD、自动精确清理和物理复位验收尚未完成。

2026-10-11 增加恢复记录 v5 与本地 TAP 内容/范围核验：创建前保存匹配条件、动作和
位置，创建后及离线读回严格核对，未知字段和共享动作明确拒绝匹配。兼容读取 v1–v4，
修复普通 remote 内部空动作路径。30 项单测、七项 sanitizer 和真实 TAP 场景见
[TAP 内容核验](tap_rule_verification.md)。

2026-10-11 完成 [只读精确清理计划](recovery_plan.md)，按精确 revision 汇总全部登记
端口，列出候选、拒绝原因、实际坐标及删除前提，跨端口重复标识和不完整观察均不能
进入候选。两套构建各 31/31 单测、三项 sanitizer、真实 TAP 与六个创建窗口场景通过。
后续增加 [查询窗口事件监视](recovery_observation_window.md)，拒绝中途变化和事件丢失，
真实验证同名同索引重建。两套构建各 32/32 单测、三项 sanitizer、八个窗口场景与恢复
回归通过。独立监护会话及失效证据已在下述阶段落地，仍不自动删除。

2026-10-11 增加 [独立接口监护](recovery_monitor.md)，在业务启动前保存随机会话编号、
TAP 身份和恢复文件关联，业务崩溃后继续观察，明确记录删除、改名、丢包和文件替换。
监护崩溃后只读查询识别旧活动记录已中断，暂停场景仍报告新鲜度未验证。两套构建各
33/33 单测、四项 sanitizer 和各九个真实 TAP 场景通过。业务绑定在下述阶段补齐，
会话尚未接入清理计划，不构成删除许可。

2026-10-11 完成 [业务与监护握手](recovery_monitor_handshake.md)，新增可选
`--recovery-monitor`，核对全部 TAP 身份与恢复文件起点，把会话持久绑定至 v6。
安装前后和主循环必须取得新鲜回复，失败锁存、退出并用真实 handle 清理；未完成意图
保留待核对状态。两套构建各 34/34 单测、五项 sanitizer 和各 12 个真实场景通过。
独占登记和只读交接已在下述阶段完成，外部写入隔离和精确删除仍待完成。

2026-10-11 完成 [受控写入者登记与恢复交接检查](recovery_writer_handoff.md)，以 pidfd
区分退出与暂停，通过持锁描述符和精确 revision 核验单个恢复租约，工具崩溃后释放。
IPC 升级 v2，磁盘格式不变；普通信号不会重置超时或误报监护失联。两套构建各 35/35
单测、六项 sanitizer 和各十个真实交接场景通过。同命名空间核验和 `plan-monitored`
已将只读查询放入会话租约，并在查询前后核对活性。随后补充
[systemd 网络命名空间与 capability 部署范例](recovery_namespace_deployment.md)，并在隔离
TAP 命名空间验证无 `CAP_NET_ADMIN` 的进程可以读取和订阅链路，但不能修改接口，接口
状态保持不变。此范例只约束按配置启动的服务，尚未形成由程序检查的完整写入者清单，
也未补齐启动前历史证明；计划仍标记 `writer-exclusion=no executable=no`，不执行跨进程删除。

2026-10-17，增加无 capability 的普通用户监护回归。两套构建各通过十五个真实 TAP
场景，其中六个直接核对监护进程 UID/GID、空附加组、五个 capability 集为零和禁止获得
新权限，覆盖停止、中断、接口重建及改名、恢复文件替换和事件丢失。恢复工具的只读查询
也使用清空全部 capability 集的普通用户回归通过，详见[部署权限验证](recovery_namespace_deployment.md)。

2026-10-17 已接通[受控本地 TAP 恢复闭环](recovery_managed_scope.md)：在新建私有网络
命名空间内落实写入权限与接口连续性，业务退出后精确删除、独立日志逐条提交、失败
续接及快照重启。普通离线计划保持只读；不把这项能力推广为其他 PMD 或实体设备恢复。

交付：

- capability profile 与 PMD probe cache（本机诊断基础已完成）；
- rule repository、稳定 ID、generation 与幂等增删改查；
- hardware/software plan 和可解释 fallback reason；
- 多规则 validate/prepare/commit/rollback；
- telemetry 中的规则状态、安装延迟和失败分类。

验收：部分创建失败可回滚；进程重启可重放期望状态；`REQUIRE_HARDWARE` 永不静默降级；`PREFER_HARDWARE` 只有语义等价时才降级。

## M2：可组合软件 pipeline

2026-10-17 完成 [TCP/UDP 跟踪与出站 SNAT](conntrack_nat.md)：双向会话、握手状态、
状态 ACL、管理、老化和资源回收先接通，再完成 SNAT、回包还原及完整校验和。两套
构建各 42/42 单测、六项 sanitizer、单双队列和真实 TAP 双向验收通过。会话重启清空，
规则和路由重放，NAT 策略保留启动参数。静态 DNAT、IPv6、ICMP/RELATED、会话复制、
硬件有状态处理及性能目标未计为完成。

2026-10-17 完成 [IPv4 软件 ACL 闭环](acl.md)：允许/拒绝、地址与端口范围、协议号、
优先级、MARK/COUNT、正式 CLI、两到四条规则原子更新及持久重放均已接通。两套构建
各 41/41 单测、真实 TAP 范围/分片/版本/重启/同步故障、单双队列 MARK/COW 与既有
二层/三层回归通过。TCP/UDP 跟踪和出站 SNAT 已在上述阶段补齐，其余边界仍待后续实现。

2026-10-17 完成 [IPv4 三层转发闭环](ipv4_routing.md)：路由与静态邻居原子快照、
最长前缀、改写、显式 daemon 模式、CLI 管理和持久重启恢复均已接通。两套构建各
40/40 单测，三 TAP 真实收发和存储故障、四端口在线更新及旧二层回归通过。
该三层阶段覆盖 IPv4 LPM/rewrite/static-neighbor，软件 ACL、TCP/UDP 跟踪与出站 SNAT
已在上述阶段补齐，其他 stage 和完整 NAT 家族未计为完成。

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
