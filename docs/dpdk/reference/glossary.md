# 容易混淆的 DPDK 术语

配合 [项目调用地图](../project-map.md) 阅读。英文名用于检索官方文档，中文解释用于复述机制。

| 名称 | 在这里怎样理解 | 容易混淆的对象 |
| --- | --- | --- |
| EAL | 应用与执行环境之间的资源初始化和适配层 | 不是完整 TCP/IP 协议栈 |
| lcore | DPDK 标识执行上下文的逻辑编号，可映射到 CPU | 不保证等于操作系统 CPU 编号或物理核编号 |
| main lcore | 运行主控制流程的执行上下文 | 本项目 worker 候选不包含它 |
| PMD | 面向某类设备的轮询收发与控制实现 | 可以面对真实网卡，也可以面对软件虚拟设备 |
| ethdev | DPDK 对以太网端口及队列的统一接口 | ethdev 编号不等于 Linux ifindex 或 PCI VF 编号 |
| RX/TX descriptor | 设备与驱动交换缓冲区地址、长度、状态等信息的记录 | 不是 mbuf，也不等于通用软件 ring |
| rte_ring | 保存对象指针或元素的软件队列 | 本项目测试用它，不说明物理硬件也用这个库 |
| net_ring | 把软件 ring 暴露为 ethdev 的 PMD | 没有真实网卡 DMA，也不能证明 rte_flow 硬件能力 |
| DMA | 设备与内存之间的数据传输机制 | CPU 把指针传给函数本身不是一次 DMA |
| VA / IOVA | CPU 虚拟地址 / 设备 DMA 使用的地址 | 即使数值相同，含义和地址翻译路径仍不同 |
| IOMMU | 对设备 DMA 地址做映射和访问约束 | 不等于 CPU MMU，也不是 DPDK 自己实现的网卡 |
| hugepage | 比普通页大的内存页 | 不能保证每段虚拟内存都物理连续，也不等于 mempool |
| mempool | 固定大小对象的分配与复用机制 | 与线程私有 cache、已借出对象共同解释可用容量 |
| mbuf | 管理报文位置、长度、引用、分段和元数据的对象 | mbuf 结构体地址不等于报文头地址 |
| headroom / tailroom | 数据区中有效报文之前/之后的可用空间 | 不是数据包中的协议字段 |
| clone / copy | 共享原数据的新管理对象 / 独立数据副本 | clone 不授予独立修改共享数据的权利 |
| burst | 一次批量请求或处理 | RX 指定最大数量，不意味着等满才返回 |
| RSS | 按报文散列结果选择接收队列的机制 | 不是按队列当前负载做动态调度 |
| NUMA | CPU 访问不同节点内存的代价可能不同 | 同 socket 布局也不能替代实际性能测量 |
| QSBR | 读者报告不再借用旧对象的安全点，帮助推迟回收 | 不负责自动构造快照，也不等于线程睡眠 |
| pattern / action | 流规则的匹配条件 / 命中后操作 | validate 成功不代表资源已分配或包已命中 |
| representor | 代表某个交换端点的控制/收发接口 | 不能把 representor 的 RX 当作对应 VF 全部流量 |
| transfer | 规则作用于设备内部交换域的属性 | 不等于在 CPU worker 中执行软件转发 |
| AGE / COUNT | 闲置老化条件 / 命中计数动作 | AGE 支持与有效计数需要独立确认 |
| indirect action | 可由多个规则引用的动作对象 | 共享 COUNT 查询返回对象汇总，不是单规则私有计数 |
| flow queue | 原生异步 flow 操作队列 | 与 RX/TX 报文队列、平台请求队列是不同对象 |

进一步查证：[EAL](https://doc.dpdk.org/guides-24.11/prog_guide/env_abstraction_layer.html)、
[PMD](https://doc.dpdk.org/guides-24.11/prog_guide/ethdev/ethdev.html)、
[mbuf](https://doc.dpdk.org/guides-24.11/prog_guide/mbuf_lib.html)、
[QSBR](https://doc.dpdk.org/guides-24.11/prog_guide/rcu_lib.html)。
