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
- management v3 的 `persistence-status` 状态查询和 `persistence-flush` 显式修复；
- `--state-path` 显式启用 daemon 持久化；
- 启动时加载、topology resolve、全量硬件事务重放及 preserved generation 发布；
- 任一规则重放失败时逆序回滚本轮硬件对象并拒绝启动。

尚未实现：

- degraded startup 和部分规则恢复；当前策略固定为 fail closed；
- rollback 自身失败后的自动 reconciliation；此时返回 `EUCLEAN` 并拒绝启动；
- 格式升级和跨版本迁移工具。

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
    → validate/prepare hardware
    → commit hardware
    → publish preserved generations
    → restore repository global generation
```

任一规则无法在当前硬件/PMD 上恢复时，默认应 fail closed 并回滚本次已创建 flow；
只有全部硬件对象提交成功后，才一次性发布 snapshot 中保留的逐规则 generation 和
repository global generation。文件不存在时会原子创建一个空 v2 snapshot；目录不可写
或空快照创建失败时同样拒绝启动，不会静默退化为内存模式。

v1 记录没有 `install_port_id`，无法判断 ingress/egress 规则应重放到哪个 ethdev。
加载器因此对 v1 返回 `EPROTONOSUPPORT`，不使用 port 0 或第一个端口进行猜测。迁移工具
必须由操作者为每条旧规则补充安装端口后才能生成 v2。
