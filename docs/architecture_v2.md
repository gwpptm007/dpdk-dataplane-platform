# 架构设计 V2

> 当前版本主线只保留现代 DPDK 用户态数据面平台方向；历史上的 DPDK 17 `igb_uio + KNI + 配套 ko` 集成模式不进入当前运行时主线，仅保留在 `docs/legacy_dpdk17_mode.md` 与 `docs/legacy_vs_modern.md` 中作为经验沉淀。


## 1. 目标定位

本项目不是单一 fast path demo，而是一个 **面向 SmartNIC / DPU 演进的 DPDK 用户态数据面平台骨架**。

## 2. 当前分层

### 平台层
- EAL 初始化
- port / queue 初始化
- worker 启动

### 报文层
- ARP / IPv4 / UDP / TCP 解析骨架
- 包头重写
- TX checksum offload flag 准备
- ARP table 占位

### 策略层
- route lookup
- ACL
- NAT session + aging
- pipeline path 选择（SW / HW_RTE / TRANSFER）

### offload 层
- core：pattern / action / flow object / backend 抽象
- soft：软件后端
- rte_flow：硬件映射桥
- hw：厂商特定后端占位

### DPU / switch 层
- representor 抽象
- transfer pipeline 占位

## 3. 设计意图

这次不是简单把目录改名，而是把后续 Phase 4 的关键约束提前显式化：

- rule 必须 pattern/action 分离
- backend 必须区分 soft / rte_flow / vendor-specific
- pipeline 必须允许 SW / HW_RTE / TRANSFER 三路径
- representor 必须作为独立模块出现，而不是隐含在 offload 里
