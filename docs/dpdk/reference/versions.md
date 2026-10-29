# 版本、驱动与扩展知识边界

源码基线：`380d7da738eb363ae1424228e332249326ae1b54`，核查日期：2026-10-29。
项目 [Meson 配置](../../../meson.build) 要求 DPDK >=21.11，历史软件验收覆盖 21.11.9 和 24.11.8。
最低可构建版本、已验证版本、某个 API 的实际可用版本是三个不同概念。

| 内容 | 本项目的边界 |
| --- | --- |
| 原生 flow template / async | [flow_template.c](../../../lib/offload/flow_template.c) 与 [async_flow.c](../../../lib/offload/async_flow.c) 用 >=24.11 条件编译，旧分支 ENOTSUP |
| 共享对象与软件异步 | 平台自己的状态机与软件后端不能当成旧 DPDK 已具有原生硬件能力 |
| transfer 的 ingress 标志 | [rte_flow.c](../../../lib/offload/rte_flow.c) 对 <22.11 保留历史组合，按实际编译分支阅读 |
| rte_flow 支持 | 函数存在不意味着 PMD 支持所需 pattern/action 组合或具有足够资源 |
| VFIO 与 mlx5 | bifurcated 驱动与内核协作，不能按通用 VFIO 步骤把其端口全部解绑 |
| TAP 与 net_ring | 都是软件实验手段，但 TAP 规则与 net_ring 收发的语义和能力不同 |
| 实体硬件 | 当前机器没有已确认的目标 DPU 卸载证据，教程不得宣称完成硬件验收 |

官方版本入口：[21.11 编程指南](https://doc.dpdk.org/guides-21.11/prog_guide/index.html)、
[24.11 编程指南](https://doc.dpdk.org/guides-24.11/prog_guide/index.html)、
[Linux 驱动说明](https://doc.dpdk.org/guides-24.11/linux_gsg/linux_drivers.html)。
阅读某个 API 的历史时查该版本源码与发布说明，不把本项目选择的兼容门槛当作 API 首次引入时间。

## 未使用的库怎样继续学

以下是扩展索引，不算本次项目使用案例，也不引入替换实现的任务：

- `rte_hash`：对照项目自建 exact/conntrack 哈希，比较并发模式、容量、删除与回收
- `rte_acl`：对照项目的软件范围匹配，比较规则编译、查找批量与更新代价
- `rte_lpm`：对照项目路由查找，比较表结构、地址族与更新需求
- `rte_graph`：对照当前 run-to-completion worker，理解图节点调度与批处理
- `rte_ring`：本项目测试已直接使用，可独立研究 SP/SC、MP/MC 等同步约束；不要据此改造正式 pipeline

扩展时先写最小示例和选择理由，再评估是否适合项目，不能只因某库属于 DPDK 就认为一定更快。
