# M5 持久请求与幂等重试

管理协议 v30 增加 `request-submit` 和 `request-get`，用于受管理软件模式的规则增删改及在线定义事务
旧 `flow-create/update/remove` 的进程内 token 接口保留原语义，新入口使用客户端长期保存的 `(client, sequence)`

```sh
dppctl --socket /run/dppd/control request-submit 81 1 flow-create 1 0 apply-drop 10 0 0
dppctl --socket /run/dppd/control request-get 81 1
```

序号必须非零，每个新请求使用比该客户端已接受水位更大的序号，响应丢失时原样重发同一个请求
同键同规范内容返回原结果，同键不同内容返回 `EEXIST`，不会重新执行
可以包装 `flow-update`、`flow-remove` 和 `flow-config-apply`，配置文件内容参与比较而不是文件路径

## 持久性与失败

- 受理记录先可靠保存，再进入异步执行队列，`phase=pending durable=yes` 只说明受理已保存
- 策略结果和 `phase=done` 回执放在同一个完整配置文件，原子替换并同步父目录后才确认完成
- 成功必须同时检查响应成功、`phase=done status=0 applied=yes durable=yes`
- 回执中的 `revision` 是该任务完成时的准确账本版本，`generations` 是每个对象的结果版本
- 失败也有持久终态，同键不会因再次提交而自动重做，修正参数后应使用新的序号
- 受理保存失败不会入队，实例封闭写入和转发，显式修复保存后原键仍返回失败
- 已应用但落盘失败时保留完整内存结果，停止 worker，`persistence-flush` 成功后才开放成功回执
- 清理残留或不确定实际状态仍返回 `EUCLEAN`，不能仅凭 `durable=yes` 忽略错误响应
- 重启遇到文件中的 pending 请求会记录 `ECANCELED`，不会自动重新执行；若策略已提交，文件内必须已有对应 done 回执
- 查询不消费回执，旧随机会话 token 不用于持久查询

配置文件按需升级为 DPPM4CFG v2，回执与策略没有分离的提交窗口
不含持久请求历史的配置仍写 v1，新程序同时读取 v1/v2，旧程序必须拒绝 v2
升级和回滚不能删掉请求历史或用旧配置覆盖新策略
COUNT 累计和连接跟踪会话仍不属于本文件的持久内容

## 有界容量

最多保留 32 个客户端身份和 128 条请求回执
客户端身份跨进程长期复用，不能每次连接生成一个新身份；满 32 个身份后新身份返回 `ENOSPC`
回执轮转只覆盖已经完成的记录，不覆盖 pending；客户端最高序号永久保留
已回收或被较高序号跨过的旧键返回 `ESTALE`，不会作为新请求执行
最后一个序号用尽时该身份不能再发起新写请求
本轮没有管理员清空防重放水位的接口，也不承诺无限历史保存

原生硬件模式的持久请求接口明确拒绝，不能从配置文件推断旧硬件对象是否已经清空
本地 C ABI 仍要求 daemon/CLI 配套升级，稳定北向协议作为后续独立交付

## 验收

`test_flow_request` 覆盖并发重复、冲突、精确代次、重复查询、重启、受理与完成写盘失败、回执淘汰和旧键保护
`test_flow_management` 验证正式分发及停机取消持久结果，CLI 单测验证新命令和禁止嵌套请求
`tests/integration/m5_requests.py` 通过正式 daemon/CLI 验证并发重试、实际丢失响应、SIGKILL、配置与共享对象恢复、淘汰及故障修复
`--tap` 入口要求私有网络命名空间，并核对实际收发、策略丢弃和共享计数

本项仍是 M5 的一个独立交付，整体完成状态由 `m5_production_plan.md` 的联合验收决定

2026-10-22，DPDK 21.11.9 与 24.11.8 的优化、ASan/UBSan 四套构建各 89/89 单测通过
各套既有 M2 核心、M3、M4 和 M5 进程恢复回归，以及五阶段正式请求闭环通过
四套分别通过私有命名空间双 TAP 的 56 包验收：24 包转发、32 包按规则丢弃
崩溃测试区分文件同步失败和 rename 后父目录同步失败，并覆盖仅回执脏状态与准确取消版本
324 项源码摘要一致：`98813ea9461d3db07daf2c71a5710728173071228720c870c49a152148d4923c`
原始日志、二进制摘要及提交核对保存在 `build/native-check/m5-requests-artifacts/summary.json` 和同目录证据包
本轮未重复完整十二步 M2 TAP 或八项冻结性能测量，二者列入 M5 最终同源联合验收
ASan/UBSan 对 DPDK 线程关闭 leak 检查，不把结果称为通用 LSAN 或硬件验收
