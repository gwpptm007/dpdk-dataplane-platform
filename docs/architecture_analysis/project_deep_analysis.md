# dpdk-dataplane-platform 深度分析

## 一、项目定位

**一个面向现代 DPDK 21.11+ 的用户态高性能数据面平台**，同时支持 SmartNIC/DPU 的 `rte_flow` 硬件卸载和 representor/embedded-switch transfer 规则。

> [!IMPORTANT]
> 纯 C 语言（GNU C11），Meson 构建，**目标运行在 Linux**。当前 Windows 工作区仅用于代码阅读，不做真实编译或硬件验证。

| 指标 | 数值 |
|------|------|
| 版本 | v0.3.0 |
| C 源码文件 | 256 个 (.c/.h) |
| 代码行数 | ~63,800 行 |
| 文档 | ~70 篇 Markdown |
| 单元测试 | ~60+ 个测试文件 |
| 管理协议版本 | v29 |

---

## 二、仓库目录架构

```mermaid
graph TD
    Root["dpdk-dataplane-platform<br/>v0.3.0"]

    Root --> App["app/ — 可执行入口"]
    Root --> Include["include/dppd/ — 60+ 公共头文件"]
    Root --> Lib["lib/ — 7 个功能库"]
    Root --> Tests["tests/ — 单元/集成/性能"]
    Root --> Docs["docs/ — ~70 篇文档"]

    App --> dppd["dppd<br/>主 daemon 进程"]
    App --> dppctl["dppctl<br/>管理 CLI 客户端"]
    App --> tools["dppd-recovery<br/>dppd-monitor<br/>dppd-scope<br/>dppd-flow-state<br/>dppd-snapshot-migrate"]

    Lib --> runtime["runtime/<br/>config.c, runtime.c<br/>managed.c, device_recovery.c"]
    Lib --> device["device/<br/>device.c, topology.c, safety.c"]
    Lib --> control["control/ — 22 个源文件<br/>snapshot, rule, planner<br/>transaction, persistence<br/>recovery_*, age, reset"]
    Lib --> mgmt["management/<br/>server.c, flow_management.c"]
    Lib --> dp["dataplane/ — 16 个源文件<br/>packet, pipeline, worker<br/>conntrack, acl, tunnel<br/>software_backend, ipv4_router"]
    Lib --> offload["offload/<br/>rte_flow.c, rte_flow_backend.c<br/>capability.c, flow_template.c<br/>async_flow.c, shared_count.c"]
    Lib --> obs["observability/<br/>stats.c, telemetry.c<br/>rule_observation.c, rule_latency.c"]
```

---

## 三、进程启动生命周期

以下是 [main.c](file:///e:/GitHub-Study/dpdk-dataplane-platform/app/dppd/main.c) 中完整的启动→运行→退出流程：

```mermaid
sequenceDiagram
    participant Main as main()
    participant EAL as DPDK EAL
    participant RT as Runtime
    participant Ctrl as Control Service
    participant Mgmt as Management Server
    participant Worker as Worker Threads

    Main->>EAL: rte_eal_init() — 解析 -l/-a/--vdev
    Main->>Main: dppd_config_parse() — 解析 --ports/--queues 等
    Main->>Main: dppd_recovery_guard_open() — 跨进程恢复保护
    Main->>RT: dppd_runtime_init() — 发现 ethdev、配置队列/NUMA
    Main->>Ctrl: dppd_control_init() — 创建 rule repository + software backend
    Main->>RT: dppd_runtime_device_bind() — 启用事件门禁
    Main->>RT: dppd_runtime_set_software_backend() — 绑定 classifier
    Main->>Ctrl: setup_ipv4_routing() — 可选三层路由
    Main->>Ctrl: dppd_control_persistence_restore() — 从 snapshot 重放规则
    Main->>Mgmt: dppd_management_start() — 创建 Unix socket
    Main->>Main: dppd_telemetry_register() — 注册 DPDK telemetry
    Main->>RT: dppd_runtime_start() — 启动 worker 线程

    loop 主循环 (100ms 间隔)
        Main->>RT: dppd_runtime_device_poll() — 设备事件
        Main->>RT: dppd_devices_poll_links() — 链路状态
        Main->>RT: dppd_runtime_check_workers() — 线程健康
        Main->>Main: dppd_conntrack_gc() — 连接老化
        Main->>Mgmt: dppd_management_poll() — 处理 CLI 请求
        Main->>Ctrl: dppd_control_age_poll() — AGE 闲置回收
        Main->>Main: dppd_telemetry_publish_rules() — 发布规则副本
    end

    Note over Main: 收到 SIGINT/SIGTERM
    Main->>Mgmt: dppd_management_stop() — 关闭管理入口
    Main->>RT: dppd_runtime_request_stop() — 通知 worker 停止
    Main->>RT: dppd_runtime_wait() — 等待全部 worker 返回
    Main->>Ctrl: dppd_control_fini() — 删除规则、释放 backend
    Main->>RT: dppd_runtime_destroy() — 关闭端口、释放缓冲池
    Main->>EAL: rte_eal_cleanup()
```

---

## 四、报文处理完整路径

这是系统的核心——每个报文从网卡 RX 到 TX 的完整旅程：

```mermaid
flowchart TB
    subgraph Worker["Worker 主循环 (worker.c:dppd_worker_main)"]
        direction TB
        REG["注册 QSBR 读者<br/>software_backend + ipv4_router"] --> LOOP

        subgraph LOOP["while !stop_requested"]
            ND_TICK["ND6 定时器 (queue 0 only)"] --> ND_WAIT["处理 ND 等待包"]
            ND_WAIT --> PORTS["遍历全部端口"]

            subgraph PORTS_INNER["process_ingress per port"]
                RX["rte_eth_rx_burst()<br/>最多 burst_size 个包"]
                RX --> BATCH["process_batch()"]
            end

            PORTS --> QSBR["报告 QSBR 静默点<br/>允许旧规则表回收"]
        end
    end

    subgraph BATCH_DETAIL["process_batch 逐包处理"]
        direction TB
        PARSE["dppd_packet_parse_mbuf()<br/>Eth → VLAN(0-2层) → ARP/IPv4/IPv6<br/>→ TCP/UDP/Fragment → 隧道(VXLAN/Geneve/GRE)"]

        PARSE --> ND_INPUT{"ND6 报文?<br/>(NS/NA/RS/RA)"}
        ND_INPUT -->|是| ND_HANDLE["dppd_nd6_input() 处理<br/>释放 mbuf, 继续下一包"]
        ND_INPUT -->|否| DAD_CHECK{"出口 DAD 就绪?"}
        DAD_CHECK -->|否| DROP_ND["DROP (ND_NOT_READY)"]

        DAD_CHECK -->|是| CT_CHECK{"启用 conntrack?"}
        CT_CHECK -->|是| STATEFUL["stateful_decide()"]
        CT_CHECK -->|否| ROUTED["routed_decide()"]

        subgraph STATEFUL_PATH["有状态路径 (stateful_decide)"]
            direction TB
            VALIDATE["传输层校验和验证"] --> CT_BEGIN["dppd_conntrack_begin()<br/>查找/创建会话"]
            CT_BEGIN --> POLICY_ADDR["用策略地址替换报文字段<br/>(反向 NAT 恢复原始目的)"]
            POLICY_ADDR --> PIPELINE2["pipeline_decide_counted()<br/>畸形→软件规则→路由/端口对"]
            PIPELINE2 --> REWRITE_NAT["dppd_ipv4_rewrite_flow()<br/>MAC/TTL/Checksum + NAT 改写"]
            REWRITE_NAT --> CT_FINISH["dppd_conntrack_finish()<br/>确认会话 + 出口"]
        end

        subgraph ROUTED_PATH["无状态路径 (routed_decide)"]
            direction TB
            PIPELINE["pipeline_decide_counted()"]

            subgraph PIPE_INNER["Pipeline 判定优先级"]
                MAL{"畸形?"} -->|是| DROP_M["DROP MALFORMED"]
                MAL -->|否| SW_RULE["software_backend_decide_counted()<br/>精确匹配 → ACL 匹配"]
                SW_RULE -->|DROP 命中| DROP_P["DROP POLICY"]
                SW_RULE -->|ALLOW/无命中| ROUTE{"有 ipv4_router?"}
                ROUTE -->|是| LPM["LPM 查路由 + 邻居查 MAC"]
                ROUTE -->|否| PAIR["snapshot_lookup_peer()<br/>端口配对"]
                LPM -->|找到| FWD["FORWARD + 出口 + MAC"]
                LPM -->|未找到| DROP_R["DROP NO_ROUTE/NO_NEIGHBOR"]
                PAIR -->|找到| FWD2["FORWARD"]
            end

            PIPELINE --> REWRITE["dppd_ipv4_rewrite() / dppd_ipv6_rewrite()<br/>目的 MAC + 源 MAC + TTL-1 + 校验和"]
        end

        STATEFUL --> EGRESS
        ROUTED --> EGRESS

        EGRESS["按出口分组暂存 mbuf"] --> MARK["写入 MARK 到 mbuf (if has_mark)"]
        MARK --> LINEARIZE["多段 mbuf 线性化 (if needed)"]
    end

    LINEARIZE --> FLUSH["flush_egress() per 出口<br/>rte_eth_tx_burst() 批量发送"]
    FLUSH --> COUNT["dppd_software_count_batch_flush()<br/>提交归并后的规则 COUNT"]
    COUNT --> STATS["dppd_stats_add()<br/>更新 per-worker + per-port 原子统计"]
```

### 关键数据结构

**[`dppd_packet`](file:///e:/GitHub-Study/dpdk-dataplane-platform/include/dppd/packet.h#L74-L110)** — 从报文头提取的字段副本（不持有报文指针）：

| 字段 | 用途 |
|------|------|
| `l3_type` | ARP / IPv4 / IPv6 |
| `l4_type` | UDP / TCP / Fragment / Other |
| `ipv4_src_be/dst_be` | IPv4 地址 (网络字节序) |
| `ipv6_src/dst[16]` | IPv6 地址 |
| `l4_src_port_be/dst_port_be` | L4 端口 |
| `vlan_tci[2], vlan_depth` | 最多 2 层 VLAN |
| `connection_state` | CT 状态位掩码 |
| `tunnel` | VXLAN/Geneve/GRE 元数据 |

**[`dppd_pipeline_decision`](file:///e:/GitHub-Study/dpdk-dataplane-platform/include/dppd/pipeline.h#L53-L66)** — 单包判定结果：

| 字段 | 用途 |
|------|------|
| `action` | DROP 或 FORWARD |
| `drop_reason` | 19 种丢弃原因 |
| `egress_port` | 出口端口 ID |
| `has_mark / mark_id` | 软件 MARK |
| `routed / ipv4` | 三层路由结果 (出口 MAC + port) |

---

## 五、控制面：规则生命周期

```mermaid
sequenceDiagram
    participant CLI as dppctl
    participant Mgmt as Management Server
    participant Ctrl as Control Service
    participant Repo as Rule Repository
    participant Plan as Planner
    participant SW as Software Backend
    participant HW as rte_flow Backend
    participant Persist as Persistence

    CLI->>Mgmt: Unix SOCK_SEQPACKET<br/>RULE_APPLY request (v29)
    Mgmt->>Ctrl: dppd_control_apply()

    Ctrl->>Repo: 查找 rule_id + 检查 generation
    Ctrl->>Plan: dppd_planner_decide()<br/>根据 domain/fallback 规划后端

    alt 硬件后端 (REQUIRE/PREFER)
        Plan->>HW: rte_flow_validate() → rte_flow_create()
        Note over HW: validate/prepare/commit/rollback<br/>事务引擎
    else 软件后端 (SOFTWARE_ONLY 或 PREFER 回退)
        Plan->>SW: 复制快照 → 添加规则 → 原子发布<br/>旧快照交给 QSBR 回收
    end

    Ctrl->>Repo: 更新 desired state (新 generation)
    Ctrl->>Persist: 保存 snapshot v5 (CRC32)

    Ctrl-->>Mgmt: apply_result {generation, plan, tx_id}
    Mgmt-->>CLI: response
```

### 规则 IR：[`dppd_rule`](file:///e:/GitHub-Study/dpdk-dataplane-platform/include/dppd/rule.h#L101-L117)

```c
struct dppd_rule {
    uint64_t id;                    // 稳定标识
    uint64_t generation;            // 乐观并发版本号
    uint16_t install_port_id;       // 安装目标 ethdev
    enum dppd_rule_domain domain;   // INGRESS / EGRESS / TRANSFER
    enum dppd_fallback_policy fallback; // REQUIRE_HW / PREFER_HW / SOFTWARE
    uint32_t group, priority;
    struct dppd_match matches[8];   // ETH/IPv4/IPv6/TCP/UDP/RANGE/PROTOCOL/STATE
    struct dppd_action actions[8];  // DROP/QUEUE/MARK/COUNT/ALLOW/AGE/SHARED_COUNT
};
```

### 软件后端 QSBR 无锁发布机制

```mermaid
stateDiagram-v2
    [*] --> Active: init (空 classifier)

    state "控制面 (writer_lock)" as Writer {
        Active --> Copy: 复制当前 snapshot
        Copy --> Modify: 添加/删除/修改规则
        Modify --> Publish: 原子替换 active 指针
        Publish --> Retired: 旧 snapshot 加入退役链表
    }

    state "工作线程 (无锁)" as Reader {
        Read: 原子读取 active 指针
        Read --> Match: 遍历规则匹配报文
        Match --> Quiescent: 一轮端口扫描结束
        Quiescent --> Read: 报告 QSBR 静默点
    }

    Retired --> Reclaim: 所有 reader 越过安全边界
    Reclaim --> Free: 释放旧 snapshot + metrics
```

> [!TIP]
> 逐包路径**完全无锁** — worker 通过原子指针读取当前 classifier snapshot，`writer_lock` 只保护控制面的发布和回收操作。QSBR 确保旧快照在所有 worker 都报告静默点之后才被回收。

---

## 六、连接跟踪与 NAT

[`conntrack.c`](file:///e:/GitHub-Study/dpdk-dataplane-platform/lib/dataplane/conntrack.c) 实现了完整的有状态报文处理：

```mermaid
flowchart LR
    subgraph ConnTrack["连接跟踪表 (pthread_mutex)"]
        Hash["哈希桶数组<br/>buckets[]"]
        Entries["connection entries[]<br/>双向索引"]
        Free["空闲槽位栈<br/>free_slots[]"]
        Ports["SNAT 端口位图<br/>ports[2][2][65536]"]
    end

    subgraph States["TCP 状态机"]
        NONE --> SYN --> SYN_ACK --> OPEN --> FIN --> CLOSED
    end

    subgraph NAT["NAT 类型"]
        SNAT["SNAT<br/>出站地址转换<br/>端口池分配"]
        DNAT["DNAT<br/>静态端口转发<br/>双向回包"]
        NAT66["NAT66<br/>IPv6 地址转换"]
    end

    PKT["入站报文"] --> BEGIN["conntrack_begin()<br/>查找或创建会话"]
    BEGIN --> |"已有会话"| UPDATE["更新超时+统计<br/>返回 ESTABLISHED"]
    BEGIN --> |"新建会话"| CREATE["分配槽位<br/>检查 DNAT 规则<br/>分配 SNAT 端口<br/>返回 NEW"]
    BEGIN --> |"无效"| INVALID["返回 INVALID<br/>无法提取五元组"]

    UPDATE --> FINISH["conntrack_finish()<br/>确认出口+提交"]
    CREATE --> FINISH
```

### 核心字段 [`dppd_conntrack`](file:///e:/GitHub-Study/dpdk-dataplane-platform/lib/dataplane/conntrack.c#L19-L29)

| 字段 | 说明 |
|------|------|
| `entries[]` | 连接条目数组 (哈希链表) |
| `buckets[]` | 哈希桶 (指向 entries 的索引) |
| `free_slots[]` + `free_count` | 空闲槽位栈 |
| `ports[2][2][65536]` | SNAT 端口占用位图 (per 方向, per 协议) |
| `dnat[32]` | 静态 DNAT 规则 |
| `clock_ms` | 单调时钟 |
| `status` | 统计: active/peak/created/expired/flushed |

---

## 七、管理协议 (v29)

[`management.h`](file:///e:/GitHub-Study/dpdk-dataplane-platform/include/dppd/management.h) 定义了 daemon 与 CLI 之间的 IPC：

| 属性 | 值 |
|------|-----|
| 传输 | Unix `SOCK_SEQPACKET` (保留消息边界) |
| 默认路径 | `/tmp/dppd-control.sock` |
| 版本检查 | 严格匹配，不兼容即拒绝 |
| 并发模型 | 主线程串行 poll，无需额外加锁 |

### 支持的 50+ 种操作

````carousel
**规则管理**
- `RULE_APPLY` — 单条创建/更新
- `RULE_CREATE_BATCH` — 2-4 条原子创建
- `RULE_DELETE` / `RULE_DELETE_BATCH` — 精确版本删除
- `RULE_UPDATE_BATCH` — 2-4 条原子替换
- `RULE_GET` / `RULE_LIST` — 查询
- `RULE_COUNT_QUERY` — 硬件/软件计数
- `RULE_STATUS` — 实际后端+安装记录
- `RULE_METRICS` / `RULE_LATENCY` / `RULE_HISTORY`
<!-- slide -->
**网络功能**
- `ROUTING_*` — IPv4 路由/邻居 CRUD + 原子替换
- `ROUTE6_*` / `NEIGHBOR6_*` — IPv6 路由/邻居
- `CONNTRACK_*` — 会话查询/清空
- `NAT_STATUS` / `DNAT_LIST` — NAT 状态
- `ND6_*` — 动态邻居发现管理
- `TRANSFER_PLAN` / `TRANSFER_APPLY` — embedded-switch 规则
- `AGE_STATUS` / `RULE_AGE_QUERY` — AGE 闲置老化
- `SHARED_COUNT_*` — 共享 COUNT 生命周期
<!-- slide -->
**系统运维**
- `PING` — 心跳 (返回 generation + rule count)
- `PORT_GET` — 端口详细信息 (MAC/driver/link/capabilities)
- `STATS_QUERY` — per-port/queue 收发统计
- `HEALTH_GET` — 就绪状态 + 12 种 blocker 位图
- `PERSISTENCE_STATUS/FLUSH` — 快照持久化管理
- `RECOVERY_STATUS/RETRY` — 恢复隔离操作
- `CAPABILITY_GET/PROBE/CLEAR` — 网卡能力画像
- `DEVICE_STATUS/RESET` — 设备事件与复位
- `FLOW_*` — M4 在线配置事务
````

---

## 八、恢复与持久化体系

```mermaid
flowchart TD
    subgraph Persistence["快照持久化"]
        V2["v2: 基础规则"]
        V3["v3: + install_port"]
        V4["v4: + AGE"]
        V5["v5: + shared COUNT + CRC32"]
    end

    subgraph Recovery["跨进程恢复保护"]
        Guard["recovery_guard<br/>安装前记录意图<br/>安装后记录结果"]
        Inspect["recovery inspect<br/>离线核对 TAP TC 规则"]
        Plan["recovery plan<br/>列出候选+拒绝原因"]
        Monitor["dppd-monitor<br/>TAP 生命周期监护"]
        Scope["dppd-scope<br/>受控 TAP 恢复闭环"]
    end

    subgraph Startup["启动恢复流程"]
        CHECK{"recovery 文件<br/>有未确认记录?"} -->|有| BLOCK["阻止启动<br/>需外部清理确认"]
        CHECK -->|无| LOAD["加载 snapshot v5"]
        LOAD --> REPLAY["fail-closed 重放<br/>逐条 plan + 事务"]
        REPLAY -->|成功| RUN["正常运行"]
        REPLAY -->|失败| ISOLATE["恢复隔离模式<br/>只保留诊断入口<br/>不启动 worker"]
    end
```

---

## 九、里程碑状态

| 阶段 | 状态 | 核心交付 |
|------|------|----------|
| **M2** | ✅ 完成 | 软件数据面：解析、port-pair、路由、ACL、ConnTrack、NAT、IPv6、ND6 |
| **M3** | ✅ 完成 | 硬件闭环：rte_flow validate/create、transfer、AGE、共享 COUNT、角色发现 |
| **M4** | ✅ 完成 | Flow Template、有界异步队列、资源配额、背压、性能门禁 (82/82 单测) |
| **M5** | 🔶 进行中 | 配置持久化、进程重启恢复、在线模板事务 |

> [!NOTE]
> 物理网卡卸载、真实复位恢复和实机验收尚未完成。可通过 `python3 scripts/simulate_m3.py demo` 在本地模拟全功能演练。

---

## 十、核心设计亮点

### 1. 逐包路径零锁
Worker 通过原子指针读取不可变 snapshot，QSBR 保障旧版本安全回收，报文处理路径无 mutex/spinlock。

### 2. 统一规则 IR + 可解释 Planner
一份 `dppd_rule` 定义同时支持硬件 (`rte_flow`) 和软件 (classifier snapshot) 两条路径，Planner 给出可审计的后端选择理由。

### 3. 事务引擎 + 乐观并发
每个规则有稳定 ID + monotonic generation，validate → prepare → commit / rollback 四阶段事务，补偿失败进入恢复隔离而非静默丢数据。

### 4. 精细统计体系
[46 种原子统计指标](file:///e:/GitHub-Study/dpdk-dataplane-platform/include/dppd/stats_values.h)，per-worker + per-port 独立累计，19 种丢弃原因精确分类，cache line 对齐避免 false sharing。

### 5. 多层恢复保护
从 fail-closed 启动重放到跨进程安装日志、TAP 原生标识核对、监护进程握手、受控命名空间闭环恢复，构成完整的故障恢复链。
