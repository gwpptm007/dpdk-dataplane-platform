# dpdk-dataplane-platform

面向现代 DPDK 用户态数据面、SmartNIC/DPU、`rte_flow` 与 representor 的演进型平台基线。

V3 不再把“软件、硬件、transfer”误建模成逐包执行模式：软件路径处理送达 CPU RX queue 的包；`rte_flow` 和 embedded-switch transfer rule 属于控制面下发的硬件规则，硬件成功处理的包通常不会进入软件 worker；representor 是 PMD 暴露的 ethdev 拓扑端点，而不是一个软件转发后端。

当前可运行基线提供：

- DPDK 21.11+ 的 Meson 构建，不提供会掩盖问题的 mock 模式；
- 每队列一个 lcore 的 run-to-completion 软件路径；
- RSS、多 NUMA socket mbuf pool、burst RX/TX 与多段 mbuf 能力处理；
- Ethernet、双 VLAN、ARP、IPv4、UDP/TCP 的边界安全解析；
- immutable forwarding snapshot 和 per-worker 原子统计；
- ethdev/representor/switch-domain 拓扑发现；
- 统一 rule IR 到 `rte_flow` 的真实 validate/create/query/destroy/flush；
- 带稳定 ID、generation、幂等更新和乐观并发控制的 rule repository；
- versioned rule snapshot v2、install port、CRC32、原子替换、dirty 写路径、fail-closed 启动重放、v1 单端口离线迁移和进程内 recovery isolation；
- 可解释的软硬件 planner，以及 validate/prepare/commit/rollback transaction engine；
- 连接 desired repository 与真实 `rte_flow` 对象仓库、支持 generation replacement 的进程内 control service；
- 版本化 Unix `SOCK_SEQPACKET` 管理接口和 `dppctl`，支持端口能力查询、generation 稳定分页、完整规则详情、单规则 apply/delete 及硬件 COUNT 查询；
- DPDK telemetry `/dppd/stats` 与可选 pdump 服务。

当前软件路径在 port-pair 基线上已支持 Ethernet/IPv4/UDP/TCP 的 DROP、MARK、COUNT：`prefer` 仅在硬件 validate 失败且语义等价时回退，`software` 可显式强制软件 backend；QUEUE 与 transfer 不会被错误降级。软件 classifier 以不可变 snapshot 原子发布，worker 每轮完整收包扫描后报告 DPDK QSBR 静默点，旧规则集在宽限期后异步回收，不阻塞逐包分类。管理 CLI 可构造 DROP/QUEUE/MARK/COUNT 规则，并提供 `apply-drop-batch`（2–4 条同端口 Ethernet DROP 新规则的原子创建）、`delete-batch`（2–4 条精确 generation 规则的原子删除）和 `update-drop-batch`（2–4 条精确旧 generation 规则完整替换为 Ethernet DROP，管理协议 v7）；旧规则和新计划全在软件后端时，批量更新通过一次快照发布完成，每个报文使用完整旧表或新表，满表也可更新；硬件及混合路径仍只保证规则账本整批发布。具体硬件规则仍以 PMD 的 `rte_flow_validate()` 为准。`--state-path` 可启用 snapshot v2 自动保存与 fail-closed 启动重放；确认所有旧规则来自同一 ethdev 时，可用 `dppd-snapshot-migrate --install-port` 离线转换 v1 文件。回滚删除失败时 daemon 只开放 `reconcile-status/reconcile-retry`，清除已知 handle 后退出重启。默认不指定路径时保持禁用。跨进程 reconciliation、degraded recovery 和 flow template 是下一阶段工作，详见 [实现状态](docs/implementation_status.md) 与 [路线图](docs/roadmap_v3.md)。

## 快速开始

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
