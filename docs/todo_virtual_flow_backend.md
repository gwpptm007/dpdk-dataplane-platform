# 待办：虚拟测试端口的规则执行能力

状态：阶段 1（TAP 基础 flow）、阶段 2 最小 software backend、immutable snapshot + QSBR 回收已完成；QUEUE 语义和规模化仍为 `DEFERRED`

记录日期：2026-07-23

计划归属：M1 software adapter、M2 可组合软件 pipeline

## 0. 2026-08-30 TAP 验证结果

已在测试机的 DPDK 21.11.9 上完成双 TAP 端口的真实 `rte_flow` 闭环。验证使用
`--no-huge --no-pci -m 64`、两个 `net_tap` vdev、独立 EAL file-prefix 和临时控制
socket，全程未修改 `ens192`。

- 无规则时，从 `dppdtap0` 注入的标记 Ethernet 帧可由 `dppdtap1` 收到，确认软件
  port-pair 基线转发；
- `apply-drop 100 0 0 10 require` 返回真实 backend，随后同类标记帧不能到达
  `dppdtap1`，确认 DROP 不是 management 层的伪成功；
- 删除 DROP 后，下发带 IPv4/TCP 五元组匹配的 `queue:0`、`require` 规则成功；匹配
  TCP 标记帧可到达 `dppdtap1`，证明 TAP PMD 能完成该 flow 的 validate/create 与真实
  数据路径命中；
- 删除 QUEUE 规则后 repository 为空，匹配帧恢复由软件 port-pair 转发；daemon 正常
  退出后控制 socket、两个 TAP 接口和临时日志均已清理。

本轮只证明 TAP 的 ETH/IPv4/TCP、DROP/QUEUE 和 create/destroy 生命周期。TAP 官方能力
范围不包含可作为验收依据的 MARK/COUNT；也没有 per-queue 可观测指标，因此不能从该结果
推出硬件队列调度性能或 MARK/COUNT 支持。

软件 backend 随后已完成最小闭环：`net_ring` 上的 `prefer` TCP/DROP 在硬件 validate
返回 `ENOSYS` 后以 `software-fallback` 发布，`require` 保持 `ENOSYS` 失败；双 TAP 上
强制 `software` 的 TCP+MARK(42)+COUNT+DROP 规则阻断真实标记帧，`dppctl count` 返回
`hits=1 bytes=76`。随后已切换到 immutable snapshot + DPDK QSBR：规则更新不再获取报文路径读锁，worker 在完整 ingress 扫描结束处报告静默点，旧 snapshot 在非阻塞宽限期检查通过后释放。

2026-08-30 补充常驻验证：双 `net_ring` worker 启动后连续执行 40 次 software-only
规则发布/删除，管理 socket 保持可用，daemon 退出后 socket 清理成功。该用例验证了
QSBR reader 生命周期和反复发布/回收；`net_ring` 本身仍没有可注入的真实 flow 数据面。

## 1. 背景与结论

当前测试机使用双 `net_ring` 运行无物理端口 smoke test。`net_ring` 的职责是把
`rte_ring` 软件 FIFO 包装成 ethdev，它没有实现 DPDK 21.11 的 `rte_flow`
operations。因此规则可以完成管理协议、IR、planner 和 transaction 的前置验证，
但在调用 `rte_flow_validate()` 时会返回 `-ENOSYS`，不能创建真实 flow，也不能
产生 MARK、COUNT 或 hits/bytes。

这不是缺少 EAL 参数或 dppd 配置，不能通过“打开某个开关”修复。DPDK 的通用
flow API 由各 PMD 自行实现，PMD 不支持的 item/action 组合通常不会自动用软件
模拟。参考：

- [DPDK 21.11 Generic flow API](https://doc.dpdk.org/guides-21.11/prog_guide/rte_flow.html)
- [DPDK 21.11 TAP PMD Flow API](https://doc.dpdk.org/guides-21.11/nics/tap.html#flow-api-support)
- [DPDK 21.11 SoftNIC Flow API](https://doc.dpdk.org/guides-21.11/nics/softnic.html#flow-api-support)

## 2. 测试机现状

测试机当前已确认：

- Linux kernel `6.8.0-124-generic`；
- `CONFIG_TUN=y`；
- `CONFIG_NET_CLS_FLOWER=m`；
- 已安装 DPDK 21.11 的 `librte_net_tap.so`；
- 已安装 `librte_net_ring.so`；
- VMware 数据口 `ens192` 使用 `vmxnet3`，已完成 PMD probe、queue start/stop，
  但不作为完整 rte_flow/MARK/COUNT 验收设备。

## 3. 后续实施路线

### 3.1 第一阶段：使用 TAP 验证基础 flow

目的：在没有支持 rte_flow 的物理 NIC 时，先形成真实报文的规则
validate/create/destroy 与 DROP/QUEUE 功能证据。

准备：

```bash
sudo modprobe cls_flower
sudo modprobe sch_ingress
```

建议启动方式：

```bash
sudo ./build/dppd \
  -l 0-2 --no-huge --no-pci -m 512 \
  --vdev=net_tap0,iface=dppdtap0,mac=fixed \
  --vdev=net_tap1,iface=dppdtap1,mac=fixed \
  -- \
  --ports 0,1 --queues 1 --mbufs 4096 --cache 64
```

DPDK 21.11 TAP PMD 使用 Linux TC flower 实现 flow，适合验证：

- ETH、VLAN、IPv4/IPv6、UDP/TCP match；
- DROP；
- QUEUE；
- RSS（受内核和队列配置限制）；
- create/destroy/flush 生命周期。

TAP 官方支持列表不包含 MARK 和 COUNT，因此不得用 TAP 的结果宣称完整 action
能力已经通过。可使用以下命令检查实际 TC 规则和计数：

```bash
sudo tc -s filter show dev dppdtap0 parent 1:
```

### 3.2 第二阶段：实现平台内 software backend

目的：让 `net_ring`、`net_tap` 和不支持 rte_flow 的 PMD 也能执行与 rule IR
语义等价的软件规则，并成为稳定 CI 环境。

推荐在 dpdk-platform 内实现，不修改 DPDK ring PMD：

```text
Canonical Rule IR
        |
        v
     Planner
      /   \
rte_flow   software backend
   |              |
物理 NIC       worker pipeline
```

最小交付范围：

- planner 根据 PMD validate 结果和 fallback policy 选择 software backend；
- Ethernet、IPv4、UDP、TCP match；
- DROP；
- 软件 MARK，明确 mbuf metadata 的字段/动态字段所有权；
- 软件 COUNT，提供 per-rule packets/bytes；
- immutable classifier snapshot；
- generation replacement；
- transaction prepare/commit/rollback adapter；
- worker quiescent/QSBR 后回收旧 snapshot；（已完成）
- `dppctl count` 对硬件和软件 backend 使用一致的响应语义。

QUEUE 的软件等价语义需要先单独设计。硬件 QUEUE 表示 NIC 在 RX 前选择目标队列，
而包进入当前 worker 后再“切换 RX queue”并不等价。实现前必须决定它表示 worker
handoff、软件 stage queue，还是明确标记为无等价 fallback。

### 3.3 第三阶段：物理 NIC/SmartNIC 验收

目的：验证真实硬件 offload、MARK、COUNT、hits/bytes 和资源限制。

优先选择支持目标 rte_flow 组合的 PMD，例如：

- NVIDIA/Mellanox `mlx5`；
- Intel E810 `ice`；
- Intel X710/XL710 `i40e`；
- 视 feature matrix 决定的 `ixgbe`、`bnxt` 等。

建议通过 PCI passthrough 或 SR-IOV VF 提供给测试机，绑定 `vfio-pci` 后逐条运行
`rte_flow_validate()`。即使 PMD整体支持 rte_flow，也不能假设所有
match/action/transfer 组合均受支持。

## 4. 不推荐方案

不建议 fork DPDK 并直接给 `net_ring` PMD 增加 rte_flow。该方案需要在 PMD 内
长期维护：

- `flow_ops_get` 和 validate/create/destroy/flush/query；
- RX burst 路径的软件分类器；
- rule object 与 counter 生命周期；
- 多队列并发和规则更新同步；
- 与上游 DPDK 21/25 的双版本补丁。

这些逻辑本质上属于平台 software backend。放在项目 planner/transaction/pipeline
中更容易测试，也不会制造私有 DPDK 分支。

DPDK SoftNIC 虽然提供 flow API 和可编程 pipeline，但 DPDK 21.11 文档明确提示其
底层 PMD 对 COUNT 和 DROP 仍有限制，并且需要 firmware、pipeline 映射及专用运行
core；它可以作为专项实验对象，不作为当前首选 CI backend。

## 5. 验收标准

### TAP 阶段

- `rte_flow_validate/create/destroy` 对 DROP、QUEUE 成功；
- host 注入的匹配/非匹配报文产生可重复的差异结果；
- `tc filter show` 能看到与 rule id/generation 对应的规则；
- 更新和删除失败不污染 desired repository；
- daemon 退出后无残留 TC 规则或 TAP 设备。

### Software backend 阶段

- `net_ring` 上 match/DROP/MARK/COUNT 有真实报文测试；
- 软件 COUNT 的 packets/bytes 与发包数量一致；
- `PREFER_HARDWARE` 只在软件语义等价时降级；
- `REQUIRE_HARDWARE` 在 ring/tap 上保持明确失败；
- generation 更新期间无半发布状态；
- 旧 snapshot 在所有 worker quiescent 后回收；
- 与硬件 backend 共用 management API 和错误分类。

### 物理硬件阶段

- 记录 NIC、固件、PMD、DPDK 版本和 devargs；
- 为每个 match/action 组合保存 validate/create/query 结果；
- COUNT hits/bytes 与外部流量源和 NIC 统计能够交叉核对；
- 证明 offloaded packets 是否进入 CPU worker；
- 形成至少一个非 mlx5 PMD 的对照报告。

## 6. 恢复本待办的触发条件

满足以下任一条件时重新评估：

- 需要在 CI 中验证真实软件规则而不依赖物理 NIC；
- 测试机获得支持 rte_flow 的 PCI passthrough/SR-IOV 设备；
- 对 `net_ring` 进行软件 classifier 的真实报文压力与并发更新验证；
- 需要对 MARK/COUNT 的端到端数据面行为提供正式验收证据。
