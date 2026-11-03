# mbuf 内存池与收发循环

[打开循环图](mbuf-cycle.html) · [返回学习说明](../../README.md)

先看三行流程，所有关键路径直接显示，不必点击才能看到：

1. 初始化：创建 mbuf 池，把池指针传给 RX 队列配置
2. 运行：PMD 从池取缓冲并接收报文，RX burst 把 mbuf 指针交给 worker
3. 归还：丢弃和 TX 未接收部分由 worker 释放；TX 已接收部分在完成后由 PMD 回收

“回到原内存池”指回到 mbuf 所属池，转发出口网卡不必与入口网卡在同一 NUMA 节点。归还也可能先进入 lcore 缓存；图中省略缓存层，不表示每次都立即进入全局空闲队列。

例如发送三个包，TX 返回 `sent = 2`：前两个已交给驱动，worker 只释放第三个。前两个在对应发送完成后由 PMD 的回收路径归还，不能由 worker 再次释放。

此图使用典型物理 NIC、普通单段报文说明所有权流转，省略引用共享、分段、ND 等待和停机清理。虚拟 PMD 可能转交给软件消费者，不保证存在物理 DMA 或硬件发送完成。

## 对应代码

| 步骤 | 位置 |
| --- | --- |
| 创建池 | [device.c](../../../../lib/device/device.c)，`create_socket_pools()` |
| RX 队列使用池 | [device.c](../../../../lib/device/device.c)，`configure_port_queues()` |
| 收到指针并处理 | [worker.c](../../../../lib/dataplane/worker.c)，`process_ingress()`、`process_batch()` |
| 发送及释放尾部 | [worker.c](../../../../lib/dataplane/worker.c)，`flush_egress()`、`free_packets()` |
| PMD 接收与完成回收原理 | [队列与 burst 收发](../../../dpdk/chapters/04-queues-io.md) |

图中源码引用固定在提交 `38ed792f5514063041e023f3e080dbab7b31b123`，不是当前工作区行号。生成时 `lib/device/device.c` 有未提交的学习注释修改，因此图内取已提交代码作为证据；本说明的相对源码链接用于打开当前文件。

DPDK 的通用 RX/TX 契约见 [ethdev API](https://doc.dpdk.org/api-24.11/rte__ethdev_8h.html)，具体设备的完成与回收细节由其 PMD 实现。

`candidate.json` 是可维护的图定义，HTML 是可在浏览器独立打开的产物；相邻的 finalize、browser-check 和 visual-check 文件记录图表及浏览器检查，不代表设备流量验收。
