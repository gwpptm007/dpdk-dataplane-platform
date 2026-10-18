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

2026-10-18 已完成原始 M2 软件交付及 [整体验收](m2_acceptance.md)：独立双栈 exact
match、ACL、LPM/rewrite/neighbor、conntrack/NAT、规则批量 COUNT、IPv6 和
VXLAN/Geneve/GRE 解析已接通。QUEUE 的 RX 分流与当前软件路径不等价，已完成明确
拒绝的设计和策略矩阵验证，不用 TX 队列静默模拟。

系统与本地 TAP 适配 DPDK 21.11.9 各 50/50 单测、各十二步完整验收通过；ASan/UBSan
全部 50 项和正式单双队列 worker 通过。三份报告与同一 210 文件源码摘要一致。
八组真实 TAP 验收覆盖双栈、ACL、状态/NAT、ND、隧道、异常及重启，未遗留业务进程。
分片、校验和、多段包、老化和更新失败语义已登记；RA/SLAAC、重组、RELATED、HA、
隧道终端及硬件/性能目标仍属于后续扩展或 M3/M4，未计入本次完成。

以下为该阶段此前各次独立交付记录，历史待办应结合上面的最终验收结论阅读。

2026-10-17 完成 [IPv6 动态 ND](nd6.md)：NS/NA、动态解析与有界首包、NUD 可达性、
本机 DAD、VLAN 域隔离、静态优先和管理闭环接通。两套构建各 47/47 单测、sanitizer
全部 47 项、单双队列共享首包与 NAT66、三种 VLAN 域的真实 TAP 和异常重启验收通过。
动态状态不落盘，协议 v22。RA/SLAAC、Redirect、隧道及其余有状态/性能目标仍待推进。

2026-10-17 完成 [IPv6 连接跟踪与 NAT66](nat66.md)：双栈 TCP/UDP 会话与状态 ACL、
SNAT66、静态 DNAT66 和端口转发、完整校验和、地址族端口池隔离及回包还原已接通。
两套构建各 46/46 单测、相关 sanitizer、单双队列正式 worker 和真实三 TAP 的状态、
IPv4 共存、故障、重启及老化验收通过。协议 v21，规则/路由恢复而会话重启清空。
动态 ND 已在上述阶段补齐；RA/SLAAC、RELATED/ICMPv6、重组、hairpin、NPTv6、隧道和硬件/性能目标仍待推进。

2026-10-17 完成 [IPv6 解析、ACL 与转发](ipv6.md)：有界扩展头、128 位地址条件和范围
ACL、双栈最长前缀/静态邻居、MAC/Hop Limit 改写、CLI 与混合快照重启恢复已接通。
两套构建各 45/45 单测、十一项 sanitizer、单双队列 200 次 IPv6 在线切换和真实 TAP
的 TCP/UDP/ICMPv6、存储故障、批量 ACL 与 IPv4 NAT 共存验收通过。IPv6 会话/NAT 已在上述阶段补齐；
动态 ND 已在上述阶段补齐；RA/SLAAC、分片重组、完整扩展选项、ICMPv6 错误生成、隧道与性能目标仍未完成。

2026-10-17 完成 [静态 DNAT 与端口转发](dnat.md)：TCP/UDP 整地址、单端口和等长
端口段、回包公网还原、逻辑 ACL/路由、SNAT 静态端口保留和管理分页查询已接通。
两套构建各 43/43 单测、七项 sanitizer、单双队列正式 worker、真实 TAP 的独立与
共存、老化及崩溃重启验收通过。策略由启动参数固定，重启不恢复会话；运行时改策略、
hairpin、ICMP/RELATED、双向主动建连的一对一静态 NAT、HA 和硬件状态卸载仍未完成。

2026-10-17 完成 [TCP/UDP 跟踪与出站 SNAT](conntrack_nat.md)：双向会话、握手状态、
状态 ACL、管理、老化和资源回收先接通，再完成 SNAT、回包还原及完整校验和。两套
构建各 42/42 单测、六项 sanitizer、单双队列和真实 TAP 双向验收通过。会话重启清空，
规则和路由重放，NAT 策略保留启动参数。静态 DNAT 已在上述阶段补齐，IPv6 静态转发也已接通；IPv6 跟踪/NAT 已接通；ICMP/RELATED、会话复制、
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

2026-10-18 已完成 [环境盘点](m3_environment_20261018.md) 和可在当前环境交付的
[transfer 控制基础](m3_transfer.md)：带证据的角色解析、同域图、七种模板、正式规划/
安装、硬件 COUNT 有效性及驱动方向修复。虚拟环境覆盖控制逻辑和明确拒绝，
不登记为实机硬件验收。可重复运行入口、验证记录和未完成项见 [M3 验收](m3_acceptance.md)。
已增加 [独立功能模拟服务](m3_simulation.md)，可用真实命令行和报文字节演练正向图、
COUNT、故障与重启，134 的私有工具和 DPDK 环境也已完成严格构建与核心验证。
已接通 [AGE 控制与模拟闭环](m3_age.md)：独立 AGE 查询、有限预算轮询、精确删除、
v4 快照和故障重试，协议 v25；三套同版本核心验收通过，未过期规则重启重新计时。
首期 [indirect 共享 COUNT](m3_shared_count.md) 已接通生产接口、独立版本与引用管理、
v5 单快照和协议 v26；三套各 60/60 单测、四步核心回归及 24 组功能模拟通过，源码摘要一致。
后续 [v7 共享恢复日志](m3_shared_recovery.md) 已支持普通恢复模式的对象与精确引用，
异常退出后保守阻止重放；TAP 原生/监护组合仍拒绝 indirect。共享 RSS/METER/AGE 及更新仍未实现。
新增 [独立复位与恢复模拟](m3_reset_simulation.md)：可选模拟回调成功才清除旧句柄，
保留完整内存期望计划和快照，按涉及端点的精确身份重新核对，先共享对象后规则，
失败隔离并显式恢复重试。COUNT 归零、AGE 重新计时；真实适配器默认不提供复位回调，
软件、三层、dirty 和 native 恢复日志组合拒绝。三套最终构建各 61/61 单测、四步核心
回归和 31 组模拟通过，237 项源码摘要一致；真实命令前端及退出清理也已通过。
真实 PMD 的 AGE/共享 COUNT、mlx5 与非 mlx5 卸载、CPU 旁路、端点移除/真实设备复位恢复仍需推进，
整个 M3 尚未完成。

共享恢复日志轮次三套各 63/63 单测、五步 M3 验收和 31 组模拟通过，241 项源码摘要一致。
该轮补齐真实 daemon 的日志和重启保护，不计为真实设备恢复或 ASIC 清理验收。

已接通 [正式设备事件与主动复位](m3_device_recovery.md)：在重放前登记 RESET/RMV/DESTROY，
事件发生后停止 worker 并隔离未知旧句柄；正式 v27 `device-status/device-reset` 支持完整组的
管理口保护、精确身份与临时拓扑核对、stop/reset/队列重建/start，以及共享对象先行恢复。
该流程调用真实 DPDK 接口，不借用独立模拟复位回调。未知旧对象仍须外部清理与干净重放，
部分 PMD 失败不保证原地重试，软件与三层联合恢复不在首期支持范围。上述代码与测试夹具
不能替代下面的实际设备、外部流量、CPU 旁路和恢复报告，整个 M3 保持未完成。
本轮四套各 69/69 单测、六步 M3、四组正式设备事件及 31 组模拟通过，252 项源码一致；
135 系统与本地 TAP 适配各十二步完整 M2 回归，ASan/UBSan 与 134 GCC 13 各四步核心通过。
后续已补齐 [通用 PCI 与接口身份核对](m3_generic_identity.md)，非零 ifindex 的端点可以保留
可信接口与 UNKNOWN 角色；没有宿主接口的目标须另有身份适配，六个命名模板仍缺非 mlx5
可信角色来源。pair 允许 UNKNOWN 不代表任意 PMD 均支持，具体版本缺口见
[21.11.9 能力矩阵](m3_pmd_capabilities_21_11.md)。

新增 [独立实机验收工具](m3_hardware_acceptance.md)：只读预检、外部报文、COUNT、AGE、
共享 COUNT 与可选完整组主动复位 runner、能力矩阵和报告 schema。三套各 70/70 单测、
58 项门禁、六步 M3 和 31 组模拟通过，255 项源码一致。两机实际预检仍为 blocked，
报告通过 schema 核对，未执行物理 reset；真实移除和双类设备实机证据继续保留缺口。

通用身份轮次四套各 70/70 单测、68 项硬件门禁、六步 M3、四组正式事件和 31 组模拟通过，
255 项源码一致。135 系统/本地适配各十二步完整 M2/TAP 回归，ASan/UBSan 与 134 GCC 13
各四步核心通过，完整证据和源码摘要见 [通用身份验收](m3_generic_identity.md#本轮整体验收)。

无宿主接口的 DPDK 21.11 sfc/bnxt 已有 [显式 PCI-only 基础验收适配](m3_hardware_acceptance.md#显式-pci-only-基础阶段)，
仅 basic pair/私有 COUNT；实际 PCI、EAL、backer 与固件归属须独立核对，不补猜角色。
三套各 70/70 单测、76 项门禁、四步核心回归、六步 M3 和 31 组模拟通过，255 项源码一致；
七项 schema 正负例通过。两机真实预检仍 blocked，未取得实机正向或 CPU 旁路证据。

后续 [DPDK 版本兼容验收](m3_dpdk_compatibility.md) 已补齐新版本方向、Telemetry API/字段键与测试夹具差异。
21.11.9 的系统/ASan/GCC 13 和私有 24.11.8 四套各 70/70 单测、76 项门禁、四步核心回归、
六步 M3 与 31 组模拟通过，255 项最终源码一致；实际动态库和真实公共 API 已核对。
版本升级与 mlx5 PMD 成功安装不等于实机验收，整体 M3 仍保持未完成。

交付：representor 角色解析、同 switch-domain 拓扑校验、transfer rule graph、uplink/PF/VF/SF 场景模板、counter/age/indirect action。

验收：至少在 NVIDIA mlx5/BlueField 和一个非 mlx5 PMD 上形成可复现实机报告；证明 offloaded packets 不经过 CPU worker；端点移除和 device reset 后可 reconciliation。

## M4：规模化 flow API

已建立 [软件基线与固定验收契约](m4_acceptance.md)，包含 16384 条规则的规模、安装速率、
控制面 P99、队列完成延迟、残留清理和快照重放门槛。基准复用正式控制面，内存驱动与
实际软件后端分别计时，不冒充 ASIC 速率。三套同源各 70/70 单测及核心/M3 回归通过，
后续模板、异步队列、配额和共享动作等实现及固定性能目标仍待完成，整体 M4 尚未完成。

后续已增加 [全局/端口配额基础组件](m4_admission.md)，支持整批预留、残留保留、凭据防复用、
精确恢复释放、动态额度和真实线程竞争；三套各 71/71 单测及核心/M3 回归通过，261 项源码一致。
模板和队列尚须接入此组件，旧接口不会因为库已存在就自动获得新额度限制。

[稳定槽位索引](m4_index.md) 已通过 16384 条规模、安装速率、控制面 P99、快照重放及残留清理门槛，
固定标准未变。三套各 72/72 单测与核心/M3 回归通过，264 项源码一致；
异步队列完成延迟和其他 M4 功能尚待验收。

[模板表生命周期与原生适配](m4_templates.md) 已接入全局/端口配额、固定掩码与可变值、
独立引用凭据和部分失败残留清理。24.11 调用实际模板 API，21.11 明确拒绝；
三套各 74/74 单测与核心/M3 回归通过，270 项源码一致，异步队列联动继续推进。

[有界异步 flow 生命周期](m4_async_queue.md) 已接通模板、实际资源配额、push/pull、
精确乱序完成、取消、超时保留和暂态恢复。24.11 调用实际异步 API；三套各 76/76 单测
与核心/M3 回归通过，275 项源码一致。整批发布、共享联动和 AGE 队列回收继续待做。

[软件整批发布基础](m4_software_publication.md) 已支持 64 条混合增删改、满表腾位、
分配失败保留原 COUNT，以及模板协调器 pin。真实 QSBR reader 跨 1000 次发布检查完整视图；
三套各 77/77 单测与核心/M3 回归通过，276 项源码一致，批量协调器继续推进。

[M4 异步批量协调器](m4_flow_service.md) 已接通模板、逻辑/实际配额、八个有界应用队列、
64 条创建/删除、完整期望账本和正式软件一次发布。部分删除按旧版本恢复，补偿失败保留任务并隔离，
暂态队列恢复可保留无关已提交规则。三套各 78/78 单测与核心/M3 回归通过，280 项源码一致。
异步更新、共享/AGE 联动、新路径报文集成及固定队列性能指标继续推进，整体 M4 尚未完成。

[队列性能及规模验收](m4_queue_performance.md) 已达到冻结的 4096 条/每批 64 条提交到完成 P99 门槛，
八个应用队列全部达到 256 深度，五轮中位数 4.664 毫秒；额外 16384 条完整安装及删除通过。
三套各 78/78 单测与核心/M3 回归通过，282 项源码一致，最终同源联合验收仍须在功能收尾后执行。

[M4 正式 worker 报文闭环](m4_worker_traffic.md) 已验证创建/删除各阶段完整视图、MARK、COUNT、
分配失败补偿、逐字节转发和缓冲池归还。三套各 224 包、78/78 单测及核心/M3 回归通过，
284 项源码一致；24.11 实际 net_ring 原生能力拒绝已核对，不登记为硬件正向。

[M4 队列 AGE 自动回收](m4_async_age.md) 已接入严格队列、独立老化额度、精确通知缓存、
公平预算及有界维护任务。用户不领取完成回执也不会阻塞维护删除，失败保留并退避或隔离重试。
三套各 79/79 单测、224 包新路径及核心/M3 回归通过，285 项源码一致；
队列完成 P99 五轮中位数 4.533 毫秒，仍通过冻结门槛。共享及异步更新继续推进。

[M4 共享 COUNT 联动](m4_shared_count.md) 已接通对象配额、模板示例引用、动态动作槽、
逻辑 pin、实际 flow、AGE 和失败残留。三套各 81/81 单测、224 包及核心/M3 回归通过，
292 项源码一致；队列 P99 五轮中位数 4.999 毫秒。异步更新及最终同源联合验收继续推进。

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
