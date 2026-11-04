# mbuf 从内存池到收发，再回到所属池

[程序启动、运行与停止总图](startup-stop.md) · [学习材料说明](../README.md)

把启动图中的“准备池和队列”与运行图中的“收包、处理、发送”连接起来。下面采用典型物理网卡、普通单段报文说明缓冲怎样复用，以及每一步由谁管理 mbuf

```mermaid
flowchart LR
    subgraph Setup["① 初始化 — 池和 RX 队列怎样连接"]
        direction TB
        POOL["<b>创建 mbuf 池</b><br/>lib/device/device.c<br/>create_socket_pools()<br/>rte_pktmbuf_pool_create()"]
        RX_SETUP["<b>把池交给 RX 队列</b><br/>configure_port_queues()<br/>rte_eth_rx_queue_setup(..., pool)<br/>PMD 据此取得接收缓冲"]
        POOL --> RX_SETUP
    end

    subgraph Receive["② 收包处理 — 从驱动交给 worker"]
        direction TB
        REFILL["<b>PMD 准备接收缓冲</b><br/>从关联池中取得 mbuf<br/>配置 RX 描述符的数据区地址"]
        NIC["<b>物理网卡接收报文</b><br/>将报文字节写入数据缓冲<br/>更新接收完成状态"]
        RX_BURST["<b>worker 取得报文指针</b><br/>lib/dataplane/worker.c<br/>process_ingress()<br/>rte_eth_rx_burst() 返回实际数量<br/>指针填入 packets 数组"]
        PROCESS["<b>逐包解析和决策</b><br/>process_batch()<br/>当前 mbuf 由 worker 持有"]
        REFILL --> NIC --> RX_BURST --> PROCESS
    end

    subgraph Return["③ 归还缓冲 — 谁持有，谁负责后续处理"]
        direction TB
        ACTION{"<b>当前报文怎么处理？</b>"}
        TX["<b>提交待转发报文</b><br/>flush_egress()<br/>rte_eth_tx_burst()<br/>返回驱动实际接收数量 sent"]
        WORKER_FREE["<b>worker 释放</b><br/>rte_pktmbuf_free()<br/>包括 DROP 和 TX 未接收的包"]
        DRIVER_OWNS["<b>已移交驱动</b><br/>PMD 管理已接收的 mbuf<br/>worker 不再访问或释放"]
        TX_DONE["<b>发送完成后的回收</b><br/>由 PMD 的回收路径处理<br/>回收时机取决于具体驱动"]
        REUSE["<b>回到 mbuf 所属池</b><br/>可能先进入 lcore 缓存<br/>后续再次进入②补充 RX 缓冲"]

        ACTION -->|DROP| WORKER_FREE
        ACTION -->|转发| TX
        TX -->|后 count - sent 个：未接收| WORKER_FREE
        TX -->|前 sent 个：已接收| DRIVER_OWNS
        DRIVER_OWNS --> TX_DONE
        WORKER_FREE --> REUSE
        TX_DONE --> REUSE
    end

    Setup -->|配置 RX 使用的池| Receive
    Receive -->|worker 根据处理结果分流| Return

    style Setup fill:#f6f9fd,stroke:#829ab7
    style Receive fill:#f4f9f5,stroke:#7f9f88
    style Return fill:#fdf8f1,stroke:#b7a081
```

归还之后，PMD 可以从关联池中再次取得可用对象，回到图②的“准备接收缓冲”，形成持续复用的循环；池和队列的初始化不需要每个包重新做

## 发送三个包，驱动只接收两个

`flush_egress()` 的待发送数组中有 3 个包，TX burst 返回 `sent = 2`：

| 数组中的包 | 调用后由谁管理 | 后续如何处理 |
| --- | --- | --- |
| `packets[0]` | PMD | 完成发送后，由驱动回收路径归还 |
| `packets[1]` | PMD | 完成发送后，由驱动回收路径归还 |
| `packets[2]` | worker | 本项目不重试，立即释放这个未接收的包 |

这里的 `packets` 是当前出口的待发送数组，先前已 DROP 的包不在里面。`sent` 只表示驱动接收了多少个包，不能据此认定对端已经收到

## 归还时容易混淆的两点

1. **回到 mbuf 所属池**：归还目标由 mbuf 自身所属的池决定，不按发送出口另选一个池；入口与出口可以属于不同 NUMA 节点
2. **先归还到本地缓存也属于复用过程**：图中省略了 lcore 缓存层，不表示每次释放都立刻进入全局空闲队列

PMD 要提前准备接收缓冲，并在收包过程中补充可用缓冲。worker 长时间持有 mbuf，或者池中的可用对象不足，都可能影响后续 RX 缓冲补充

## 对应代码

| 图中的位置 | 文件与函数 | 重点看什么 |
| --- | --- | --- |
| 创建池 | [lib/device/device.c:268](../../../lib/device/device.c#L268) · `create_socket_pools()` | 按端口使用的 NUMA 节点创建池，保存到 `devices->pools[]` |
| RX 关联池 | [lib/device/device.c:512](../../../lib/device/device.c#L512) · `configure_port_queues()` | 将 `pool` 传入 RX queue setup；TX queue setup 不需要这个池参数 |
| 收到 mbuf 指针 | [lib/dataplane/worker.c:512](../../../lib/dataplane/worker.c#L512) · `process_ingress()` | RX burst 填写指针数组，返回实际收包数 |
| 逐包处理 | [lib/dataplane/worker.c:272](../../../lib/dataplane/worker.c#L272) · `process_batch()` | 根据结果释放包，或放入出口的待发送数组 |
| TX 交接与短发送 | [lib/dataplane/worker.c:37](../../../lib/dataplane/worker.c#L37) · `flush_egress()` | 用 `sent` 区分驱动已接收部分和 worker 仍持有的尾部 |
| 释放仍由 worker 持有的包 | [lib/dataplane/worker.c:13](../../../lib/dataplane/worker.c#L13) · `free_packets()` | 逐个调用 `rte_pktmbuf_free()` |
| PMD 原理对照 | [队列描述符与 burst 收发](../../dpdk/chapters/04-queues-io.md) | RX 缓冲补充、ethdev 到 PMD 的调用，以及发送完成和回收 |

图中省略引用共享、分段、ND 等待和停机清理。虚拟 PMD 可能将对象转交给软件消费者，其路径不保证存在物理 DMA 或硬件发送完成；停止阶段的资源顺序见[程序启动、运行与停止总图](startup-stop.md)
