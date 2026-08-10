# DPDK Dataplane Platform V4 — 架构画布

> 本文只定义平台级总体结构、一级模块边界和关键闭环，不进入 C API、结构体、RPC schema、数据库表或线程实现。

## 1. 画布目标

平台不是一个“增强版 l2fwd”，而是一个持续管理 desired state、执行计划和实际数据面状态的系统：

- 用户声明想要的网络策略，而不是直接调用某个 PMD；
- 平台解析 ethdev、PF/VF/SF、representor 和 embedded-switch 拓扑；
- planner 根据能力、资源、策略约束选择 software、`rte_flow` 或未来 vendor backend；
- transaction engine 负责有序提交、失败回滚和 generation 发布；
- reconciler 持续比较 desired state 与 actual state，并在设备变化或进程恢复后重建；
- dataplane executor 只执行已经发布的软件 snapshot，不承担平台控制逻辑。

---

## 2. L0：系统总画布

```mermaid
flowchart TB
    subgraph UserSpace["使用者与外部系统"]
        Operator["Operator / Developer"]
        Controller["External Controller / Orchestrator"]
        CI["CI / Test / Benchmark"]
    end

    subgraph Northbound["Northbound Management Plane"]
        CLI["dppctl"]
        API["Management API"]
        Config["Versioned Config / Intent"]
    end

    subgraph Platform["dppd Platform Control Plane"]
        Model["Intent + Canonical Model"]
        State["Desired / Planned / Actual State"]
        Topology["Topology + Capability Inventory"]
        Planner["Planner + Compiler Registry"]
        Txn["Transaction Engine"]
        Reconciler["Reconciler"]
    end

    subgraph Backends["Execution Backends"]
        SWB["Software Backend"]
        RFB["rte_flow Backend"]
        Vendor["Future Vendor Backend"]
    end

    subgraph Runtime["Runtime Data Plane"]
        Snapshot["Versioned SW Snapshot"]
        Workers["RTC / Queue Workers"]
        HWObjects["Flow / Counter / Shared Objects"]
    end

    subgraph Devices["NIC / SmartNIC / DPU"]
        Ethdev["Physical ethdev"]
        Reps["PF / VF / SF Representors"]
        Eswitch["Embedded Switch"]
    end

    subgraph Operations["Operations"]
        Telemetry["Metrics / Telemetry"]
        Audit["Audit / Event Journal"]
        Health["Health / Alarm / Diagnostics"]
    end

    Operator --> CLI
    Controller --> API
    CI --> API
    CLI --> API
    Config --> API
    API --> Model
    Model --> State
    State --> Planner
    Topology --> Planner
    Planner --> Txn
    Txn --> SWB
    Txn --> RFB
    Txn --> Vendor
    SWB --> Snapshot
    Snapshot --> Workers
    RFB --> HWObjects
    Vendor --> HWObjects
    Workers --> Ethdev
    HWObjects --> Ethdev
    HWObjects --> Eswitch
    Reps --> Eswitch
    Ethdev --> Topology
    Reps --> Topology
    Eswitch --> Topology
    Reconciler --> State
    Reconciler --> Topology
    Reconciler --> Txn
    Platform --> Telemetry
    Txn --> Audit
    Runtime --> Telemetry
    Devices --> Health
    Health --> Reconciler
```

### 总体边界

- Northbound 描述“想要什么”。
- Control Plane 决定“应该怎样实现”。
- Backend 负责“在某一种执行环境中落地”。
- Runtime 承载“当前已提交的实际执行对象”。
- Reconciler 保证系统不会只在第一次启动时正确。
- Operations 让每次选择、降级、失败和恢复都可解释。

---

## 3. L1：平台内部模块总图

```mermaid
flowchart LR
    subgraph Entry["入口层"]
        ApiServer["API Server"]
        Command["Command Handler"]
        Query["Query Service"]
    end

    subgraph Domain["领域模型层"]
        Intent["Intent Model"]
        Endpoint["Endpoint Model"]
        RuleIR["Canonical Rule IR"]
        PlanModel["Execution Plan"]
        StatusModel["Resource Status"]
    end

    subgraph StateLayer["状态层"]
        DesiredRepo["Desired-state Repository"]
        ActualRepo["Actual-state Repository"]
        TxnJournal["Transaction Journal"]
        EventBus["Internal Event Bus"]
    end

    subgraph Intelligence["决策层"]
        Resolver["Selector Resolver"]
        Normalizer["Normalizer / Validator"]
        Capability["Capability Evaluator"]
        Partitioner["Backend Partitioner"]
        Admission["Resource Admission"]
        Compiler["Compiler Registry"]
    end

    subgraph Convergence["收敛层"]
        TxManager["Transaction Manager"]
        ReconcileLoop["Reconcile Loop"]
        Recovery["Recovery / Replay"]
    end

    subgraph Execution["执行层"]
        SWBackend["Software Backend"]
        FlowBackend["rte_flow Backend"]
        RuntimeMgr["Runtime Manager"]
        DeviceMgr["Device Manager"]
    end

    ApiServer --> Command
    ApiServer --> Query
    Command --> Intent
    Intent --> Normalizer
    Endpoint --> Resolver
    Resolver --> Normalizer
    Normalizer --> RuleIR
    RuleIR --> DesiredRepo
    DesiredRepo --> Capability
    Capability --> Partitioner
    Partitioner --> Admission
    Admission --> Compiler
    Compiler --> PlanModel
    PlanModel --> TxManager
    TxManager --> SWBackend
    TxManager --> FlowBackend
    SWBackend --> RuntimeMgr
    FlowBackend --> DeviceMgr
    RuntimeMgr --> ActualRepo
    DeviceMgr --> ActualRepo
    TxManager --> TxnJournal
    ActualRepo --> Query
    DesiredRepo --> Query
    EventBus --> ReconcileLoop
    ReconcileLoop --> DesiredRepo
    ReconcileLoop --> ActualRepo
    ReconcileLoop --> TxManager
    TxnJournal --> Recovery
    Recovery --> TxManager
```

---

## 4. 三条关键闭环

### 4.1 配置提交闭环

```mermaid
flowchart LR
    Apply["Apply Intent"] --> Schema["Schema Validation"]
    Schema --> Normalize["Normalize"]
    Normalize --> Resolve["Resolve Endpoint Selectors"]
    Resolve --> Plan["Build Execution Plan"]
    Plan --> Admit["Capability + Resource Admission"]
    Admit --> Validate["Validate All Operations"]
    Validate --> Prepare["Prepare SW/HW Resources"]
    Prepare --> Commit["Ordered Commit"]
    Commit --> Publish["Publish Generation"]
    Publish --> Actual["Update Actual State"]
    Actual --> Result["Return Transaction Result"]
    Validate -->|"failure"| Reject["Reject Without Mutation"]
    Prepare -->|"failure"| Rollback["Rollback Prepared Objects"]
    Commit -->|"partial failure"| Rollback
```

### 4.2 数据包执行闭环

```mermaid
flowchart LR
    Packet["Ingress Packet"] --> Steering{"Hardware Rule Hit?"}
    Steering -->|"yes"| HWAction["NIC / eSwitch Actions"]
    HWAction --> HWEgress["Queue / Port / Represented Port / Drop"]
    Steering -->|"no or software-owned"| RX["RX Queue"]
    RX --> Parse["Parser + Metadata"]
    Parse --> Snapshot["Immutable Pipeline Snapshot"]
    Snapshot --> Stages["Match / Route / ACL / Stateful Stages"]
    Stages --> TX["Burst TX / Drop"]
    TX --> Counters["Per-worker Counters"]
    HWAction --> HWCounters["Flow Counters / Aging"]
```

### 4.3 状态收敛闭环

```mermaid
flowchart LR
    Trigger["Startup / Timer / Device Event / Failure"] --> Observe["Observe Runtime + Device State"]
    Observe --> Diff["Desired vs Actual Diff"]
    Diff -->|"no drift"| Healthy["Healthy"]
    Diff -->|"recoverable drift"| Repair["Build Repair Plan"]
    Diff -->|"capability changed"| Replan["Re-plan"]
    Diff -->|"cannot satisfy"| Degraded["Degraded + Explain Reason"]
    Repair --> Txn["Transaction Engine"]
    Replan --> Txn
    Txn --> Observe
    Degraded --> Backoff["Backoff / Await Event"]
    Backoff --> Trigger
```

---

## 5. 模块画布 A：Management Plane

```mermaid
flowchart TB
    subgraph Clients["Clients"]
        Dppctl["dppctl"]
        Controller["Controller"]
        Automation["Automation / CI"]
    end

    subgraph Management["Management Module"]
        Transport["Unix Socket / Future Remote Transport"]
        Auth["Identity / RBAC Boundary"]
        Commands["Apply / Delete / Retry / Rollback Commands"]
        Queries["Topology / Capability / Rule / Transaction Queries"]
        Watch["Watch Events / Status Stream"]
        Idempotency["Request ID + Idempotency"]
    end

    DomainService["Domain Services"]
    Audit["Audit Journal"]

    Clients --> Transport
    Transport --> Auth
    Auth --> Commands
    Auth --> Queries
    Auth --> Watch
    Commands --> Idempotency
    Idempotency --> DomainService
    Queries --> DomainService
    DomainService --> Watch
    Commands --> Audit
```

Management Plane 不直接创建 `rte_flow`，也不直接修改 worker snapshot。它只提交领域命令、查询状态和观察事件。

---

## 6. 模块画布 B：Intent、Model 与 State

```mermaid
flowchart TB
    Raw["Raw Intent"] --> Schema["Schema Model"]
    Schema --> EndpointSelector["Endpoint Selector"]
    Schema --> Policy["Policy / Service Intent"]
    EndpointSelector --> Resolved["Resolved Endpoints"]
    Policy --> Normalized["Normalized Policy"]
    Resolved --> RuleIR["Canonical Rule IR"]
    Normalized --> RuleIR
    RuleIR --> Desired["Desired Resource"]
    Desired --> Plan["Planned Resource"]
    Plan --> Actual["Actual Resource"]

    subgraph Identity["Identity and Versioning"]
        StableID["Stable Resource ID"]
        Revision["Intent Revision"]
        Generation["Runtime Generation"]
        Owner["Owner / Scope"]
    end

    Identity --> Desired
    Identity --> Plan
    Identity --> Actual
```

核心状态不是一个布尔值，而是三层：

- Desired：用户声明的目标。
- Planned：经过拓扑与能力决策后的执行计划。
- Actual：当前软件 snapshot 和硬件对象的真实安装状态。

---

## 7. 模块画布 C：Topology 与 Capability

```mermaid
flowchart LR
    subgraph Sources["Discovery Sources"]
        EAL["EAL Device Events"]
        EthInfo["rte_eth_dev_info"]
        FlowProbe["rte_flow Validation Probes"]
        Xstats["Link / xstats / Health"]
        VendorInfo["Optional Vendor Discovery"]
    end

    subgraph Inventory["Inventory"]
        Device["Device"]
        Port["ethdev Port"]
        Representor["Representor"]
        Entity["PF / VF / SF / Uplink Entity"]
        Switch["Switch Domain"]
        Queue["Queue / NUMA / RSS"]
    end

    subgraph CapabilityModel["Capability Profile"]
        QueueCaps["Queue + RSS Caps"]
        PacketCaps["RX/TX Offload Caps"]
        FlowCaps["Domain + Item + Action Caps"]
        ScaleCaps["Template / Async / Resource Limits"]
        LifecycleCaps["Reset / Keep / Replay Behavior"]
    end

    Sources --> Inventory
    Inventory --> CapabilityModel
    Inventory --> Graph["Endpoint Topology Graph"]
    CapabilityModel --> Snapshot["Versioned Capability Snapshot"]
    Graph --> Resolver["Endpoint Selector Resolver"]
    Snapshot --> Planner["Planner"]
    Resolver --> Planner
```

拓扑图解决“这些 port id 代表谁、彼此如何连接”；capability profile 解决“在当前 PMD、设备和固件组合上能做什么”。

---

## 8. 模块画布 D：Planner 与 Compiler Registry

```mermaid
flowchart TB
    RuleIR["Canonical Rule IR"] --> Semantic["Semantic Validation"]
    Topology["Resolved Topology"] --> Constraints["Placement Constraints"]
    Capability["Capability Snapshot"] --> Candidates["Candidate Backends"]
    Semantic --> Partition["Rule / Pipeline Partitioning"]
    Constraints --> Partition
    Candidates --> Partition
    Partition --> Admission["Resource Admission"]
    Admission --> Cost["Cost + Preference Evaluation"]
    Cost --> Selection{"Backend Selection"}
    Selection -->|"software"| SWCompile["Software Compiler"]
    Selection -->|"rte_flow"| RFCompile["rte_flow Compiler"]
    Selection -->|"vendor"| VendorCompile["Vendor Compiler"]
    SWCompile --> Plan["Execution Plan DAG"]
    RFCompile --> Plan
    VendorCompile --> Plan
    Plan --> Explanation["Decision + Fallback Explanation"]
```

Planner 的输出不是“backend 枚举”，而是一张有依赖关系的 execution plan DAG，其中包含资源需求、提交顺序、回滚动作和验证证据。

---

## 9. 模块画布 E：Transaction Engine

```mermaid
stateDiagram-v2
    [*] --> Created
    Created --> Validating
    Validating --> Rejected: validation failed
    Validating --> Preparing: validation passed
    Preparing --> RollingBack: prepare failed
    Preparing --> Committing: resources prepared
    Committing --> RollingBack: partial commit failed
    Committing --> Publishing: all operations committed
    Publishing --> Succeeded: generation visible
    RollingBack --> RolledBack: rollback complete
    RollingBack --> Degraded: rollback incomplete
    Rejected --> [*]
    Succeeded --> [*]
    RolledBack --> [*]
    Degraded --> [*]
```

```mermaid
flowchart LR
    Plan["Execution Plan DAG"] --> Validate["Validate Operations"]
    Validate --> PrepareSW["Prepare SW Snapshot"]
    Validate --> PrepareHW["Reserve / Prepare HW Objects"]
    PrepareSW --> CommitHW["Commit Hardware in Dependency Order"]
    PrepareHW --> CommitHW
    CommitHW --> PublishSW["Atomically Publish SW Generation"]
    PublishSW --> Retire["Retire Old Generation / Objects"]
    CommitHW -->|"failure"| UndoHW["Destroy Created HW Objects"]
    UndoHW --> DiscardSW["Discard Prepared SW Snapshot"]
```

DPDK flow create 并不天然提供跨多条规则的全局原子事务，因此平台事务属于“有序提交 + 补偿回滚 + 明确 degraded 状态”，不能伪装成硬件两阶段提交。

---

## 10. 模块画布 F：Reconciler

```mermaid
flowchart TB
    Events["Startup / Periodic / Device / Transaction Events"] --> Queue["Reconcile Work Queue"]
    Queue --> Scope["Coalesce by Resource Scope"]
    Scope --> LoadDesired["Load Desired State"]
    Scope --> ObserveActual["Observe Actual State"]
    LoadDesired --> Diff["Diff Engine"]
    ObserveActual --> Diff
    Diff --> Noop["No-op"]
    Diff --> Create["Create Missing"]
    Diff --> Update["Replace Changed"]
    Diff --> Delete["Delete Orphaned"]
    Diff --> Replan["Re-plan on Capability Change"]
    Create --> Txn["Transaction Engine"]
    Update --> Txn
    Delete --> Txn
    Replan --> Txn
    Txn --> Result{"Result"}
    Result -->|"success"| Status["Update Status"]
    Result -->|"retryable"| Backoff["Backoff + Retry Budget"]
    Result -->|"permanent"| Degraded["Degraded Condition"]
    Backoff --> Queue
```

Reconciler 是平台与 demo 的关键分界：设备 reset、representor 重新枚举、进程重启或规则漂移后，系统必须再次收敛，而不是要求人工重新启动全部配置。

---

## 11. 模块画布 G：Backend Framework

```mermaid
flowchart TB
    PlanOps["Backend-neutral Plan Operations"] --> Contract["Backend Contract"]

    subgraph ContractBox["Common Backend Contract"]
        Probe["Probe Capabilities"]
        Validate["Validate"]
        Prepare["Prepare"]
        Commit["Commit"]
        Abort["Abort / Rollback"]
        Query["Query Actual State"]
        Destroy["Destroy"]
        Recover["Recover / Rebind"]
    end

    Contract --> ContractBox
    ContractBox --> SW["Software Backend"]
    ContractBox --> RF["rte_flow Backend"]
    ContractBox --> VD["Future Vendor Backend"]

    SW --> SWObjects["Pipeline Snapshot / Tables / Stateful Resources"]
    RF --> RFObjects["Flow / Counter / Meter / Indirect Action / Template"]
    VD --> VendorObjects["Vendor-specific Objects"]

    SWObjects --> Actual["Actual-state Repository"]
    RFObjects --> Actual
    VendorObjects --> Actual
```

Backend 之间共享生命周期和错误模型，但不共享私有对象。`rte_flow *`、vendor handle 和 software table pointer 都不能泄漏到 canonical model。

---

## 12. 模块画布 H：Software Dataplane

```mermaid
flowchart LR
    RX["RX Burst"] --> Metadata["Parse + Packet Metadata"]
    Metadata --> Entry["Pipeline Entry"]

    subgraph Snapshot["Immutable Pipeline Generation"]
        Classify["Classifier"]
        ACL["ACL / Policy"]
        Route["Route / Next-hop"]
        Stateful["Conntrack / NAT"]
        Rewrite["Rewrite / Offload Metadata"]
        Egress["Egress Selection"]
    end

    Entry --> Classify
    Classify --> ACL
    ACL --> Route
    Route --> Stateful
    Stateful --> Rewrite
    Rewrite --> Egress
    Egress --> TX["TX Burst"]
    ACL --> Drop["Drop + Reason"]
    Route --> Drop
    Stateful --> Drop

    Control["Software Backend"] --> Build["Build New Generation"]
    Build --> Publish["Atomic Publish"]
    Publish --> Snapshot
    Snapshot --> QSBR["QSBR / Grace Period"]
    QSBR --> Retire["Retire Old Generation"]
```

软件 pipeline 是可组合 stage graph，不把 ACL、route、NAT 和 parser 混成一个巨型 worker 函数。worker 只持有当前 generation 的只读引用。

---

## 13. 模块画布 I：Runtime 与 Device Manager

```mermaid
flowchart TB
    subgraph Lifecycle["Process Lifecycle"]
        Boot["EAL Boot"]
        Discover["Device Discovery"]
        Allocate["NUMA Memory Allocation"]
        Configure["Port / Queue Configuration"]
        Launch["Worker Launch"]
        Stop["Ordered Stop"]
    end

    subgraph DeviceRuntime["Device Runtime"]
        PortRegistry["Port Registry"]
        QueueMap["Queue ↔ Worker Map"]
        PoolMap["Socket ↔ Mempool Map"]
        LinkState["Link / Hotplug State"]
        RepresentorMap["Representor Runtime Map"]
    end

    subgraph WorkerRuntime["Worker Runtime"]
        Lcores["Worker Lcores"]
        SnapRef["Snapshot Reference"]
        LocalStats["Per-worker Stats"]
        StopToken["Stop / Quiescent State"]
    end

    Boot --> Discover --> Allocate --> Configure --> Launch
    Discover --> PortRegistry
    Configure --> QueueMap
    Allocate --> PoolMap
    PortRegistry --> LinkState
    PortRegistry --> RepresentorMap
    QueueMap --> Lcores
    Launch --> Lcores
    Lcores --> SnapRef
    Lcores --> LocalStats
    Lcores --> StopToken
    StopToken --> Stop
```

Runtime Manager 管资源与生命周期；Topology Manager 管语义关系；Backend 管规则对象。三者不能合并成一个全局 port context。

---

## 14. 模块画布 J：Observability 与 Operations

```mermaid
flowchart LR
    subgraph Producers["Signal Producers"]
        API["API Requests"]
        Planner["Planner Decisions"]
        Txn["Transactions"]
        Reconcile["Reconcile Results"]
        Workers["Worker Counters"]
        Flows["Flow Counters / Aging"]
        Devices["Link / xstats / Health"]
    end

    subgraph Observability["Observability Module"]
        Metrics["Metrics Aggregator"]
        Events["Structured Event Stream"]
        Logs["Structured Logs"]
        Audit["Immutable Audit Records"]
        Diagnostics["pdump / Trace / Snapshot Dump"]
        Conditions["Health Conditions"]
    end

    subgraph Consumers["Consumers"]
        Telemetry["DPDK Telemetry"]
        Prometheus["Future Metrics Exporter"]
        Dppctl["dppctl inspect / watch"]
        Alerting["Alerting"]
        Reconciler["Reconciler"]
    end

    Producers --> Observability
    Metrics --> Telemetry
    Metrics --> Prometheus
    Events --> Dppctl
    Logs --> Dppctl
    Conditions --> Alerting
    Conditions --> Reconciler
    Audit --> Dppctl
    Diagnostics --> Dppctl
```

平台至少要回答：用户要求了什么、planner 选择了什么、为什么降级、创建了哪些实际对象、哪一步失败、是否回滚完成、当前是否发生漂移。

---

## 15. 进程与线程画布

```mermaid
flowchart TB
    subgraph Process["dppd Process"]
        Main["Main Lcore / Lifecycle"]
        APIThread["Management Thread"]
        ControlThread["Planner + Transaction Thread"]
        ReconcileThread["Reconciler Thread"]
        EventThread["Device Event / Backend Completion"]
        TelemetryThread["Telemetry / Metrics"]

        subgraph Workers["Pinned Worker Lcores"]
            W0["Queue Worker 0"]
            W1["Queue Worker 1"]
            WN["Queue Worker N"]
        end
    end

    APIThread --> ControlThread
    ReconcileThread --> ControlThread
    EventThread --> ReconcileThread
    ControlThread --> Publish["Publish Snapshot / Commit HW Objects"]
    Publish --> Workers
    Workers --> TelemetryThread
    EventThread --> TelemetryThread
    Main --> APIThread
    Main --> ControlThread
    Main --> ReconcileThread
    Main --> EventThread
    Main --> Workers
```

线程数量不是最终决定；这里先固定 ownership：慢路径状态只能由控制域修改，worker 不执行策略变更，管理请求也不能直接触碰 fast-path 对象。

---

## 16. Host / SmartNIC / DPU 部署画布

```mermaid
flowchart LR
    subgraph Host["Host"]
        HostController["Controller / dppctl"]
        HostAgent["Optional Host Agent"]
        HostPF["Host PF / VF"]
    end

    subgraph DPU["DPU / SmartNIC"]
        Dppd["dppd Control Plane"]
        SWWorkers["Arm-side SW Workers"]
        FlowBackend["rte_flow / Vendor Backend"]
        Reps["Host PF/VF/SF Representors"]
        Uplink["Uplink Representor"]
        Eswitch["Embedded Switch"]
    end

    Network["External Network"]

    HostController --> Dppd
    HostAgent --> Dppd
    HostPF --> Reps
    Dppd --> FlowBackend
    Dppd --> SWWorkers
    FlowBackend --> Eswitch
    Reps --> Eswitch
    Uplink --> Eswitch
    Eswitch --> Network
    SWWorkers --> Uplink
```

后续需要明确每种部署的唯一 ownership：谁创建 representor、谁拥有 flow namespace、谁负责持久化 desired state、DPU 重启后由谁触发 replay。

---

## 17. 一级资源状态模型

```mermaid
stateDiagram-v2
    [*] --> Pending
    Pending --> Planning
    Planning --> Unsupported
    Planning --> Planned
    Planned --> Applying
    Applying --> Active
    Applying --> Failed
    Active --> Updating
    Updating --> Active
    Updating --> Degraded
    Active --> Deleting
    Degraded --> Reconciling
    Reconciling --> Active
    Reconciling --> Degraded
    Failed --> Reconciling
    Deleting --> Deleted
    Unsupported --> [*]
    Deleted --> [*]
```

每个资源状态必须带 reason、observed generation、backend、最近 transaction id 和可重试性，而不是只有 success/failure。

---

## 18. 设计不变量

1. Canonical model 不包含 `rte_flow *`、mbuf、PMD 私有句柄。
2. 管理 API 不直接操作端口、worker 或 flow object。
3. Planner 只生成计划，不产生外部副作用。
4. 所有副作用通过 transaction engine 和 backend contract 发生。
5. Actual state 来自可观察对象，不能仅根据“曾经调用成功”推断。
6. 软件 snapshot 发布后不可原地修改。
7. representor 是拓扑端点；transfer 是硬件 domain，不是逐包软件模式。
8. `PREFER_HARDWARE` 必须记录选择和降级证据；语义不等价时不得 fallback。
9. 回滚不完整必须进入 degraded，而不是返回普通 success。
10. 设备与能力变化必须触发 reconciliation。

---

## 19. 下一轮细分入口

本文停留在总体与一级模块。下一轮可以按以下顺序继续拆分：

1. 领域对象：Endpoint、Intent、Rule IR、Plan、Actual Resource、Condition。
2. Backend contract：操作集合、错误分类、对象 ownership、幂等规则。
3. Transaction：plan DAG、提交顺序、补偿动作、journal 与恢复。
4. Topology：PF/VF/SF/uplink/representor graph 和 selector 语义。
5. Software pipeline：stage contract、snapshot layout、QSBR 更新协议。
6. Management API：命令、查询、watch、request id 和版本冲突。
7. 进程模型：线程、消息队列、锁边界和 shutdown 时序。
8. 持久化：desired state、journal、actual cache 和启动恢复。

在这些细分得到确认前，不进入代码重构。
