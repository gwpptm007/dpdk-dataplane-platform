# M5：在线模板与共享 COUNT 配置事务

本项在 `dppd --flow-state` 中增加在线定义事务，管理协议升级到 **v29**。
模板与共享 COUNT 可整批新增或删除，支持查询、引用保护、持久化、失败重试及重启恢复。
软件 daemon 使用真实原子共享计数；原生 C API 使用既有 DPDK 工厂，测试不代替物理 ASIC 验收。

## 使用

新初始化的软件配置预留 `3 * rule_capacity + 128` 个共享借用位置。
旧文件仍可读取，队列、配额和借用池容量不会被在线事务暗中放大；此前仅预留一个共享借用的
软件配置不能承载共享规则，应先停止实例，再按实际规模离线调整完整配置。
每条共享规则需要准备 pin、实际 flow 和 classifier 快照三类借用，旧快照退出前继续占有位置。

```sh
build-perf/dppd-flow-state init-software /absolute/path/m4.state 0 64 2 64
# 按正式 daemon 文档启动 --flow-state 后，先读取当前修订号
build-perf/dppctl --socket /absolute/path/dppd.sock flow-config-get
```

配置文件为不含 BOM 的 UTF-8 文本，每行一个操作，最多八个操作，空行和 `#` 注释可忽略。
本例一次建立共享计数和模板：

```text
count-add 800 0 ingress
template-add 2 80 1 apply-drop 1 0 0 shared:800:0
```

```sh
# REV 是 flow-config-get 返回的精确 revision，不能填 any
build-perf/dppctl --socket /absolute/path/dppd.sock flow-config-apply REV /absolute/path/transaction.txt
# configuration applied=yes durable=yes revision=NEW_REV durable-revision=NEW_REV
# operation=0 allocated-generation=G

# 模板键 2、容量 80、dynamic 位掩码 1，示例规则 ID 会归零
# 最后的 shared 参数指向新对象的实际代次，规则版本零表示新建
build-perf/dppctl --socket /absolute/path/dppd.sock \
  flow-create 2 0 apply-drop 50 0 0 shared:800:G
# 规则仍按原来的异步凭据调用 flow-collect，只有完成并落盘才成功
build-perf/dppctl --socket /absolute/path/dppd.sock shared-count-query 800 G
```

`template-add KEY CAPACITY DYNAMIC RULE...` 复用软件规则构造器：`apply-drop`、`apply-filter`、
`apply-filter-drop`、`acl-set`、`acl6-set`。`DYNAMIC` 是十进制动作位掩码，不能超出动作数量。
`shared:ID:GEN` 只能出现在最后；有私有 COUNT 时将其替换为共享 COUNT，否则插入到动作开头。
同批新建的共享引用可以填零代次，由服务端解析为本批分配的实际 generation；普通规则必须填精确代次。
模板检查仍约束动作形状、匹配掩码、安装端口和容量，不把不兼容规则悄悄改成另一种语义。

其余操作：

```text
template-delete 2
count-delete 800 G
```

同批操作顺序不决定引用删除结果：最终配置仍有模板示例或规则引用该共享对象就返回 `EBUSY`。
有规则使用的模板不能删除。规则任务尚未全部完成并领取时，定义事务返回 `EBUSY`。
同一个事务中重复操作同类 ID/模板键会拒绝，不允许“删掉再用同一个身份偷偷替换”。

## 在线替换与查询

替换采用显式迁移：先用新键/ID 建立定义，再以精确规则版本 `flow-update` 整批改绑新模板/共享对象，
领取成功回执后删除旧定义。现有规则版本、未修改规则的私有 COUNT，以及旧共享对象累计均保持。
新共享对象从零开始累计；已有共享对象在规则替换、同对象改绑或修改其他配置时不清零。

`flow-config-get [REV [AFTER_TABLE AFTER_COUNT]]` 独立分页两张定义表，每页两个模板、四个共享定义。
响应带同一个配置修订号、下一次共享代次水位、可靠保存的修订号以及 dirty/残留标志。
翻页期间任何规则或定义提交都会推进全局修订号，旧页续读返回 `ESTALE`。
共享对象可用 `shared-count-get/query` 查询精确身份和实际累计；低层引用还包括准备项及退役快照。

## 事务与失败处理

配置、规则使用同一个全局 revision。两个并发生产者携带同一个旧版本时只允许一个提交。
组件在实例锁内执行以下步骤：

1. 构造完整候选配置，检查重复身份、引用、模板匹配、最终资源额度和精确版本
2. 新共享代次先写入可靠的水位记录，失败或崩溃后不复用已经分配的身份
3. 准备全部新共享对象和模板，旧定义及现有 classifier 保持有效
4. 推进全局修订号并采用完整新配置，使用同目录临时文件、文件 fsync、rename 和目录 fsync 保存
5. 保存成功后回收不再引用的旧模板及共享对象，全部完成才返回成功

准备失败先清理本批新对象，保留旧定义；代次水位可能已经前移，这是防止旧身份复活的保留记录。
硬件对象和模板需要临时容量，最终配置能容纳不表示准备时也一定有空间，不降低既有配额要求。

采用新配置后写盘失败返回 `EUCLEAN`，输出 `applied=yes durable=no`，保留内存配置和旧实际对象，
进入 dirty 隔离并停止 worker。不能把失败解释成完全没有执行，也不能盲目重发相同事务。
修复存储后使用 `persistence-flush` 保存当前完整配置，再核对定义及版本。
仅代次水位保存失败时返回 `applied=no`，也需要显式 flush 修复 dirty。

保存已经成功但旧对象回收失败时，响应可能是 `applied=yes durable=yes` 且错误码仍为 `EUCLEAN`。
这说明文件可靠保存，但事务实际清理尚未完成，不是成功回执；实例保留精确句柄、配额和引用并隔离。
`flow-retry` 只重试这些已知对象，失败时不清空凭据；未知状态不能靠普通重试抹去。
管理轮询发现隔离后立即让主循环停止读者，再处理后续恢复请求。

软件共享累计通过快照借用保护，旧 worker 尚未越过 QSBR 静默点时不能释放计数指针。
已安全退役的快照由显式回收释放借用；仍有旧读者时保留资源，等待停止/静默后重试。
正常停机先注销 worker，再关闭 classifier，最后回收共享库，不把实际对象回收写成策略删除。

重启保留定义、规则版本、共享代次和删除产生的空洞，COUNT 重新归零。
原生恢复仍必须提供独立端点身份和旧对象清空证明，配置事务不能代替崩溃前安装意图的硬件核对。

## 验收与边界

入口为 `python3 scripts/verify_m5.py --build-dir BUILD`，包括原有完整回归及新增配置事务单测。
`tests/integration/m5_daemon.py --tap` 在独立网络命名空间运行正式 daemon/CLI，固定 96 包中
32 包逐字节转发、64 包规则丢弃，覆盖新模板安装、共享累计、引用拒绝、删除、SIGKILL、代次重建和存储故障恢复。
原生测试包装公共 PMD 入口，覆盖正式工厂的创建失败、模板清理失败、共享清理失败与精确重试。

仍仅支持共享 COUNT，不增加 RSS/METER 等共享动作。不在线调整队列、配额、原生计数/AGE 池或 worker 布局。
同身份原地替换、有引用定义删除、跨配置与规则的一笔混合事务、持久事务回执、北向 API、鉴权和滚动升级尚不支持。
本项不登记为完整 M5 或实机硬件验收完成。

2026-10-18 同源验收记录：

| 构建 | 单测 | 原 M4 worker | 原 M5 两进程 worker | 正式 daemon 双 TAP |
| --- | --- | --- | --- | --- |
| 系统 DPDK 21.11.9 优化 | 88/88 | 272 包 | 96 包 | 96 包，32 转发、64 丢弃 |
| 私有 DPDK 24.11.8 优化 | 88/88 | 272 包 | 96 包 | 96 包，32 转发、64 丢弃 |
| 21.11.9 ASan/UBSan | 88/88 | 272 包 | 96 包 | 96 包，32 转发、64 丢弃 |
| 私有 24.11.8 ASan/UBSan | 88/88 | 272 包 | 96 包 | 96 包，32 转发、64 丢弃 |

完整源码目录共 319 项，摘要为 `92621acd1014ceb4ef7f901e0246286237195c480d0275d66c54f04a0af03da8`。
四套功能入口均要求严格构建、断言和所有测试通过，原有核心四步、M3 六步、共享恢复五组、
设备事件四组、模拟 31 组及 76 项验收工具测试继续执行。ASan/UBSan 为 DPDK 线程关闭 leak 检查，
不把此结果写成通用 LSAN 或实际硬件验收。

额外的本地 TAP 兼容构建通过 88/88 单测和原有十二步完整回归；实际加载的 DPDK 动态库均核对，
私有 24.11 构建未混用系统 21.11 库。四组性能各保留完整五轮，参考 CPU 0–3 与 M4 revision 1 契约不变，
八项冻结门禁全部通过，没有降低阈值或排除失败样本。
16384 条内存驱动安装为 1091601.517 条/秒、API P99 为 4.057 微秒；正式软件安装为
5065.266 条/秒、API P99 为 538.826 微秒；4096 条队列完成 P99 为 4.870059 毫秒，
原期望快照重放为 59.697949 毫秒。这些是既有 M4 回归指标，不是新定义事务落盘吞吐或硬件性能。

原始报告、源码映射、二进制/动态库检查及最终提交推送记录保存于
`build/native-check/m5-config-artifacts/summary.json` 及同目录证据包。
