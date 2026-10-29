# DPDK 机制与项目调用地图

核查基线：`380d7da738eb363ae1424228e332249326ae1b54`，2026-10-29。
下表中的文件链接指向工作树，函数名用于定位；代码后续改变时应重新核查调用与条件。

| 机制 | 真实入口 | 读代码时追问什么 |
| --- | --- | --- |
| EAL 初始化 | [app/dppd/main.c](../../app/dppd/main.c) 的 `main`、`rte_eal_init` | EAL 消费了哪些参数，业务参数从哪里开始 |
| 内存池 | [device.c](../../lib/device/device.c) 的 `create_socket_pools` | 选择哪个 socket，队列从哪个 pool 取 mbuf |
| ethdev 初始化 | [device.c](../../lib/device/device.c) 的 `prepare_port_configuration`、`configure_port_queues` | 查询能力、调整描述符和实际启用有什么差别 |
| worker 布局 | [runtime.c](../../lib/runtime/runtime.c) 的 `dppd_runtime_init`、`dppd_runtime_start` | worker 固定 queue_id，并轮询多个端口的同编号队列 |
| NUMA 选择 | [numa.c](../../lib/runtime/numa.c) 的 `dppd_numa_select` | 候选仅来自启用的非主 lcore，未知 socket 如何处理 |
| 收发与归属 | [worker.c](../../lib/dataplane/worker.c) 的 `process_ingress`、`flush_egress` | 哪些包交给 PMD，哪些仍需应用释放 |
| 多段安全读取 | [packet_mbuf.c](../../lib/dataplane/packet_mbuf.c) 的 `dppd_packet_parse_mbuf` | `rte_pktmbuf_read` 返回原地址还是临时副本 |
| 可写报文 | [ipv4_rewrite.c](../../lib/dataplane/ipv4_rewrite.c) 的 `needs_copy`、`dppd_ipv4_rewrite_flow` | 哪些引用/外部缓冲情形要求复制 |
| IPv6 改写 | [ipv6_rewrite.c](../../lib/dataplane/ipv6_rewrite.c) | 扩展头、分段读取和写入怎样保持长度边界 |
| QSBR | [software_backend.c](../../lib/dataplane/software_backend.c)、[ipv4_router.c](../../lib/dataplane/ipv4_router.c) | 发布新指针与等待旧读者是不同步骤 |
| 原生规则 | [rte_flow.c](../../lib/offload/rte_flow.c) | 模型怎样变成 pattern/action，失败返回什么 |
| 拓扑与 transfer | [topology.c](../../lib/device/topology.c)、[transfer.c](../../lib/control/transfer.c) | port_id 是否代表目标 VF，证据来自哪里 |
| indirect COUNT | [shared_count.c](../../lib/offload/shared_count.c) | 句柄归属、引用关系和查询范围 |
| 原生模板/异步 | [flow_template.c](../../lib/offload/flow_template.c)、[async_flow.c](../../lib/offload/async_flow.c) | PMD 完成前哪些参数必须继续存活 |
| 项目请求队列 | [control/async_flow.c](../../lib/control/async_flow.c) | 平台请求状态与 PMD 操作状态怎样对应 |
| telemetry | [telemetry.c](../../lib/observability/telemetry.c) | 是快照、副本还是直接硬件查询 |
| 退出 | [runtime.c](../../lib/runtime/runtime.c)、[device.c](../../lib/device/device.c) | worker 仍持有资源时能否销毁设备和内存池 |

## 四种不同的实现来源

1. **DPDK 原生**：EAL、ethdev、mbuf/mempool、QSBR、rte_flow、telemetry 等 API
2. **项目封装**：规则仓库、不可变快照、事务、持久化、请求状态机，以及软件 ACL/路由/连接跟踪
3. **测试夹具**：net_ring、驱动 API 包装、故障注入；用于确定特定路径行为
4. **扩展知识**：本项目没有直接采用的 DPDK 库，用来比较方案，不能写成已落地功能

核查 `app/`、`lib/`、`include/`，没有发现对 `rte_acl`、`rte_lpm`、`rte_hash`、`rte_graph` 或 `rte_ring` 的直接调用。
这不表示 DPDK 底层内部不使用 ring；它说明应用自身没有用这些 API 实现相应模块。

[exact.c](../../lib/dataplane/exact.c) 自建哈希桶；[acl.c](../../lib/dataplane/acl.c) 实现规则匹配；
[conntrack.c](../../lib/dataplane/conntrack.c) 自建会话结构并使用互斥锁。
不能因为功能名称相同，就把它们归入 DPDK 对应库的使用案例。

`rte_ring` 的直接使用可在 [software_traffic.c](../../tests/integration/software_traffic.c)
和 [ipv4_traffic.c](../../tests/integration/ipv4_traffic.c) 中找到：通过 `rte_eth_from_rings` 构造 ethdev，再驱动正式 worker。
这里没有物理 DMA 描述符，且部分设备配置由测试包装，不代表物理网卡能力。
