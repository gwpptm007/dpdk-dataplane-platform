# 规则失败事件历史

2026-10-09 新增 `dppctl rule-history` 与 `/dppd/rule_history`，保留本进程最近 64 次
已完成的规则控制失败，每页最多返回 4 条。每条独立保存失败阶段、规则身份、原始错误、
补偿错误、最终返回错误和是否已完整发布账本，后续成功操作不会改写已有记录。

当前管理协议为 **v15**，`dppd` 与 `dppctl` 必须一起更新；v14 及更旧客户端返回
`EPROTO`。规则快照格式仍为 v2，失败历史不写入快照，进程重启后清空。

## 查看和分页

```bash
./build/dppctl rule-history
./build/dppctl --socket /tmp/demo-control.sock rule-history
```

先输出窗口信息，再输出本页事件。假设本进程已发生 70 次失败，首行如下：

```text
rule-history revision=70 capacity=64 total=64 returned=4 overwritten=6 oldest=7 newest=70 after=0 next-after=10 more=yes gap=yes exhausted=no
```

这表示失败 1–6 已被覆盖，本页返回 7–10，后面仍有记录。后续页同时带上 `next-after`
和首个回应中的 `revision`，直到 `more=no`：

```bash
./build/dppctl rule-history 10 70
./build/dppctl rule-history 14 70
```

期间若新增失败，精确版本查询返回 `ESTALE`，应从零重新读取窗口。成功修改规则、
删除、幂等请求和只读查询不会推进历史版本，因此不会让失败历史分页失效。
省略版本只表示读取当前窗口，不能保证多次请求来自同一个窗口。

参数只接受十进制非负整数，不接受符号、空白、小数或多余参数。
`AFTER_EVENT_ID` 可取 0 到 `18446744073709551615`；最大值返回空页。
`EXPECTED_REVISION` 可取 0 到 `18446744073709551614`；最大 uint64 保留为内部
“不检查版本”，不能显式输入。空历史的版本为零，可以用 `rule-history 0 0` 精确查询。

| CLI 字段 | 含义 |
|---|---|
| `revision` | 最近一次被历史接受的失败 ID，也是本进程窗口版本 |
| `capacity` / `total` | 固定容量 64 / 当前保留条数，后者不是全部失败次数 |
| `returned` | 本页条数，最多 4 条 |
| `overwritten` | 已被较新失败覆盖的记录数 |
| `oldest` / `newest` | 当前保留窗口两端的失败 ID，空窗口均为零 |
| `after` | 请求游标，只返回 ID 严格大于它的事件 |
| `next-after` | 本页最后一条 ID；空页保留原请求游标 |
| `more` | 当前窗口中是否还有下一页 |
| `gap` | 游标之后存在已被覆盖、无法补读的记录 |
| `exhausted` | 失败 ID 空间是否耗尽并冻结历史 |

`gap` 判断的是该游标是否漏掉记录，不是窗口曾否发生覆盖。上例以 `after=0` 查询时
有缺口，以 `after=6` 查询则没有缺口，因为从 7 开始的记录仍完整保留。
重复读取不会消费记录；当历史不变时，同一游标与版本返回相同内容。

所有 ID 和版本都只在同一个进程中有效。重新连接或进程重启后从零开始读取，不能
沿用旧游标：新进程可能恰好生成相同版本数字，接口没有跨进程身份令牌。

## 每条失败说明什么

`event id` 只随失败连续增加；`sequence` 是原有的公开控制操作序号，成功请求也占用
操作序号。两者不能混用，分页必须使用事件 ID。历史不保存时间戳，只提供发生顺序。

| CLI 字段 | 含义 |
|---|---|
| `id` / `sequence` | 失败事件 ID / 全部公开控制操作中的序号 |
| `operation` / `stage` / `kind` | 公开操作、最初失败阶段和错误分类 |
| `rule` / `generation` / `transaction` / `rules` | 可定位的规则、版本、事务 ID 和请求条数 |
| `port-known` / `port` | 失败端口是否已知及对应端口 |
| `backend-known` / `backend` | 后端是否已知；名称为 `software`、`rte_flow` 或 `unknown` |
| `cause-error` / `response-error` | 最初原始负 errno / 控制 API 最终返回的负 errno |
| `compensation-error` / `compensation-rule` | 第一处补偿失败的负 errno 和可定位的规则 ID，没有则为零 |
| `applied` | 此请求是否已完整发布规则账本 |

例如创建返回 `EIO`，回滚删除又返回 `EFAULT`，最后返回 `EUCLEAN`，一条记录保留
这三个错误。`applied=no` 不能证明驱动没有部分副作用，应结合恢复隔离状态检查。
若规则已经发布，保存文件返回 `ENOTDIR`，则记录 `applied=yes`，原始错误为
`ENOTDIR`，最终修改 API 错误为 `EUCLEAN`。分类、未知身份和错误语义与
[最近失败指标](rule_telemetry.md) 一致。

计数边界沿用公开控制请求：单条和批量创建/更新/删除、启动重放、显式 flush、清理重试。
一笔批量失败只增加一条，内部补偿不另记一条。成功、幂等、状态读取、COUNT、画像和
诊断探测不增加失败历史。客户端解析、协议版本校验或管理服务隔离检查直接拒绝的请求
没有进入控制 API，也不增加历史。直接调用控制 API 被隔离拒绝则会记录，与现有指标一致。

在线隔离和启动重放隔离期间仍可查询历史。清理重试失败增加新事件，原有补偿错误保留；
成功重试不抹去旧事件。新进程正常重放不产生失败历史，重放失败则记录为新进程事件。
历史窗口属于诊断信息，没有清零、筛选或持久化接口，不能作为完整审计日志。

## Telemetry 与一致性

```text
/dppd/rule_history
/dppd/rule_history,10
/dppd/rule_history,10,70
```

连接本实例的 DPDK telemetry v2 socket，方法见 [规则 Telemetry](rule_telemetry.md)。
非法参数、过期版本或实例未绑定时返回 JSON `null`；需要精确错误码时使用管理 API。
每页使用扁平字典，无嵌套数组，窗口字段映射如下：

| CLI | JSON |
|---|---|
| `oldest` / `newest` | `oldest_event_id` / `newest_event_id` |
| `after` / `next-after` | `after_event_id` / `next_after` |
| `more` | `has_more` |
| 其他窗口字段 | 同名；布尔值为 0/1 |

JSON 额外提供整份副本的 `publication`。每条字段前缀为 `event_0_` 到 `event_3_`，
有效索引小于 `returned`。事件字段为 `id`、`sequence`、`operation`、`stage`、`kind`、
`rule_id`、`generation`、`transaction_id`、`rule_count`、`port_known`、`port`、
`backend_known`、`backend`、`cause_error`、`response_error`、`compensation_error`、
`compensation_rule_id` 和 `applied`。规则和事件 ID 均以完整 uint64 输出。

历史版本、规则仓库 generation 和副本 publication 含义不同。失败可能不改变规则，
成功修改又不增加失败；跨页一致性必须使用历史 `revision`。管理回应可能先于 telemetry
发布，对齐 CLI 和 JSON 时应等待目标历史版本出现。不同命令的回应不承诺跨次原子读取。

管理线程仅在请求结束、错误和生效状态确定后追加固定大小的值记录，再随规则副本发布。
驱动或保存操作进行中，telemetry 仍返回上一份完整副本。回调只在生命周期短锁内选取
一页，离开锁后构造 JSON，不读取可变控制对象。追加和读取不新增时钟、动态分配、驱动
访问或快照保存，逐包路径不新增操作；历史空间不随运行时间增长。

失败 ID 正好达到 `18446744073709551614` 时仍正常保存；再发生失败时设置
`exhausted=yes` 并冻结窗口、版本和覆盖计数，防止编号回绕。现有最近失败指标继续更新，
因此耗尽后历史最后一条可能不再等于 `rule-metrics` 的最近失败。分页版本约束事件窗口，
冻结标志本身可在版本不变时由 no 变为 yes。

## 验证记录

2026-10-09，Ubuntu 22.04.5 / DPDK 21.11.9 / GCC 11.4 / Meson 0.61.2：

- `-Werror` 全量构建与 25/25 单元测试通过
- 130 次失败穿插成功请求，验证循环覆盖、完整 64 条分页、缺口边界、最大游标、精确空版本和过期版本
- 验证失败 ID 耗尽冻结、原有操作序号回绕仍不影响失败 ID、完成态复制和重复结束不重复记录
- 真实管理 socket 拒绝 v14，v15 正常及隔离查询通过；追加和读取不新增分配或时钟调用
- ring/TAP 正式进程对齐全部 CLI/JSON 字段，并发 20 次整批更新与 130 次版本冲突，完整读取 16 页保留窗口
- 存储失败、成功删除后保留、重启清空、两组在线隔离及一组启动重放隔离通过，清理失败历史保留原始补偿错误
- 历史事件、规则 telemetry、失败分类、历史耗时四项单测通过 AddressSanitizer/UBSan
- 既有 2/4 条规则状态、ring/null 健康及线程故障、四组正式 worker 收发回归通过

```bash
meson setup build -Dwerror=true
meson compile -C build
meson test -C build --print-errorlogs
python3 tests/integration/rule_telemetry.py --build-dir build
sudo python3 tests/integration/rule_telemetry.py --build-dir build --tap
python3 tests/integration/recovery_isolation.py --build-dir build
python3 tests/integration/software_traffic.py --build-dir build
python3 tests/integration/health_readiness.py --build-dir build
python3 tests/integration/rule_status.py --build-dir build
```

独立 sanitizer 构建使用 `-Db_sanitize=address,undefined -Db_lundef=false -Db_pie=false`。
测试只使用自己的虚拟端口，结束后接口、socket、handle 和 mbuf 回收。
跨进程 residual flow reconciliation 仍待实现；本轮没有验证物理 PMD 故障恢复或硬件卸载性能。
