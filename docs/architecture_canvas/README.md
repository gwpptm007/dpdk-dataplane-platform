# DPDK Dataplane Platform · Architecture Canvas

这是平台 V4 的可视化架构画布，不是实现代码。

## 查看方式

- `index.html`：完整可缩放画布，顶部编号可以在 17 张图之间跳转。
- `png/`：逐张 1600×900 PNG，可直接评审或贴入文档。
- `svg/`：逐张自包含 SVG，适合浏览器独立打开和按画布尺寸缩放展示。

## 画布目录

| 编号 | 画布 |
| --- | --- |
| 00 | 平台总览 |
| 01 | 三条关键闭环 |
| 02 | Management Plane |
| 03 | Intent / Model / State |
| 04 | Topology / Capability |
| 05 | Planner / Compiler Registry |
| 06 | Transaction / Generation Git 式演进图 |
| 07 | Reconciler |
| 08 | Backend Framework |
| 09 | Software Dataplane |
| 10 | Runtime / Device Manager |
| 11 | Observability / Operations |
| 12 | Host / SmartNIC / DPU 部署 |
| 13 | Canonical Resource Model（二级细分） |
| 14 | Placement Decision（二级细分） |
| 15 | Transaction State Machine（二级细分） |
| 16 | Reconciliation State Machine（二级细分） |

00–12 用于确认平台边界、模块职责、状态闭环和执行关系；13–16 把领域对象、规划决策、事务与收敛状态机细化到可评审层。下一轮再进入 API 契约、持久化 schema、线程同步和后端接口。
