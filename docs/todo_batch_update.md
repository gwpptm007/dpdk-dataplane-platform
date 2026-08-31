# 待办：跨规则原子更新

状态：设计已完成，待接入 control/management；不影响当前已实现的单规则更新、批量创建和批量精确删除。

## 目标

提供 2–4 条已有规则的原子更新。每条输入必须携带稳定 rule ID、精确旧 generation、
新的 canonical rule 内容和安装端口。成功时所有规则切换到连续的新 generation；失败时
调用方不能看到部分 desired-state 更新。

这不是把 `dppd_control_apply()` 循环 N 次：逐条更新会在第一个成功后推进 repository，
后续失败时无法恢复原 generation，也不能向客户端宣称该批是原子的。

## 正常提交顺序

```text
全量预检（ID 唯一、旧 generation、规划、临时容量）
        ↓
transaction 创建全部新 generation actual 对象
        ↓
删除全部旧 generation actual 对象
        ↓
按输入顺序连续发布 repository 更新
        ↓
finalize 新对象的 rollback token，并保存 snapshot
```

新旧 generation 在第二步到第三步之间短暂共存；因此 backend 必须有足够的临时槽位。
当前后端的基础容量为 `rule_capacity + 1`，只保证单规则替换。批量更新在进入任何
backend 前必须检查可用临时容量；不足返回 `-ENOSPC`，不能先删除旧对象来腾空间。

## 失败补偿

- 新 generation 的 validate/prepare/commit 失败：由 transaction 逆序回滚，旧版本仍在；
- 删除旧版本失败：先回滚全部新版本，再以原 rule/generation 恢复此前已删的旧版本；
- 任一补偿失败：actual/desired 一致性无法证明，进入 `RECOVERY_RECONCILIATION_REQUIRED`
  并返回 `-EUCLEAN`；
- repository 更新理论上在预检后不会失败。若未来引入并发而发生失败，不能通过重新 apply
  伪造旧 generation；必须进入隔离，而不是发布部分结果。

## 语义边界

- 仅接受精确旧 generation，不接受 `0` 或 `any`；
- 同一批不得含重复 rule ID，也不混入 create/delete；
- `PREFER_HARDWARE` 仍只允许在无副作用的 `validate` 失败且软件语义等价时回退；
- `QUEUE` 和 transfer 不得降级到软件 backend；
- 结果数组按请求顺序返回，每条结果共享同一 transaction ID。

## 验收

1. 两条硬件规则更新后，repository 与 actual 均只保留新 generation；
2. 第二条新建失败时，不删除任何旧 generation；
3. 第二条旧版本删除失败时，新版本全部撤销、旧版本全部恢复；
4. 容量不足在触碰 backend 前返回 `-ENOSPC`；
5. net_ring 上 `prefer` 规则可在软件 backend 完成相同的更新/回滚语义。
