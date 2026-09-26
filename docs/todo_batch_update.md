# 跨规则原子更新

状态：control、repository、management v7 和 CLI 已实现（2026-09-26）。纯软件批量更新已改为一次快照发布；DPDK 21.11.9 下 `-Werror` 构建、15/15 单测及四组 net_ring 进程间验证通过，见 [验证记录](validation_batch_update.md)。并发报文分类测试使用解析后的测试报文，不代表物理 PMD 或实际网卡收发验收。

在线补偿失败的两条进程级故障注入路径也已通过：worker 停止、普通请求阻断、重试失败
保持隔离、重试成功退出，以及旧 snapshot 重启恢复。使用显式 flow API 测试库，不代表
物理 PMD 验收。

## 目标

提供 2–4 条已有规则的原子更新。每条输入必须携带稳定 rule ID、精确旧 generation、
新的 canonical rule 内容和安装端口。成功时所有规则切换到连续的新 generation；失败时
调用方不能看到部分 desired-state 更新。

这不是把 `dppd_control_apply()` 循环 N 次：逐条更新会在第一个成功后推进 repository，
后续失败时无法恢复原 generation，也不能向客户端宣称该批是原子的。

## 正常提交顺序

旧版本实际归属与新执行计划全部为软件时：全量校验 → 复制活跃快照 → 在副本中替换全部精确旧版本并创建新计数器 → 一次发布 active 指针 → 整批发布 repository → 保存 snapshot。未更新规则共享原计数器；发布前任意分配失败都销毁副本，旧表及其计数保持不变。旧快照由 QSBR 延迟回收，不要求工作线程暂停。

包含硬件或软硬件迁移时继续使用以下流程：

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

上述硬件及混合流程的新旧 generation 暂时共存，需要临时槽位；后端容量仍为
`rule_capacity + 1`。纯软件快照替换复用旧槽位，满表也可更新。
`PREFER_HARDWARE` 在 PMD validate 前仍保守检查硬件目标容量。旧规则全部在软件且
所有目标都有软件执行可能时，可暂缓软件临时容量检查；如果最终计划是混合路径，
在任何安装前复查实际软件目标所需的临时容量，不足返回 `ENOSPC`。其他情况仍预检
所有可能的软件目标。容量不足本身不触发降级，也不提前删除旧对象来腾空间。
这不是 NIC 的硬件资源预留；PMD 自身资源不足仍可能在 validate/create 时报告失败。

## 失败补偿

纯软件路径在发布前失败只丢弃私有副本，无需逐条回滚。以下安装与删除补偿规则适用于硬件或混合路径；repository 异常隔离与持久化 dirty 处理适用于两条路径。

- 新 generation 的 validate/prepare/commit 失败：由 transaction 逆序回滚，旧版本仍在；
- 删除旧版本失败：先回滚全部新版本，再以原 rule/generation 恢复此前已删的旧版本；
- 任一补偿失败：actual/desired 一致性无法证明，进入 `RECOVERY_RECONCILIATION_REQUIRED`
  并返回 `-EUCLEAN`；
- repository 更新不分配内存，先复查整批条件，再执行无失败点的发布循环。
  若复查失败，不能通过重新 apply 伪造旧 generation；必须进入隔离，desired
  repository 保留整批旧版本。
- 在线隔离会停止软件 worker，普通管理操作返回 `EUCLEAN`；清理重试成功后退出。
  这不保证硬件中残留的规则停止处理报文。软件对象在退出清理阶段释放。
- snapshot 保存失败遵循既有 dirty/fail-stop 语义：整批内存更新已生效，但返回
  `EUCLEAN`；先通过 `persistence-status/flush` 处理落盘故障，不能把错误理解为已回滚。

## 语义边界

- 仅接受精确旧 generation，不接受 `0` 或 `any`；
- 同一批不得含重复 rule ID，也不混入 create/delete；
- 相同内容也生成连续新 generation；重放同一组旧 generation 返回 `ESTALE`；
- generation 不允许溢出或变成保留的 `UINT64_MAX`，预检返回 `EOVERFLOW`；
- `PREFER_HARDWARE` 仍只允许在无副作用的 `validate` 失败且软件语义等价时回退；
- `QUEUE` 和 transfer 不得降级到软件 backend；
- 结果数组按请求顺序返回，每条结果共享同一 transaction ID。
- 纯软件更新保证每次报文分类只使用完整旧表或完整新表，批次内不出现部分替换的快照。
  已持有旧表的线程可继续处理当前报文，不保证多个报文、线程或收包批次同时切换。
- 硬件及混合更新仍只保证 desired-state 整批发布，实际后端可能暂时保留新旧版本。
- 新软件版本 COUNT 从零开始，未更新规则保留计数；发布前失败保留旧版本计数。
  硬件及混合路径的对象重建不承诺保留 COUNT。

## 管理入口

管理协议 v7 新增 `RULE_UPDATE_BATCH`，每条可独立携带完整 canonical rule 和安装端口。
CLI 提供较窄的构造器：

```text
dppctl update-drop-batch PORT PRIORITY prefer|require|software RULE_ID GENERATION RULE_ID GENERATION [RULE_ID GENERATION [RULE_ID GENERATION]]
```

CLI 将每条规则完整替换为同一端口、指定 priority/policy 的 Ethernet ingress DROP，
不会保留旧规则的过滤条件或 MARK/COUNT。精确旧 generation 从最近一次 `list/get` 获取。
v6 客户端会被 v7 daemon 以 `EPROTO` 拒绝，需一起重编译部署。

## 验收用例

1. 两条硬件规则更新后，repository 与 actual 均只保留新 generation；
2. 第二条新建失败时，不删除任何旧 generation；
3. 第二条旧版本删除失败时，新版本全部撤销、旧版本全部恢复；
4. 纯软件满容量更新成功，混合路径临时空间不足在安装前返回 `-ENOSPC`；
5. net_ring 上 `prefer` 回退到软件后使用整批快照替换；
6. 两条、四条软件规则在并发分类时各反复更新 2000 次，不出现混合表的匹配结果；
7. 快照、副本计数器和退役记录的所有分配失败点均保留旧表、旧版本和旧计数。

`test_batch_update` 覆盖 2/4 条更新、跨端口、相同内容、版本冲突、重复 ID、容量不足、
版本溢出、新建/删除/补偿失败、软件回退、混合 backend 补偿和持久化失败。另有 repository
整批发布、management v7 边界和 CLI 参数测试。可执行：

```bash
bash scripts/build.sh
meson test -C build --print-errorlogs 'atomic batch update' 'software batch snapshot publication' 'management protocol and socket' 'dppctl batch update arguments'
python3 tests/integration/batch_update.py --build-dir build
python3 tests/integration/recovery_isolation.py --build-dir build
python3 tests/integration/software_traffic.py --build-dir build
```
