# V3 实现状态

状态只表示代码是否真实存在，不以目录或占位接口计入完成度。

| 领域 | 当前状态 | 边界 |
|---|---|---|
| 构建 | 已验证 | Ubuntu 22.04、Meson 0.61.2、DPDK 21.11.9 下完成 `-Werror` 全量编译和链接 |
| 配置 | 已实现最小 CLI | 端口对、队列、burst、mbuf、统计、duration、promisc、pdump、管理 socket、规则容量 |
| ethdev | 已实现 baseline | capability 交集、RSS、per-socket pool、queue setup、start/stop |
| 软件 worker | 已实现 | 每 queue 一个 lcore，所有端口 burst RX/TX，mbuf ownership 完整 |
| parser | 已实现 baseline | 双 VLAN、ARP、IPv4、fragment、UDP/TCP；IPv6/tunnel 未实现 |
| 软件策略 | 已实现最小闭环 | malformed drop，其余受支持/未知协议按静态端口对转发 |
| stats/telemetry | 已实现 baseline | 聚合计数与 `/dppd/stats`；尚无 port/queue/rule 维度 |
| topology | 已实现发现 | ethdev/representor、driver、switch domain/port；尚无角色解析 |
| rule IR | 已实现第一版 | 有序 match/action、domain/fallback、持久化 install port 和语义校验 |
| rule repository | 已实现内存版和可选写路径 | 稳定 ID、单调 generation、幂等 CRUD、乐观并发、按 ID 稳定分页；control 可在 mutation 后保存完整 snapshot |
| snapshot persistence | 已实现 v2、v1 单端口迁移与进程内恢复隔离 | 保存 install port、CRC32、0600、fsync+rename、dirty fail-stop、全量重放、preserved generation、回滚失败的 handle 重试；v1 迁移需显式统一端口；跨进程 residual flow reconciliation 未实现 |
| planner | 已实现第一版 | 明确 software/rte_flow 选择、fallback reason 与 transfer domain 校验 |
| rte_flow | 已实现事务 backend | validate 与 create 分离；对象仓库持有 handle；批量失败逆序 destroy |
| 等价软件 fallback | 已实现最小集合 | ingress Ethernet/IPv4/UDP/TCP、DROP/MARK/COUNT；`prefer` 仅在 validate 失败时回退，QUEUE/transfer 拒绝降级；immutable snapshot + DPDK QSBR 非阻塞回收已接入 |
| 虚拟 PMD flow 验证 | TAP 与 net_ring 已验证 | 双 TAP 已验证硬件 DROP/QUEUE；软件 TCP+MARK+COUNT+DROP 有真实报文与计数证据；net_ring 已验证 prefer 回退，见 [待办文档](todo_virtual_flow_backend.md) |
| 批量事务/回滚 | 已接双 backend | 批量创建采用全量 validate/prepare、顺序 commit、逆序 destroy；批量精确删除先删 actual、失败时以原 generation 重建补偿，补偿失败进入 recovery isolation；软件 backend 同样受 transaction 驱动 |
| control service | 已实现单规则闭环 | 创建、幂等重放、generation replacement、查询、删除和错误状态保护 |
| management API | 已实现本机 v6 | `dppctl` 支持 port-show、ping、稳定分页、完整 get、DROP/QUEUE/MARK/COUNT、count query、delete、persistence-status/flush、reconcile-status/retry、2–4 条同端口 Ethernet DROP 的原子 `apply-drop-batch`，以及 2–4 条精确 generation 的原子 `delete-batch` |
| flow template/async | 未实现 | 规模化与高频更新能力待实现 |
| ACL/LPM/NAT/conntrack | 未实现 | 旧占位实现已删除，需按 stage/snapshot 模型重建 |
| DPU/SmartNIC 实机 | 未验证 | 需在具体 PMD、固件、representor devargs 上建立能力矩阵 |

## DPDK 21.11 测试机验证

- Ubuntu 22.04、GCC 11.4、Meson 0.61.2、DPDK 21.11.9；
- DPDK 21.11.9 下 `-Werror` 全量编译和链接通过，12/12 单元测试通过；
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
- 当前缺少连接到 `192.168.100.0/24` 的外部发包端，尚未形成真实 RX/TX 内容证据。
- 双 TAP 上的 software-only TCP+MARK+COUNT+DROP 规则已验证真实报文阻断和 `hits=1/bytes=76`；该证明来自平台软件路径，不把 TAP 误当成 MARK/COUNT 硬件验收。
- 软件 classifier 已从 rwlock 规则表切换为不可变 snapshot：worker 注册 QSBR reader、每轮完整 ingress 扫描报告静默点；控制面仅在所有 reader 越过发布 token 后回收旧 snapshot。
- 测试机以双 `net_ring` 常驻运行 worker，连续完成 40 次 software-only 规则发布/删除；管理 socket 仍可响应、daemon 正常退出且 socket 清理成功，覆盖了 QSBR reader 注册、静默点与旧 snapshot 回收路径。
- 双 `net_ring` 上的 `apply-drop-batch 0 700 701 702` 与 `delete-batch 700 1 701 2 702 3` 已验证：创建共享同一 transaction，删除按输入顺序推进 generation 4/5/6，最终 repository 为空且 daemon/socket 正常清理。
- 双 `net_tap` 已完成真实 flow 验证：无规则报文转发、ETH/DROP 阻断、IPv4/TCP→QUEUE
  命中、规则删除后的转发恢复，以及 daemon/TAP/socket 清理均已验证；TAP 不作为
  MARK/COUNT 或队列性能验收依据。

## 已知工程限制

- 启动后端口集合和 port-pair snapshot 不动态更新；
- 没有 link-status change、device removal 和热重配处理；
- TX 发送不足时立即丢弃，没有软件重试队列；这是可预测的 baseline 策略；
- 统计为 worker 聚合维度，无法直接定位某个 port/queue；
- RSS key 和 RETA 使用 PMD 默认值；
- 没有查询 `rte_flow` 资源容量或预留规则空间；
- COUNT id 由 rule id 的低 32 位生成，控制面必须保证其作用域内不冲突。
- 硬件规则更新采用“先创建新 generation，再删除旧 generation”；若 PMD 拒绝重叠规则，更新失败并保留旧规则，当前不承诺跨 PMD 的无损原子替换。
- management v6 直接传递本机构建的 C 结构体，只承诺同主机、同版本 `dppd/dppctl` 配对；跨版本或远程接入需要另行定义稳定序列化协议。
- daemon 为安全起见不会自动删除启动前已存在的 socket 路径；异常退出后需由部署脚本确认没有存活进程再清理残留文件。
- snapshot v2 已连接 daemon `--state-path`、control mutation 和启动硬件重放；默认未指定路径时仍禁用。回滚失败时 daemon 进入仅允许 `reconcile-status/reconcile-retry` 的进程内隔离模式；进程终止后的 PMD 专用 residual flow 清理、degraded recovery 与多端口 v1 迁移尚未实现。格式及语义见 [snapshot v2 文档](persistence_snapshot_v2.md)。
