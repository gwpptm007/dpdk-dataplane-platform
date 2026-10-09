# V3 实现状态

状态只表示代码是否真实存在，不以目录或占位接口计入完成度。

2026-10-09 完成持久逐次安装记录：恢复文件 v3 保存唯一尝试编号、规则版本、安装意图、
创建/删除结果和 TAP 单一新增候选；创建结果同步失败仍保留 handle 回滚，无 handle 的
创建失败不再允许正常退出自动确认 clean。256 槽只复用成功删除的记录，v1/v2 待确认
文件保持原格式。`-Werror`、28/28 单测、三个真实创建窗口故障、同坐标外部规则替换、
ring/TAP 保护与原隔离/健康回归、五项 sanitizer 通过，见 [逐次安装记录](recovery_attempts.md)。
这些是可追溯线索，仍不是所有权证明；下一项需要驱动原生可回读的归属标识与完整范围核验。

2026-10-09 完成跨进程恢复第二阶段的 TAP 本地只读核对：安装前保存内核接口、启动
标识和网络命名空间，离线 `inspect` 按精确修订号读取实际 TC 对象，识别环境不符、
同名接口替换及接口消失；所有对象归属仍标为未知，检查不解除恢复保护。恢复记录 v2
兼容读取 v1，干净旧文件才升级。`-Werror`、27/27 单测、自有持久 TAP 的真实崩溃残留
与内核列表对齐、ring/TAP 保护回归及四项 AddressSanitizer/UBSan 通过，见
[TAP 残留核对](recovery_inspection.md)。逐次安装线索已在上述阶段补齐，所有权证明仍待驱动适配；
其他 PMD、remote 范围和自动清理仍未实现。

2026-10-09 完成跨进程恢复第一阶段：可选 `--recovery-path` 在驱动 create 前持久化端口
身份并同步，旧记录未确认时阻止项目运行时初始化和快照重放；`dppd-recovery` 提供离线
查看与精确版本的外部清理确认，同一文件由进程持锁。修复退出漏删部分创建 handle 的
缺口。`-Werror`、26/26 单测、ring 夹具/真实 TAP 的 SIGKILL 与恢复、三组旧隔离场景、
健康/四组 worker 回归及四项 AddressSanitizer/UBSan 通过，见 [恢复保护](recovery_guard.md)。
第一阶段未枚举实际流表；本地 TAP 核对已在上述阶段补齐，自动复位或清理仍未实现。

2026-10-09 新增 management v15 的 `rule-history` 与 `/dppd/rule_history`。固定保留
最近 64 次完成态失败，每页 4 条，独立失败 ID 与历史版本校验，覆盖缺口和编号耗尽显式
标记；原始/补偿/最终错误和发布标志完整保留。成功操作不清除，隔离仍可读，重启清空。
`-Werror` 构建、25/25 单测、ring/TAP 的 130 次失败覆盖与 16 页 CLI/JSON 对齐、三组
恢复故障、状态/健康/四组 worker 回归，以及四项相关单测的 AddressSanitizer/UBSan
通过，见 [失败事件历史](rule_history.md)。跨进程恢复保护已在上述阶段落地，自动清理仍待实现。

2026-10-04 新增 management v14 的 `rule-latency` 与 `/dppd/rule_latency`。历史成功提交
按软件单条、软件整批和驱动创建分组，复用已有计时，提供十六区间、均值和 P50/P95/P99
上界；未知时钟单独计数，溢出显式标记。删除或回滚保留历史，隔离仍可读，重启重新累计。
`-Werror` 构建、24/24 单测、ring/TAP CLI/JSON 对齐与并发更新、三组恢复故障、状态/健康/
四组 worker 收发回归，以及四项相关单测的 AddressSanitizer/UBSan 通过，见
[历史耗时分布](rule_latency.md)。失败事件历史已在上述 v15 补齐；跨进程自动 reconciliation 仍待实现。

2026-10-03 新增 management v13 的 `rule-metrics` 和规则 telemetry。按公开控制请求
累计成功、失败、幂等、生效和回退，保留原始/补偿/最终错误、失败阶段与生效标志。
查询只读管理线程发布的完整副本，支持 64 ID 分页；启动重放隔离同样可查询。
补齐单规则补偿失败的隔离保护。`-Werror` 构建、23/23 单测、两组 ring/TAP telemetry、
三组在线/启动隔离、规则状态、健康、能力画像和四组 worker 收发通过；两个新单测与
事务单测通过 AddressSanitizer/UBSan，见 [规则失败与 Telemetry](rule_telemetry.md)。
该版本尚未提供历史耗时分布和失败事件列表，已分别在上述 v14、v15 补齐；跨进程自动 reconciliation 仍待实现。

2026-10-03 新增 management v12 的能力画像、完整规则探测和固定 64 槽的五秒诊断缓存。
设备、PMD、固件、DPDK 版本和描述符信息在启动时保存；临时错误不缓存，正式安装仍
重新校验，创建/删除尝试使全端口旧结果失效。恢复隔离仍允许只读画像，禁止探测与
清缓存。`-Werror` 构建、21/21 单测、ring/TAP 正式进程、规则状态、隔离、健康与四组
worker 收发回归通过，三个相关单测通过 AddressSanitizer/UBSan，见
[能力画像与验证](capability_probe.md)。资源容量、任意组合矩阵和高级 flow 能力仍未探测。

2026-10-03 新增 management v11 的 `rule-status`，按当前规则版本定位实际后端、安装端口、
COUNT 配置和后端提交耗时，核对账本与安装记录。纯软件批量更新显示共享的整批耗时，
查询不调用驱动、不改变计数、快照或版本；重放重新测量。`-Werror` 构建、20/20 单测、
两组 ring 状态/存储/重放场景、真实 TAP rte_flow 生命周期、四组 worker 收发及隔离和
健康回归通过，详见 [规则安装状态与验证](rule_status.md)。该版本尚未提供失败分类与规则 telemetry，已在上述 v13 补齐。

2026-10-03 新增 management v10 的 `health/ready`、工作线程实际状态和异常结束监控。
只读健康查询在恢复隔离期间仍可响应，未就绪时保留全部原因；存储修复和链路恢复后
就绪自动恢复。`-Werror` 构建、19/19 单测、三个正常/存储恢复进程场景、两组线程
注册故障、四组正式 worker 收发及既有隔离/移除/链路错误回归通过，详见
[健康与就绪验证](health_readiness.md)。线程状态不包含卡死心跳或端到端可达性检查。

2026-09-26 跨规则原子更新及 management v7 已通过 Ubuntu 22.04.5 / DPDK 21.11.9 下的 `-Werror` 全量构建、15/15 单测和四组 net_ring 进程间验证。验证了 2/4 条更新、满容量纯软件替换、旧版本拒绝、相同内容更新、混合路径容量拒绝、snapshot 状态和退出清理；新增并发分类及分配故障测试通过，并通过 AddressSanitizer/UBSan 检查；详见 [验证记录](validation_batch_update.md)。实际 RX/TX 验证也已补齐：正式 worker 在 net_ring 上处理 72456 个报文，两条和四条规则各成功更新 1001 次，DROP、COUNT、正常转发、分配失败后重试及 mbuf 回收通过普通和 AddressSanitizer/UBSan 构建验证。下方保留此前 v6 基线的硬件与 TAP 验证记录。

在线恢复隔离另有两组进程级 flow API 故障注入测试通过：确认 worker 停止、普通请求
阻断、清理重试失败与成功、失败码退出、旧 snapshot 重启恢复。该测试不涉及真实 PMD
故障；测试共享库独立构建且不链接到生产程序。

| 领域 | 当前状态 | 边界 |
|---|---|---|
| 构建 | 已验证 | Ubuntu 22.04、Meson 0.61.2、DPDK 21.11.9 下完成 `-Werror` 全量编译和链接 |
| 配置 | 已实现最小 CLI | 端口对、队列、burst、mbuf、统计、duration、promisc、pdump、管理 socket、规则容量 |
| ethdev | 已实现 baseline、链路恢复与移除保护 | capability 交集、RSS、per-socket pool、queue setup、start/stop；链路轮询、down 丢弃、up 恢复；移除通知/状态停止 worker，等待回调后退出重启；重新枚举、热重配与物理热拔插验收未完成 |
| 软件 worker | 已实现 | 每 queue 一个 lcore，所有端口 burst RX/TX，mbuf ownership 完整；实际运行状态发布与异常结束监控 |
| parser | 已实现 baseline | 双 VLAN、ARP、IPv4、fragment、UDP/TCP；IPv6/tunnel 未实现 |
| 软件策略 | 已实现最小闭环 | malformed drop，其余受支持/未知协议按静态端口对转发 |
| stats/telemetry | 已实现报文统计与规则诊断 | 工作线程、端口、队列统计及丢弃原因；规则汇总/分页、安装副本、失败指标、失败事件和耗时历史完整发布；规则 COUNT 独立查询 |
| topology | 已实现发现 | ethdev/representor、driver、switch domain/port；尚无角色解析 |
| capability profile / probe cache | 已实现本机诊断闭环 | 设备/固件/DPDK 身份、描述符和既有 queue/RSS/offload；完整规则校验结果缓存五秒、64 槽、强制刷新与失效；成功观察集合不承诺任意组合；正式安装始终新校验 |
| rule IR | 已实现第一版 | 有序 match/action、domain/fallback、持久化 install port 和语义校验 |
| rule repository | 已实现内存版和可选写路径 | 稳定 ID、单调 generation、幂等 CRUD、乐观并发、按 ID 稳定分页；control 可在 mutation 后保存完整 snapshot |
| snapshot persistence | 已实现 v2、v1 单端口迁移、进程内隔离与可选跨进程保护 | 保存 install port、CRC32、0600、fsync+rename、dirty fail-stop、全量重放、preserved generation、回滚失败 handle 重试；独立恢复记录 v3 保存逐次意图、结果和候选坐标；TAP 本地只读核对已实现，归属证明和自动清理未实现 |
| planner | 已实现第一版 | 明确 software/rte_flow 选择、fallback reason 与 transfer domain 校验 |
| rte_flow | 已实现事务 backend | validate 与 create 分离；对象仓库持有 handle；批量失败逆序 destroy |
| 等价软件 fallback | 已实现最小集合 | ingress Ethernet/IPv4/UDP/TCP、DROP/MARK/COUNT；`prefer` 仅在 validate 失败时回退，QUEUE/transfer 拒绝降级；immutable snapshot + DPDK QSBR 非阻塞回收已接入 |
| 虚拟 PMD flow 验证 | TAP 与 net_ring 已验证 | 双 TAP 已验证硬件 DROP/QUEUE；软件 TCP+MARK+COUNT+DROP 有真实报文与计数证据；net_ring 已验证 prefer 回退，见 [待办文档](todo_virtual_flow_backend.md) |
| 批量事务/回滚 | 已接双 backend；单测和 net_ring 已验证 | 新建、精确删除及 2–4 条精确版本更新；更新先建全部新对象、再删旧对象、整批发布 repository；失败补偿保留原 generation，补偿失败进入 recovery isolation |
| control service | 已实现单规则与批量闭环 | 创建、幂等重放、generation replacement、查询、删除；在线隔离停止软件 worker，清理重试成功后退出 |
| management API | 本机 v15 已验证 | rule-history、rule-latency、rule-metrics、能力画像、探测与清缓存；rule-status 核对实际后端与耗时；health/ready、画像、指标和历史在隔离仍可查询；旧版本客户端被拒绝 |
| 规则安装观测 | 已实现状态、失败分类、失败事件、耗时历史与规则 telemetry | 精确版本核对后端、端口与 COUNT 配置；三组耗时、整批不重复计数、分位数上界与未知/溢出标志；最近 64 条完整失败的版本分页、覆盖提示和编号耗尽冻结；不提供持久化审计日志 |
| health/readiness | 已实现本机最小闭环 | 实际线程、端口与链路、移除、恢复隔离和持久化 dirty；未就绪原因与 CLI 退出码；未提供远程探针、心跳或主动网络探测 |
| flow template/async | 未实现 | 规模化与高频更新能力待实现 |
| ACL/LPM/NAT/conntrack | 未实现 | 旧占位实现已删除，需按 stage/snapshot 模型重建 |
| DPU/SmartNIC 实机 | 未验证 | 需在具体 PMD、固件、representor devargs 上建立能力矩阵 |

2026-10-01 新增端口/队列统计、丢弃原因与 management v8，16/16 单测通过；实际收发核对入口 RX、出口 TX 及端口/队列汇总，并覆盖畸形、无路由、出口异常与 TX 队列满。详见验证记录中的统计验证。 双队列实际收发验证已补齐：两个正式 worker 分别处理 24/48 包的每轮负载，满容量批量更新、逐队列统计、报文序号、恢复和资源回收通过普通及 AddressSanitizer/UBSan 构建。net_ring 无 RSS，双队列仅由测试夹具显式配置，生产 daemon 的 RSS 要求保持不变。

## DPDK 21.11 测试机验证

- Ubuntu 22.04、GCC 11.4、Meson 0.61.2、DPDK 21.11.9；
- DPDK 21.11.9 下 `-Werror` 全量编译和链接通过，15/15 单元测试通过（含本次批量更新和 CLI 测试）；
- 双 `net_ring` 完成 `dppd`/`dppctl` 进程间 smoke test；PMD 返回 `ENOSYS` 时 `prefer` 规则以 software backend 发布，`require` 保持失败，退出后 socket 正常清理；
- `port-show` 已验证能返回 `net_ring` 的队列与 offload 快照；合法 TCP/CIDR 规则进入 PMD 校验，非法 CIDR 在客户端边界被拒绝；
- QUEUE/MARK/COUNT 组合规则已通过 CLI 到 PMD 的端到端解析；重复 action modifier 在客户端拒绝；COUNT 查询的成功、无 COUNT、过期 generation 和规则不存在路径由单测覆盖；
- repository 列表按 rule id 排序；分页 generation 一致性和跨页写入返回 `ESTALE` 已由单测覆盖；
- snapshot 正常回读、乱序写入后的稳定排序、payload 损坏、截断、空 repository，以及 v1 严格读取和 v2 迁移后的代际/端口保留已由单测覆盖；
- apply/delete 已生效但保存失败、dirty 写入阻断、preflight/显式 flush 修复和 generation 对齐已由故障注入单测覆盖；
- 双 `net_ring` 进程 smoke 已验证 management v6；默认未配置路径时 `enabled=no` 且 flush 返回 `EINVAL`；
- `--state-path` 首次启动创建 `0600` 空 v2 snapshot，第二次启动从同一路径恢复为 clean generation；v1 文件返回 `EPROTONOSUPPORT` 并拒绝启动；
- 两规则启动重放、install port 保留、逐规则/全局 generation 保留，以及第二条创建失败时逆序回滚第一条已由 fake flow 故障注入单测覆盖；
- 第二条创建失败且第一条删除失败时，recovery isolation、普通操作阻断、残留 handle retry 与 restart-required 状态已由 control/management 单测覆盖；
- 双 `net_null` 完成 worker、统计、telemetry 和退出生命周期 smoke test；
- VMware `vmxnet3` 数据口 `ens192`（PCI `0000:0b:00.0`）通过真实 PMD probe、能力读取、queue setup、start/stop；
- 测试后已恢复 `vmxnet3`、`192.168.100.1/24` 和初始大页配置；
- 2026-10-02 已将外部发包机 `.134` 的数据口接到 `VMnet3` 并设置
  `192.168.100.2/24`；指定接收端数据 MAC 的定向 ARP、ping 3/3 和 Linux UDP
  实际收包 256 包/64 流通过，序号及负载完整。发送端配置已保存但未重启验证。
  后续独立真实 PMD 测试通过 `net_vmxnet3` 接收各轮 4096 个 UDP/TCP 包，
  64 条流的地址、端口、序号及负载匹配，无重复或遗漏。UIO 和 VFIO 配置下
  均为队列 0 收到全部报文、队列 1 为零、无 RSS hash；双队列 RSS 验收未通过，
  Linux 原生驱动对照同样只有队列 0 收包：8 个队列及 RSS 显示启用，正常 TCP
  的 64 个连接/4096 条记录全部核对通过，但 RX 单播增量为 320/0/0/0/0/0/0/0。
  问题范围已缩小到当前 Workstation/VMnet3 数据路径，具体实现原因仍待确定；
  不直接判定为平台 RSS 初始化缺陷。
  后续单队列验收使用正式 daemon/worker，真实 vmxnet3 入口 → TAP 出口收到
  512 个测试 UDP 包，384 转发、128 软件规则丢弃；初始 256 包及删除规则后的
  64 包逐字节匹配，DROP+COUNT 两次均为 64 hits/3968 bytes，同步捕获无漏包。
  修正单队列 RSS 状态误报后，额外 64 包转发及逐字节核对通过；16/16 单测通过。
  非空软件快照随后完成一次真实 daemon 重启和外部收包：规则内容、generation 1、
  安装端口保持一致，COUNT 从零重新计数，快照 SHA-256 不变，过期版本返回 ESTALE。
  重启前后各 64 包均被 DROP，COUNT 各为 64 hits/3968 bytes，同步捕获无漏包；
  删除恢复规则后 64 包逐字节转发验证通过，最终快照 clean、generation 2、账本为空。
  不证明重启期间持续流量无损，不涉及硬件 flow 恢复。
  满容量 2/4 条纯软件规则分别在外部连续收包时完成 50 次 CLI 批量更新，
  每组 RX 与规则丢弃均为 10000，TAP 出口零泄漏；容量和过期版本拒绝不改变
  账本或快照。修复了单规则满表新增先安装 backend、再返回 EUCLEAN 的缺陷，
  现在预检返回 ENOSPC；软硬件 backend 的容量回归及 16/16 单测通过。
  此收包测试中的旧新规则均为 DROP，逐包完整旧表/新表仍由并发快照测试验证。
  随后补齐真实入口 TCP 过滤：无规则 TCP、规则下 UDP、快照恢复后的 TCP DROP、
  删除后的 TCP 恢复四个通过阶段合计 256 包，192 包逐字节转发、64 包丢弃；
  TCP COUNT 为 64 hits/4736 bytes，UDP 不改变 TCP 计数，最终空表 clean。
  使用原始 TCP ACK 帧，不作为 TCP 会话验收；初轮参数错误和窗口错过发包已排除。
  2026-10-03 补齐真实网卡 ↔ TAP 同时双向转发：UDP/TCP 各方向 30000 包，
  共 120000 包逐字节一致、无重复或遗漏；收发统计交叉匹配，背景流量单独核对。
  退出时缓冲池 8191/8191 全部归还、in-use 0；严格构建和 16/16 单测通过。
  同日增加链路轮询及出口保护，两次真实 TAP 断开/恢复期间外部 PMD 收到 448 包：
  192 包逐字节转发、128 包出口断开丢弃、128 包规则丢弃；已有 COUNT 64/4736
  在恢复期间保持，随后增加至 128/9472，rule generation 和快照字节不变。
  ENODEV/EIO 查询失败的独立进程注入验证退出码 1、快照不变和正常重启恢复；
  这是查询错误路径验证，不证明物理设备热拔插。17/17 单测、四组 net_ring 收发通过。
  随后接入设备移除回调及状态查询；8 组 net_ring 进程测试覆盖两端口事件、初始
  link ENOTSUP 下的事件/查询、启动期间事件与第二端口注册失败。实际 DPDK 回调
  分发和在途回调注销 EAGAIN 重试通过，失败码 1、1024 个 mbuf 归还、旧快照不变，
  非空两端口规则恢复且 COUNT 重置。四组正式 worker 测试确认移除标记直接结束
  收发、QSBR 注销和进程内再次启动 ENODEV。补齐 stop/close 失败回传与 close 失败
  保留缓冲池的回归，18/18 单测及既有故障恢复回归通过。
  在真实 vmxnet3 → worker → TAP 上注入移除通知，故障退出码 1、8191 个 mbuf
  归还、快照不变；未加载测试库重启后规则/版本恢复、COUNT 重置、TCP 继续阻断、
  UDP 转发及删除后的 TCP 转发通过。五个通过阶段 320 包，192 逐字节转发、128
  规则丢弃；错过捕获窗口的额外 64 UDP 包和 TAP 背景流量单独排除。两次退出
  资源回收、数据口恢复及 ARP/ping 通过；通知注入不证明物理热拔插。
  这不作为双物理端口、RSS、硬件卸载或吞吐证明。
  测试后数据口驱动、地址、路由、大页数量和 VFIO No-IOMMU 参数已恢复；
  定向 ARP 与 ping 3/3 通过。详见集成测试说明的真实 PMD 收包结果。
- 双 TAP 上的 software-only TCP+MARK+COUNT+DROP 规则已验证真实报文阻断和 `hits=1/bytes=76`；该证明来自平台软件路径，不把 TAP 误当成 MARK/COUNT 硬件验收。
- 软件 classifier 已从 rwlock 规则表切换为不可变 snapshot：worker 注册 QSBR reader、每轮完整 ingress 扫描报告静默点；控制面仅在所有 reader 越过发布 token 后回收旧 snapshot。
- 测试机以双 `net_ring` 常驻运行 worker，连续完成 40 次 software-only 规则发布/删除；管理 socket 仍可响应、daemon 正常退出且 socket 清理成功，覆盖了 QSBR reader 注册、静默点与旧 snapshot 回收路径。
- 双 `net_ring` 上的 `apply-drop-batch 0 700 701 702` 与 `delete-batch 700 1 701 2 702 3` 已验证：创建共享同一 transaction，删除按输入顺序推进 generation 4/5/6，最终 repository 为空且 daemon/socket 正常清理。
- 双 `net_tap` 已完成真实 flow 验证：无规则报文转发、ETH/DROP 阻断、IPv4/TCP→QUEUE
  命中、规则删除后的转发恢复，以及 daemon/TAP/socket 清理均已验证；TAP 不作为
  MARK/COUNT 或队列性能验收依据。

## 已知工程限制

- 启动后端口集合和 port-pair snapshot 不动态更新；
- 已有链路轮询、down 出口保护及 up 自动恢复；移除通知与状态触发失败退出，
  依赖 PMD 的报告能力；总线访问保护、进程内重新枚举与热重配未实现，物理热拔插未验收；
- TX 发送不足时立即丢弃，没有软件重试队列；这是可预测的 baseline 策略；
- 统计支持 worker、port、queue 维度；独立原子采样不承诺逐字段一致时刻，硬件卸载报文不计入软件 worker；
- RSS key 和 RETA 使用 PMD 默认值；
- 没有查询 `rte_flow` 资源容量或预留规则空间；
- COUNT id 由 rule id 的低 32 位生成，控制面必须保证其作用域内不冲突。
- 硬件规则更新采用“先创建新 generation，再删除旧 generation”；若 PMD 拒绝重叠规则，更新失败并保留旧规则，当前不承诺跨 PMD 的无损原子替换。
- management v15 直接传递本机构建的 C 结构体，只承诺同主机、同版本 `dppd/dppctl` 配对；跨版本或远程接入需要另行定义稳定序列化协议。
- 批量更新保留 `rule_capacity + 1` 的 backend 容量。纯软件路径整批替换快照、复用旧槽位，满表可更新；每次分类使用完整旧表或新表，新版本 COUNT 归零，未更新规则保留计数。硬件及混合路径仍需要临时容量，只保证账本整批发布；PREFER 可能整表替换但最终成为混合计划时，会在安装前复查软件空间。
- daemon 为安全起见不会自动删除启动前已存在的 socket 路径；异常退出后需由部署脚本确认没有存活进程再清理残留文件。
- snapshot v2 已连接 daemon `--state-path`、control mutation 和启动硬件重放；默认未指定路径时仍禁用。回滚失败时 daemon 进入允许 `reconcile-status/reconcile-retry` 和只读 `health/ready`、`capability-show`、`rule-metrics/rule-latency/rule-history` 与规则 telemetry 的进程内隔离模式。可选 `--recovery-path` 已提供异常退出后的重放阻断与外部清理确认；PMD 专用自动残留清理、degraded recovery 与多端口 v1 迁移尚未实现。格式及语义见 [snapshot v2 文档](persistence_snapshot_v2.md) 与 [恢复保护](recovery_guard.md)。
