# Phase 1 Scope V2

## 前置说明

Phase 1 仅面向现代 DPDK 用户态软件路径实现，不纳入 DPDK 17 的 `igb_uio + KNI + 配套 ko` 运行时支持。历史模式只保留说明文档，不进入当前代码主线。


## Phase 1 要做深的内容

### 必须手写
- ARP table 维护（静态/动态）
- 路由查表（LPM 或 hash）
- NAT session + aging
- TX checksum offload flag 设置
- mbuf 构造与 clone 的学习型样例

### 必须真正理解
- `rte_mbuf` / `rte_pktmbuf`
- `rte_eth_rx_burst` / `rte_eth_tx_burst`
- `rte_mempool`
- RSS hash field 与 queue 映射

## Phase 1 明确不做
- 真实 SmartNIC / DPU 硬件适配
- 完整 `rte_flow` 落地
- 完整 vSwitch / overlay 系统
- 完整 TCP 状态机 / IPsec / VPN

这些内容会在 Phase 2~4 逐步增加，但不会挤占第一阶段主线。
