# API 查询索引

这张表用于学完机制后快速定位。源码入口统一见 [项目调用地图](../project-map.md)。
API 的支持范围还取决于 DPDK 版本和 PMD，不能只根据函数声明判断设备能力。

| API 或 API 组 | 作用 | 调用后仍要检查什么 |
| --- | --- | --- |
| `rte_eal_init` / `rte_eal_cleanup` | 初始化/释放 EAL 资源 | 参数消费数量、返回码、业务对象是否已先释放 |
| `rte_eth_dev_info_get` | 查询设备能力 | 不代表功能已启用，实际配置和创建仍可能失败 |
| `rte_eth_dev_configure` | 配置端口队列数和模式 | 队列是否逐一设置，是否真正启动 |
| `rte_eth_dev_adjust_nb_rx_tx_desc` | 根据 PMD 约束调整描述符数 | 返回后的实际值可能不同于请求值 |
| `rte_eth_rx_queue_setup` | 配置 RX 队列及报文池 | pool 的生命周期必须覆盖使用它的队列 |
| `rte_eth_tx_queue_setup` | 配置 TX 队列 | 与 RX 队列、应用软件 ring 分别管理 |
| `rte_eth_dev_start/stop/close` | 启动、停止、关闭端口 | worker 是否还可能调用收发接口 |
| `rte_eth_rx_burst` | 获取最多指定数量的报文 | 返回 0 不单独证明链路故障；返回包归调用方处理 |
| `rte_eth_tx_burst` | 尝试把前缀报文交给 PMD | 未接收尾部仍归应用；已接收部分不能再访问 |
| `rte_pktmbuf_pool_create` | 创建报文对象池 | socket、容量、cache、缓冲区大小及分配失败 |
| `rte_pktmbuf_alloc/free` | 获取/释放一份 mbuf 持有关系 | 引用未归零时释放不等于实际数据回收 |
| `rte_pktmbuf_read` | 获取一段逻辑连续数据 | 返回原地址或 scratch，应使用返回值 |
| `rte_pktmbuf_clone/copy` | 共享数据/复制数据 | 内存失败与可写性不是同一个问题 |
| `rte_pktmbuf_linearize` | 尝试将分段数据合并到首段 | 空间可能不足，不能忽略失败 |
| `rte_eal_remote_launch/wait_lcore` | 启动/等待 worker | 部分启动失败也要处理已经启动的线程 |
| `rte_rcu_qsbr_thread_register/online/offline` | 登记和启停读者参与 | 不得让仍借用旧表的读者提前离线 |
| `rte_rcu_qsbr_start/check/quiescent` | 发起回收周期、检查和报告安全点 | 先取消旧对象的可达性，再判断何时回收 |
| `rte_flow_validate/create/destroy/query` | 验证、安装、销毁、查询规则 | validate 不预留资源，失败及未知状态要分别处理 |
| `rte_flow_get_aged_flows` | 查询已老化规则的关联上下文 | 只能删除匹配当前身份的规则 |
| `rte_flow_action_handle_*` | indirect 动作生命周期 | 被引用对象不能提前销毁 |
| `rte_flow_*_template_create` | 创建匹配/动作模板 | 模板、表和规则之间有依赖 |
| `rte_flow_async_create/destroy` | 提交原生异步操作 | 受理不等于完成，保留参数和关联标识 |
| `rte_flow_push/pull` | 推送待处理操作、收集完成项 | queue、user_data 和完成状态要对应 |
| `rte_telemetry_register_cmd` | 注册可观测查询 | 返回数据是何时采样、什么范围的计数 |
| `rte_pdump_init` | 初始化抓包服务支持 | 初始化不等于已抓包，也不代表全部卸载流可见 |

官方接口：[ethdev](https://doc.dpdk.org/api-24.11/rte__ethdev_8h.html)、
[mbuf](https://doc.dpdk.org/api-24.11/rte__mbuf_8h.html)、
[rte_flow](https://doc.dpdk.org/api-24.11/rte__flow_8h.html)、
[QSBR](https://doc.dpdk.org/api-24.11/rte__rcu__qsbr_8h.html)。
