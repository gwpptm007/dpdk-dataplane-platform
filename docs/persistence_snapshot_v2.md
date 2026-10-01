# Rule snapshot 持久化格式 v2

## 1. 当前状态

已实现：

- 与 C 结构体 padding 无关的显式编码；
- 固定 little-endian 元数据；
- IPv4/L4 网络字节原样保存；
- rule id 升序记录；
- CRC32 完整文件校验；
- 同目录临时文件；
- 文件 `fsync`；
- 原子 `rename`；
- 父目录 `fsync`；
- `0600` 文件权限；
- symlink 拒绝、长度校验、截断校验和 rule IR 语义校验；
- control apply/delete 成功发布 repository 后自动保存完整 snapshot；
- 保存失败后进入 dirty/fail-stop 状态，并以 `-EUCLEAN` 报告“变更可能已生效但未持久化”；
- dirty 状态下的新写操作必须先成功保存当前 repository，失败则拒绝继续变更；
- 相同 apply 命令幂等重试时先修复 snapshot，再返回 `UNCHANGED`；
- management v5 的 `persistence-status` 状态查询和 `persistence-flush` 显式修复；
- `--state-path` 显式启用 daemon 持久化；
- 启动时加载、topology resolve、按策略选择硬件或软件的全量事务重放及 preserved generation 发布；
- 任一规则重放失败时逆序回滚本轮对象并拒绝启动。
- v1 到 v2 的离线迁移工具；迁移时显式指定统一的安装端口。

尚未实现：

- degraded startup 和部分规则恢复；当前策略固定为 fail closed；
- 跨进程的 residual flow 枚举/删除；不同 PMD 的可用能力不同，不能用无差别
  `rte_flow_flush()` 代替；
- 多端口 v1 快照的逐规则端口映射迁移；v1 格式没有该信息，不能安全推断。

默认仍不启用持久化；只有显式传入 `--state-path PATH` 才会读取或创建 snapshot。
启用后，daemon 必须成功完成整个恢复事务才开放 management socket 和启动 worker。

## 2. 文件布局

所有整数元数据使用 little-endian。IPv4 地址、IPv4 mask、TCP/UDP port 和 port
mask 已经是协议网络字节序，按原始字节保存。

```text
+------------------------------+
| Header: 48 bytes             |
+------------------------------+
| Rule record 0: 264 bytes     |
+------------------------------+
| Rule record 1: 264 bytes     |
+------------------------------+
| ...                          |
+------------------------------+
```

Header：

| Offset | Size | 字段 |
|---:|---:|---|
| 0 | 8 | magic `DPPRULE\0` |
| 8 | 4 | format version，当前为 2 |
| 12 | 4 | header size，固定 48 |
| 16 | 4 | record size，固定 264 |
| 20 | 4 | rule count |
| 24 | 8 | repository global generation |
| 32 | 8 | payload size |
| 40 | 4 | CRC32 |
| 44 | 4 | reserved，必须为 0 |

CRC32 计算覆盖 checksum 字段清零后的完整 header 和全部 payload。它用于检测存储
损坏，不提供身份认证；部署层仍必须保护文件和父目录权限。

## 3. Rule record

每条记录固定 264 字节：

```text
id                 u64
generation         u64
domain             u32
fallback           u32
group              u32
priority           u32
nb_matches         u16
nb_actions         u16
install_port_id    u16 little-endian
reserved           u16，必须为 0
matches[8]         8 × 20 bytes
actions[8]         8 × 8 bytes
```

Match record：

- `type: u32`
- `payload: 16 bytes`
- ETH payload 全零；
- IPv4 保存 src/src-mask/dst/dst-mask 四个网络字节序字段；
- UDP/TCP 保存 src/src-mask/dst/dst-mask 四个网络字节序字段；
- represented-port 使用 little-endian `u16`。

Action record：

- `type: u32`
- `payload: 4 bytes`
- DROP/COUNT payload 全零；
- QUEUE/represented-port 使用 little-endian `u16`；
- MARK 使用 little-endian `u32`。

记录必须按 rule id 严格升序。解码后每条规则重新执行 `dppd_rule_validate()`；
rule id/generation 为 0、类型越界、数量越界、顺序错误或 action 语义错误均拒绝整个
snapshot，不允许部分恢复。

## 4. 原子保存语义

保存顺序：

```text
repository generation 固定
    → 按 rule id 获取完整页
    → 编码和 CRC
    → open(path.tmp.PID, O_EXCL, 0600)
    → write all
    → fsync(temp)
    → close(temp)
    → rename(temp, path)
    → fsync(parent directory)
```

在 `rename` 前失败时，旧快照保持不变，临时文件尽力删除。`rename` 成功但父目录
`fsync` 失败时，新快照已可见，但掉电持久性不能保证，调用方必须把该状态视为
持久化失败并记录告警。

## 5. 写路径失败语义

control service 采用 fail-stop dirty 模型：

```text
hardware commit
    → repository publish
    → save complete snapshot
        ├─ success: persisted_generation = current_generation，返回成功
        └─ failure: dirty = true，返回 -EUCLEAN

next mutation while dirty
    → preflight save current repository
        ├─ success: clear dirty，继续本次 mutation
        └─ failure: 返回 -EUCLEAN，不接触硬件和 repository
```

`-EUCLEAN` 不表示操作完全失败。对于首次 apply 或实际 delete，硬件对象和内存
repository 可能已经提交，只是磁盘状态仍旧。客户端应先修复目录、权限、容量或底层
I/O 问题，再执行 `dppctl persistence-flush`，然后通过 `persistence-status` 以及
`get/list` 核对结果。apply 可用同一命令幂等重试；delete 已生效后使用旧 generation
重试会返回 `ESTALE`，因此不能只依赖重复 delete 判断首次操作是否生效。

`dppctl persistence-status` 返回：

- `enabled`：control service 是否已经绑定 state path；
- `dirty`：内存状态是否尚未得到一次成功的完整 snapshot；
- `persisted-generation`：最近一次确认保存成功的 repository generation；
- `current-generation`：当前内存 repository generation；
- `last-error`：最近一次保存失败的负 errno，成功后清零。

底层 attach API 只建立后续 mutation 的保存关系，不读取或覆盖已有文件。非零
repository generation 会被保守标记为 dirty，直到显式 flush 或下一次 mutation 的
preflight 完成一次完整保存。调用方必须先完成启动恢复/reconciliation，或者确认目标
是一个全新的 state path。

## 6. 启动恢复语义

`--state-path` 的恢复顺序为：

```text
load + validate snapshot
    → resolve current topology
    → plan every rule
    → validate/prepare selected backends
    → commit selected backends
    → publish preserved generations
    → restore repository global generation
```

`software` 规则恢复到软件后端；`prefer` 仅在 PMD validate 失败且软件语义等价时回退；
`require` 不允许软件回退。任一规则无法在当前环境恢复时，默认 fail closed 并回滚
本次已创建对象；只有全部对象提交成功后，才一次性发布 snapshot 中保留的逐规则 generation 和
repository global generation。文件不存在时会原子创建一个空 v2 snapshot；目录不可写
或空快照创建失败时同样拒绝启动，不会静默退化为内存模式。

恢复入口仅接受全新空服务：规则账本、硬件和软件后端都必须为空，已有实际软件对象
但账本为空时也返回 `EBUSY`。恢复保留规则内容、安装端口和版本，COUNT 不存入快照，
软件计数器从零开始。2026-10-01 已验证非空软件快照的两次 daemon 重启、恢复后的
批量更新、损坏快照拒绝启动，以及恢复后实际 RX/TX，见 [验证记录](validation_batch_update.md)。

v1 记录没有 `install_port_id`，无法判断 ingress/egress 规则应重放到哪个 ethdev。
daemon 加载器因此对 v1 返回 `EPROTONOSUPPORT`，不使用 port 0 或第一个端口进行猜测。

若确认旧快照内的**所有**规则原本都安装在同一 ethdev，可在 daemon 未运行时使用：

```bash
./build/dppd-snapshot-migrate \
  --input /var/lib/dppd/rules.v1 \
  --output /var/lib/dppd/rules.v2 \
  --install-port 5
```

工具只读取通过完整 CRC、长度、旧 record 布局和 rule IR 校验的 v1 文件；将指定端口
赋给所有规则，保留 rule generation 与 repository generation，并以 v2 的原子保存语义
写出新文件。它拒绝输入和输出为同一 inode，绝不原地修改旧快照。若旧规则实际分布在
多个安装端口，不能使用该工具；必须先取得逐规则端口映射，再由后续专用迁移流程处理。

## 7. 回滚失败的恢复隔离模式

如果启动重放中某条 flow 创建失败，且 transaction 逆序删除已创建 flow 时又失败，daemon
不会启动 worker，也不会发布空 repository。它保留当前进程仍持有的 `rte_flow` handle，
只开放本地管理 socket 的两个命令：

```text
dppctl reconcile-status
dppctl reconcile-retry
```

`reconcile-status` 返回恢复状态、仍残留的 backend 对象数以及最近一次 PMD 删除错误。
隔离期间包括 `ping`、`list`、`get`、apply/delete/count、持久化 flush 在内的普通操作均
返回 `EUCLEAN`。`reconcile-retry` 对每个仍持有 handle 的对象再次调用 PMD remove；若
全部删除成功，daemon 进入 `restart-required` 并以失败码退出。服务管理器或操作者必须
重新启动 daemon，让它从未改变的 snapshot 重新执行完整恢复。

该机制仅解决进程仍存活时的瞬态 PMD 删除失败。若进程在 retry 前被 `SIGKILL`、崩溃或
机器断电，handle 无法跨进程序列化，当前版本不会尝试猜测或对整个端口执行
`rte_flow_flush()`；部署方应按目标 PMD 的受支持流程复位/清理设备。跨进程 residual
flow journal 与 PMD 专用 reconciliation 是后续独立工作。

在线批量更新补偿失败同样进入上述隔离模式：停止软件 worker，保留旧 desired snapshot，
只接受恢复查询与重试，成功后退出重启。`residual_objects` 统计硬件 backend 对象；软件
对象在 worker 停止后的进程清理阶段释放。停止软件 worker 不代表硬件残留 flow 停止转发。
若整批更新成功但 snapshot 保存失败，则进入原有 dirty/fail-stop 状态而非恢复隔离；
此时内存是整批新版本，磁盘可能仍是旧版本，需使用 persistence 状态与 flush 修复。
