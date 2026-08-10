# NVIDIA mlx5 / BlueField 验收重点

- 明确应用运行在 host 还是 Arm/DPU 侧，以及 steering ownership；
- 用实际 devargs 枚举 uplink、PF、VF/SF representor，并记录 switch domain/port；
- transfer rule 使用 represented-port item/action，验证两个端点属于同一 embedded switch；
- 分开验证同步 `rte_flow`、template/async、counter、age 与 shared/indirect action；
- 用 CPU RX counter 证明命中 transfer rule 的包没有绕回软件 worker；
- 覆盖 representor 出现顺序变化、VF/SF 创建删除、DPU reboot、firmware reset 与规则重放；
- DOCA/DV Flow 若作为独立 backend，引入的是另一个编译器和 capability profile，不能与 `rte_flow` 对象混用。

具体能力以目标 mlx5 PMD、rdma-core、固件、DPU mode 和 DPDK 版本的组合实测为准。
