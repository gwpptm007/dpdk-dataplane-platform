# 项目评审报告

> 评审时间：2026-04-06
> 评审范围：dpdk-dataplane-platform 全局

---

## 一、第一次评审：基础布局与学习路径

### 1.1 整体评价

**当前骨架设计思路正确**，分层清晰（平台层/报文层/决策层/观测层/扩展层），目录规划合理，为后续演进预留了接口。Phase 1~4 的路线图清晰，适合学习者按阶段推进。

### 1.2 布局问题与建议

| 问题 | 建议 |
|------|------|
| `lib/flow/` 和 `lib/offload/` 平铺，control-plane 和 data-plane 边界模糊 | 考虑 `lib/dataplane/` vs `lib/control/` 分离，或按协议族分子目录 |
| `app/` 混合了 control-plane 代码 (args.c) 和 data-plane (main_loop.c) | 把 `args.c`/`port.c` 归为 `app/` 下的 `ctrl.c`/`port_init.c`，快路径代码保持精简 |
| 缺少 `include/` 公共头文件目录 | 建议增加 `include/dppd/` 放公共类型定义，避免相对路径 include |
| `meson.build` all-in-one 编译 | 建议按 `lib/` 子目录各出 `meson.build`，主 `meson.build` 用 `subdir()` 汇总 |

### 1.3 学习路径建议（按优先级）

```
第一优先级（必须精通）
├── rte_mbuf / rte_pktmbuf        — 报文核心数据结构，最常考
├── rte_ethdev 收发               — port → rxq/txq → burst
├── rte_mempool                   — mbuf pool 创建与挂核
└── rte_lcore / worker            — 多核模型，remote_launch

第二优先级（面试高频）
├── rte_ipv4_hdr / rte_ether_hdr  — 二三层头解析，务必能手动解析
├── rte_flow                      — 规则抽象，Phase 4 核心
├── RSS /多队列                   — 理解队列映射关系
└── mbuf refcnt / headroom/tailroom

第三优先级（锦上添花）
├── rte_acl                       — DPDK ACL 库
├── rte_hash / rte_lpm            — 查表加速
└── Representor / PF-VF          — SmartNIC 概念
```

### 1.4 文件名建议

| 当前 | 建议 | 原因 |
|------|------|------|
| `main_loop.c` | `worker.c` | "main_loop" 易混淆，worker 更直观 |
| `port.c` | `port_init.c` | 强调初始化职责 |
| `pipeline.c` | `pipeline_fwd.c` | 明确是转发管道 |
| `rewrite.c` | `pkt_rewrite.c` | 避免与 `rewrite.h` 重名误会 |

### 1.5 代码深度建议

当前骨架偏浅，**Phase 1 建议达到的深度**：

**必须手写，不能只调库：**
- ARP table 维护（静态/动态）
- 路由查表（LPM 或 hash）
- NAT 转换（session table + aging）
- Checksum offload 标志位设置（`RTE_MBUF_TX_*`）
- mbuf 手动构造和克隆

**理解原理而非调库：**
- `rte_pktmbuf_alloc` 背后从哪个 pool 取
- `rte_eth_rx_burst` 为什么用 scalar 而非 vector
- RSS hash field 怎么配置 (`ETH_RSS_*`)

### 1.6 其他建议

**1. 增加 `examples/` 目录**
放"最小可运行片段"：单口发包、纯 ARP 响应、纯 UDP echo 等，便于拆解调试。

**2. 测试建议用 `dpdk-pdump` + Scapy 组合**
当前 Scapy 测试 OK，建议增加 `dpdk-pdump` 转储 pcap，便于用 Wireshark 确认报文内容。

**3. 简历亮点写法**
建议把 Phase 1 的成果描述为：
> "基于 DPDK 实现用户态数据面，支持 ARP/IPv4/UDP 解析、多核 worker、ACL + NAT 策略管道，预留 rte_flow offload 接口"

---

## 二、第二次评审：SmartNIC / DPU / rte_flow / Representor / Transfer

### 2.1 当前 offload 抽象的问题

`lib/offload/offload.h` 只有一个裸 `struct dppd_rule`，Phase 4 无法直接映射到 rte_flow pattern/action。

**问题清单：**

| 问题 | 严重性 | 说明 |
|------|--------|------|
| `dppd_rule` 无 pattern/action 分离 | 高 | rte_flow 本质是 pattern × action，无法用单一结构表达 |
| 无 traffic direction 抽象 | 高 | SmartNIC 分 PF/VF/AF_XDP，方向概念缺失 |
| 无 priority / meter / counter 抽象 | 中 | 真实网卡 offload 必有这些 |
| 无 representor 关联 | 高 | DPU 场景没有 representor 就无法管理 VF |
| 无 queue action 抽象 | 中 | RSS/VPQ/流量镜像都需要 queue action |

### 2.2 建议重构 offload 层

```c
// lib/offload/match.h  — 建议新增
struct dppd_match {
    enum {
        DPPD_MATCH_IPV4_5TUPLE,
        DPPD_MATCH_IPV4_SRC,
        DPPD_MATCH_IPV4_DST,
        DPPD_MATCH_L4_PORT,
        // ... extensible
    } type;
    uint32_t value;
    uint32_t mask;
};

struct dppd_action {
    enum {
        DPPD_ACTION_RSS,
        DPPD_ACTION_QUEUE,    // 定向到某队列
        DPPD_ACTION_DROP,
        DPPD_ACTION_COUNT,
        DPPD_ACTION_MARK,     // metadata mark
        DPPD_ACTION_NAT,     // NAT 转换
    } type;
    uint32_t queue_id;       // for RSS/QUEUE
    uint32_t counter_id;
};

// lib/offload/flow_obj.h  — 建议新增
struct dppd_flow_obj {
    enum dppd_offload_backend {
        DPPD_BACKEND_SOFT,        // 软件查表
        DPPD_BACKEND_RTE_FLOW,   // 硬件 rte_flow
    } backend;

    struct dppd_match  match;
    struct dppd_action action;

    // SmartNIC 特有
    uint16_t          dst_port_id;   // 目标 port (PF/VF/representor)
    uint16_t          priority;
    bool              transfer;      // 跨域 transfer 规则
};
```

### 2.3 DPU 场景必须增加的模块

```
lib/offload/
├── offload.h              # 抽象接口
├── offload_soft.c         # 软件后端（当前 OK）
├── offload_rte_flow.c     # rte_flow 映射（当前太简）
├── offload_hw.c           # 建议新增：直接调用 rte_flow 的硬件后端
├── representor.c          # 建议新增：PF/VF representor 管理
└── switch.c               # 建议新增：transfer pipeline（跨域）
```

### 2.4 rte_flow 缺失的关键能力

当前 `offload_rte_flow.c` 是占位符，真实 rte_flow 适配需要：

**必须实现的 pattern：**
- `RTE_FLOW_ITEM_TYPE_ETH` — MAC 匹配
- `RTE_FLOW_ITEM_TYPE_IPV4` — IP 匹配
- `RTE_FLOW_ITEM_TYPE_UDP` / `TCP` — L4 匹配
- `RTE_FLOW_ITEM_TYPE_PORT_ID` — 跨端口转发（representor 场景）
- `RTE_FLOW_ITEM_TYPE_VF` — VF 定向（PF-VF 场景）

**必须实现的 action：**
- `RTE_FLOW_ACTION_TYPE_RSS` — RSS 分流
- `RTE_FLOW_ACTION_TYPE_QUEUE` — 队列定向
- `RTE_FLOW_ACTION_TYPE_DROP` — 丢弃
- `RTE_FLOW_ACTION_TYPE_COUNT` — 统计
- `RTE_FLOW_ACTION_TYPE_OF_PUSH_VLAN` — VLAN 重写
- `RTE_FLOW_ACTION_TYPE_PHY_PORT` — 出端口重定向

**建议增加 `lib/offload/rte_flow_map.c`：**
```c
// 将 dppd_flow_obj 映射为 rte_flow_attr + pattern[] + action[]
static int
dppd_rte_flow_build(uint16_t port_id,
                    const struct dppd_flow_obj *flow,
                    struct rte_flow **flow_out)
{
    struct rte_flow_attr attr = {
        .ingress = 1,    // 取决于 direction
        .transfer = flow->transfer,
        .priority = flow->priority,
    };
    // ... build pattern[] and action[] from flow->match / flow->action
}
```

### 2.5 Representor 场景

DPU/SmartNIC 上 `ethtool -S` 能看到 VF 统计，靠的就是 representor。

**当前仓库完全没有 representor 相关设计**，建议 Phase 2~3 增加：

```
docs/scenario/
├── pf_vf_sf_design.md     # PF-VF-SF 场景说明
├── representor_api.md      # representor 编程接口
└── offload_pipeline.md     # 软硬协同 pipeline
```

### 2.6 Transfer 规则（跨域转发）

transfer 是 DPU 的核心能力——在 VF 和物理端口之间转发不过 CPU。

当前 pipeline 是纯软转发（收到→CPU→发走），**缺少 transfer bypass 路径**：

```c
// 建议在 pipeline.h 增加
enum dppd_pipeline_path {
    DPPD_PATH_SW,        // 软件转发（当前）
    DPPD_PATH_HW_RTE,    // rte_flow 硬件转发
    DPPD_PATH_TRANSFER,  // 跨域 transfer（不过 CPU）
};

// main_loop.c worker 增加
switch (dppd_pipeline_path(pkt)) {
case DPPD_PATH_SW:
    // 当前逻辑
    break;
case DPPD_PATH_HW_RTE:
    // rte_flow offload，直接 HW 转发
    break;
case DPPD_PATH_TRANSFER:
    // 跨域，不经过 CPU
    break;
}
```

### 2.7 交换机 / NIC / 网络芯片场景适配

| NIC 能力 | 当前抽象 | 建议 |
|----------|----------|------|
| Flow Director (FDIR) | 无 | 在 `offload.h` 增加 `DPPD_BACKEND_FDIR` |
| RSS (Toeplitz hash) | `nb_rxq` 参数 | 显式 `struct dppd_rss_conf` |
| Flow aging | 无 | session table 配套 aging 机制 |
| Wire-speed forwarding | 无 | pipeline 加 bypass 路径 |
| Packet pacing | 无 | Phase 3 后考虑 |

**不同芯片场景的适配思路：**

```
Intel (受限于 ICE/IRD)
  → rte_flow + FDIR + RSS
  → 内联 checksum offload

NVIDIA (DOCA / BlueField DPU)
  → representor + DVFlow
  → 对外呈现为 ethdev

Broadcom (StrataXGS + DUNE)
  → 交换芯片独立 pipeline
  → 需要学习芯片 SDK

Marvell (OCTEON + CN10K)
  → 内部 pipeline 自有语言
  → 建议用 CN10K SDK
```

**建议 Phase 3 结束时，能说清楚：**"我这套软件在 Intel NIC 上用 rte_flow，在 BlueField 上用 DOCA DVFlow，在裸金属上用软件路径"。

---

## 三、目录重组建议

基于 SmartNIC/DPU 场景，建议重组项目结构：

```
dpdk-dataplane-platform/
├── app/                    # 主程序（不变）
├── lib/
│   ├── common/             # 通用（不变）
│   ├── pkt/                # 报文层（不变）
│   ├── flow/               # 决策层（不变）
│   ├── offload/            # 重构为三层抽象
│   │   ├── core/           # 抽象接口层
│   │   ├── soft/           # 软件后端
│   │   ├── rte_flow/       # rte_flow 后端
│   │   └── hw/             # 硬件特定后端（ice/ird/cn10k...）
│   ├── representor/        # 新增：representor 抽象
│   └── switch/             # 新增：transfer pipeline
├── include/dppd/           # 新增：公共头文件
│   ├── app.h
│   ├── common.h
│   ├── pkt.h
│   └── offload.h
├── scenarios/              # 新增：各芯片场景设计文档
│   ├── intel_icelake.md
│   ├── nvidia_bluefield.md
│   └── generic_rm_abstract.md
└── docs/phase4_offload_design.md  # 新增：Phase 4 详细设计
```

---

## 四、学习路线图（SmartNIC/DPU 方向增强版）

```
Phase 1（打牢基础）
  ↓
Phase 2（并行与策略）
  ├── 多队列 + RSS + 多核 worker
  ├── rte_flow 入门：pattern × action 关系
  └── ACL/NAT 完整实现
  ↓
Phase 3（可观测性 + 回归）
  ├── 添加 rte_flow 配置导出/回放
  ├── 学习 ethdev stats → 芯片 counter 映射
  └── 场景测试：Scapy × DPDK pdump × Wireshark
  ↓
Phase 4（offload 抽象）← 重构点
  ├── 重构 offload.h → pattern/action 分离
  ├── representor 基础（pf_ring 或者 BlueField SDK）
  ├── transfer pipeline 概念
  └── 对比软硬路径行为差异（benchmark）
  ↓
后续（加分项）
  ├── DOCA SDK on BlueField（NVIDIA 官方 DPU 编程框架）
  ├── Intel IPU / FPGA 加速卡
  └── 自定义 rte_flow pattern 开发
```

---

## 五、面试储备建议

如果目标是 DPU/SmartNIC 岗位，光有项目骨架不够，面试官会问：

**高频问题清单：**
1. rte_flow 的 pattern 匹配顺序？priority 相同时哪个优先？
2. RSS 和 FDIR 的区别？能否同时使用？
3. representor 的作用？如何通过 representor 管理 VF？
4. DPU 和 SmartNIC 的区别？
5. transfer 规则和普通 ingress 规则的区别？
6. mbuf 如何携带 metadata 在 pipeline 层间传递？
7. BlueField 上的 ENETC 和 vSwitch DPA 是什么关系？

**建议：** Phase 4 完成后，在 `scenarios/` 里写清楚每个场景的"如何配置 + 预期行为 + 验证方法"，面试时可以主动说："我在软件路径和 rte_flow 路径之间做了行为对比验证"。

---

## 六、优先级改进清单

| 优先级 | 改进项 | 理由 |
|--------|--------|------|
| **P0** | 重构 `offload.h`，引入 pattern/action 分离 | 否则 Phase 4 无法落地 |
| **P0** | `parser.c` 增加 TCP 解析骨架 | 面试/实用都必需 |
| **P1** | 增加 `include/dppd/` 公共头目录 | 解决相对路径 include |
| **P1** | `main_loop.c` 加 TX checksum offload 标志 | 真实网卡的核心能力 |
| **P2** | 增加 `lib/offload/rte_flow_map.c` | 从软到硬的桥梁 |
| **P2** | 增加 `scenarios/` 目录 + Intel/NVIDIA 场景文档 | 面试加分项 |
| **P3** | 增加 `lib/representor/` 骨架 | DPU 必需 |
| **P3** | `meson.build` 拆分为 subdir 方式 | 构建解耦 |
