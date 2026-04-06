# dpdk-dataplane-platform

一个面向 **现代 DPDK 用户态数据面 / SmartNIC / DPU / rte_flow / representor** 演进方向的项目。

当前版本已经从“纯骨架/可编译调用链”进一步收口到 **Phase 1 真实数据面闭环基线**：

- **主线只保留现代 DPDK 方式**：用户态 DPDK 应用 + 现代运行环境准备。
- **不把 DPDK 17 的 `igb_uio + KNI + 配套 ko` 当成当前工程目标**。
- **保留 DPDK 17 旧模式说明文档**，作为历史经验沉淀与面试材料。
- **Phase 1 已接入真实 DPDK 主路径代码**：EAL、mempool、ethdev、rx/tx burst、真实二三四层解析、最小 ACL/route/NAT/rewrite 调用链。

---

## 当前版本定位

这不是最终产品版，但已经不再只是目录样机。

当前仓库应理解为：

> **一个以现代 DPDK 为目标、具备 Phase 1 真实主路径代码的工程基线。**

它现在处于这样一个阶段：

- 工程结构已经稳定下来；
- mock 模式可实际编译与运行；
- 真实 DPDK 路径代码已经落地到 `main / port_init / worker / parser / rewrite` 主线上；
- 但容器里没有 `libdpdk`，所以本包中只验证了 mock 构建，没有在当前容器里完成真实 DPDK 链接验证。

---

## 当前已完成什么

### 1. 工程层

- `include/dppd/` 公共头目录已建立
- `app/` / `lib/` / `examples/` / `tests/` / `scenarios/` / `docs/` 目录边界已收敛
- `lib/offload/` 已拆为 `core / soft / rte_flow / hw`
- `lib/representor/`、`lib/switch/` 已作为后续扩展位保留
- `docs/plans/` 中保留项目计划书原件

### 2. 构建层

当前支持两种构建模式：

#### mock 模式
- 用于当前容器内实际验证
- 可完整编译
- 可运行主调用链
- 用构造的 ARP / IPv4 / UDP 报文验证 parser / pipeline / NAT rewrite

#### dpdk 模式
- 通过 `pkg-config libdpdk` 链接真实 DPDK
- 当前代码里已经接入真实 DPDK 主路径
- 需要目标机器上具备 `libdpdk`

### 3. Phase 1 主路径层

这一版已经落地的真实主路径包括：

- `main.c`
  - DPDK 模式下的 EAL 初始化入口
  - `--` 分隔 EAL 参数与应用参数
- `port_init.c`
  - mbuf pool 创建
  - ethdev 配置
  - rx/tx queue setup
  - 端口启动与链路信息输出
- `worker.c`
  - DPDK 模式真实 `rte_eth_rx_burst / rte_eth_tx_burst`
  - mock 模式构造真实协议报文而不是 fake len 占位
- `parser.c`
  - 真正解析 Ethernet / ARP / IPv4 / UDP / TCP 最小头部
- `pipeline_fwd.c`
  - ARP 允许发送
  - IPv4/UDP 进入 ACL + route 决策
- `nat_session.c`
  - 最小 session table + 端口转换基线
- `pkt_rewrite.c`
  - UDP 源端口改写 + IPv4/UDP checksum 重算

---

## 当前还没有完成什么

为了避免误判，这一版明确 **还没有完成** 下面这些内容：

- 多 worker / `rte_eal_remote_launch` 真正拉起
- per-lcore stats
- 配置文件真正加载到 ACL / route / NAT
- 真实 RSS 配置优化和队列映射调优
- 真实 rte_flow pattern/action 映射
- representor / transfer 的真实行为实现
- DPU / SmartNIC 硬件场景验证
- 当前容器内的真实 `libdpdk` 链接验证

所以这版虽然已经进入“真实 DPDK 主路径代码”阶段，但仍应被视为：

> **Phase 1 真实闭环基线，而不是完整数据面平台成品。**

---

## 目录速览

```text
.
├── app/                  # 主程序、控制、worker、端口初始化、统计
├── include/dppd/         # 公共头文件
├── lib/
│   ├── common/           # 日志、checksum、RSS config 等
│   ├── pkt/              # parser / rewrite / ARP table / tx offload
│   ├── flow/             # pipeline / route / ACL / NAT
│   ├── offload/          # core/soft/rte_flow/hw
│   ├── representor/      # representor 抽象
│   └── switch/           # transfer pipeline 抽象
├── examples/             # 最小示例规划
├── scenarios/            # Intel/NVIDIA/通用场景文档
├── tests/                # Scapy / pdump / regression
├── scripts/              # 构建、运行、抓包、绑定脚本
└── docs/                 # 架构、路线图、legacy 经验、计划书
```

---

## 构建方式

### 1. 当前容器可验证方式

```bash
./scripts/build.sh
./scripts/run_platform.sh --port 0 --rxq 1 --txq 1 --burst 32 --loops 1
```

### 2. 目标机器上的真实 DPDK 构建方式

```bash
DPPD_BUILD_MODE=dpdk ./scripts/build.sh
./build/dppd -l 0-1 -n 4 -- --port 0 --rxq 1 --txq 1 --burst 32 --loops 1000
```

说明：
- `-l 0-1 -n 4` 这类参数属于 EAL
- `--` 后面的参数属于 `dppd` 应用本身

---

## 当前完成度判断

### 从工程结构角度
- **约 85%**
- 目录边界、扩展位、文档主线已经比较稳定

### 从 Phase 1 可编译工程角度
- **约 75%**
- mock 主路径可跑
- DPDK 主路径代码已经接入

### 从“真实 DPDK Phase 1 可在目标机运行”的角度
- **约 45% ~ 55%**
- 代码主路径已经落地
- 但还缺目标机上的真实编译、收包验证、发送验证和配置加载

---

## 建议先看哪些文件

### 主代码入口
- `app/main.c`
- `app/port_init.c`
- `app/worker.c`
- `lib/pkt/parser.c`
- `lib/pkt/pkt_rewrite.c`
- `lib/flow/pipeline_fwd.c`
- `lib/flow/nat_session.c`

### 设计与路线文档
- `docs/architecture_v2.md`
- `docs/phase1_scope_v2.md`
- `docs/phase4_offload_design.md`
- `docs/repo_map_v2.md`
- `docs/build_verified.md`

### legacy 经验文档
- `docs/legacy_dpdk17_mode.md`
- `docs/legacy_vs_modern.md`

### 计划书
- `docs/plans/PLAN_INDEX.md`
- `docs/plans/DPDK_*.pdf`
- `docs/plans/DPDK_*.docx`

---

## 下一步最值得完成什么

建议后续只盯住三件事，不要再散：

### 1. 在真实 DPDK 机器上完成构建和收发验证
- `MODE=dpdk` 编译
- 真正绑网卡
- 真正跑一轮 RX/TX
- 验证 parser / NAT / rewrite 行为

### 2. 把配置文件真正接入主路径
- `route.conf`
- `acl.conf`
- `nat.conf`

### 3. 把统计升级为 per-lcore
- 为后续多 worker 做准备

---

## 一句话总结

当前仓库已经从“工程骨架”升级为：

> **一个以现代 DPDK 为目标、Phase 1 真实主路径已经接入、mock 模式可运行、真实 DPDK 模式待目标机验证的用户态数据面平台基线。**
