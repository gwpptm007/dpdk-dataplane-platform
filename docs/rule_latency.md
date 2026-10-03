# 历史规则安装耗时分布

2026-10-04 新增 `dppctl rule-latency` 与 `/dppd/rule_latency`。它们统计本进程已经发生的
成功后端提交，提供次数、均值、最小值、最大值、固定区间和 P50/P95/P99 上界。
删除规则或回滚请求不会抹去已经成功提交的样本，因此可以观察目前账本里已不存在的安装。

本机管理协议为 **v14**，`dppd` 与 `dppctl` 必须一起更新；v13 及更旧客户端返回 `EPROTO`。
规则快照格式仍为 v2；耗时历史只在内存保存，重启后从本次实际安装或启动重放重新累计。

## 查看三组统计

```bash
./build/dppctl rule-latency
./build/dppctl --socket /tmp/demo-control.sock rule-latency
```

命令不接受其他参数。输出三组，每组一行摘要和十六行 `bucket`，共 51 行。
三组测量的操作不同，不能直接合并成单条安装延迟：

| `scope` | 一个样本表示什么 | `rules` 的含义 |
|---|---|---|
| `software` | 一次成功的软件单条提交 | 成功提交涉及的规则数，每次加一 |
| `software_batch` | 一次成功的纯软件整批快照发布 | 各批次成员数量之和，每批只增加一个耗时样本 |
| `rte_flow` | 一次成功的驱动 create 提交 | 成功创建涉及的规则数，每次加一 |

例如更新四条纯软件规则，只给 `software_batch` 增加一次提交和四条涉及规则，
不会给每个成员重复添加同一个耗时，也不会增加 `software`。硬件或混合事务中
通过单条后端提交的成员分别进入对应分组，不记录整笔管理请求的端到端延迟。
纯软件批量创建和启动重放当前仍按实际的单条后端提交记录；批量删除不产生样本。

计时直接复用 [规则安装状态](rule_status.md) 已有的后端提交结果，起止位置保持不变。
软件测量包含副本分配、指针发布和当次可完成的退役回收，硬件测量包含驱动 create 调用；
不包含规划、能力校验、准备阶段、后续旧对象删除、规则账本发布或磁盘保存。
这是后端控制操作耗时，不能解释成报文处理延迟或网卡内部完成时间。

## 读懂摘要与未知测量

| CLI 字段 | 含义 |
|---|---|
| `samples` | 有效测量的成功提交次数，等于十六个区间计数之和 |
| `unavailable` | 成功提交但时钟测量不可用的次数，单独保存 |
| `rules` | 有效与未知测量两类成功提交涉及的规则总数 |
| `total-ns` | 有效样本耗时总和，单位纳秒 |
| `min-ns` / `max-ns` | 有效样本中实际观察到的最小值和最大值 |
| `mean-available` / `mean-ns` | 均值是否可用，以及总和除以有效样本数的整数结果 |
| `quantiles-available` | P50/P95/P99 区间上界是否可用 |
| `p50-upper-ns` / `p95-upper-ns` / `p99-upper-ns` | 对应分位数所落区间的上界，受实际最大值约束 |
| `total-saturated` | 耗时总和已经溢出并饱和，均值不可用 |
| `counters-saturated` | 计数不能继续完整更新，整组已冻结，均值和分位数不可用 |

成功提交总次数是 `samples + unavailable`，未知耗时不进入区间、总和或分位数。
如果一次提交被有效测量为零纳秒，它仍是有效样本，`mean-available=yes`。
没有有效样本时可用标志为 `no`，相应数值为零；必须结合标志解释零值。
存在未知项但也存在有效样本时，摘要仅描述已测量的部分，不能覆盖未知项。

所有计数和总和使用 uint64。总和超过可表示范围时保留 `UINT64_MAX` 并设置
`total-saturated=yes`，区间和分位数仍可继续使用。若 `samples`、`unavailable`
或 `rules` 中任何一个不能增加，整组保留前一份完整计数并设置 `counters-saturated=yes`，
后续记录停止，避免绕回零或破坏“区间和等于有效样本数”的关系。
正好达到最大值不算溢出；下一次无法增加才标记饱和。冻结前的原始值仍可读取。

## 固定区间与分位数上界

区间互斥，第一段包含零，其余左开右闭；不是累计小于某个上界的计数。
例如正好 5000 ns 进入区间 1，5001 ns 进入区间 2。CLI 和 JSON 均输出纳秒上界。

| 索引 | 上界 |
|---|---|
| 0–3 | 1 μs、5 μs、10 μs、50 μs |
| 4–7 | 100 μs、500 μs、1 ms、5 ms |
| 8–11 | 10 ms、50 ms、100 ms、500 ms |
| 12–14 | 1 s、5 s、10 s |
| 15 | 大于 10 s 的所有 uint64 纳秒值，上界编码为 `18446744073709551615` |

P50/P95/P99 先取向上取整的样本排名，再找到包含该排名的区间。
返回 `min(区间上界, 实际最大耗时)`，所以最后一段也能提供有限的观察上界。
它们是分位数上界估计，固定区间不保留原始排序样本，不能提供精确分位数。

例如三个样本为 1000、4000、9000 ns：均值为 4666 ns，P50 落在第二个区间，
其上界是 5000 ns；P95/P99 落在第三个区间，10000 ns 被实际最大值约束为 9000 ns。
4000 ns 的真实中位数没有被保存，查询不能声称测得精确的 5000 ns 中位数。

## 成功提交与请求结果的关系

| 场景 | 是否增加历史 |
|---|---|
| 创建成功后又被补偿删除 | 增加，成功提交已经发生 |
| 删除旧对象失败后成功重建旧对象 | 重建增加一个新提交 |
| 后端提交成功，但保存快照失败 | 增加，文件保存错误不撤销已完成的提交 |
| validate、prepare、create 返回错误 | 不增加，即使失败 create 留下待清理 handle |
| 软件快照发布前分配失败 | 不增加，旧表和旧历史保留 |
| 幂等 apply、删除、探测、COUNT、状态读取、flush 或清理重试 | 不增加 |
| 启动重放成功创建规则 | 从新进程的零值开始增加本次安装 |

`rule-metrics` 数公开请求的完成结果，耗时历史数后端成功提交，两者口径不同。
一笔失败的两条硬件事务可能先成功创建一条、再在第二条失败；请求失败数增加一次，
耗时历史仍增加第一条的一次成功提交。失败 create 的耗时本轮没有记录。
恢复隔离期间 `rule-latency` 仍可读取，包括尚未发布到账本的成功提交历史。

## Telemetry 查询与发布

使用本实例的 DPDK telemetry v2 socket：

```text
/dppd/rule_latency
/dppd/rule_latency,software
/dppd/rule_latency,software_batch
/dppd/rule_latency,rte_flow
```

省略参数输出三组，`scope=all`；指定参数只输出该组。拼错、大小写不匹配或多组参数
返回 JSON `null`。服务未绑定或已退出时也返回 `null`，与其他规则 telemetry 命令一致。
连接、分页和只读副本的通用约定见 [规则失败与 Telemetry](rule_telemetry.md)。

JSON 使用扁平字典。通用字段为 `publication`、`scope`、`bucket_count=16`，
以及 `bucket_0_upper_ns` 到 `bucket_15_upper_ns`。各组字段以 `software_`、
`software_batch_`、`rte_flow_` 为前缀：

- CLI 的连字符字段在 JSON 中改为下划线，如 `software_mean_ns`、`rte_flow_p99_upper_ns`
- `available` 和 `saturated` 标志在 JSON 中为 0/1
- 区间次数为 `software_bucket_0` 到 `software_bucket_15`，其他组同理
- 过滤分组仍保留全部共享上界，其他组的统计字段不输出

一次回应中的三组历史来自同一份完整发布副本。管理回应可能先于下一轮副本发布，
跨次请求可以读到不同 `publication`；对比 CLI 与 JSON 时应等待目标提交次数发布。
失败或清理可能只改变发布号，历史本身保持原值。即使后端提交没有改变 repository
generation，发布检查仍会发现历史变化。

记录不新增时钟调用或内存分配，软件历史在原有控制面写锁内更新，硬件历史由管理线程
串行更新。telemetry 只在生命周期短锁内复制历史值，离开锁后构造 JSON；查询不调用
PMD、COUNT、时钟、快照保存或软件回收。逐包路径没有新增锁、分配或统计操作。
三个固定区间数组的空间不随安装历史增长；没有历史原始样本列表或清零接口。

## 验证记录

2026-10-04，Ubuntu 22.04.5 / DPDK 21.11.9 / GCC 11.4 / Meson 0.61.2：

- `-Werror` 全量构建和 24/24 单元测试通过
- 十六个区间的精确边界及边界加一、零耗时、分位数排名、未知时钟、总和溢出及计数冻结通过
- 真实控制路径验证单条/整批分组、重复请求、删除保留、create/分配失败、成功后补偿、旧规则重建与保存故障
- 查询不增加时钟或驱动访问、不消耗故障分配；真实管理 socket 拒绝 v13，并允许 v14 在隔离期间查询
- ring/TAP 正式进程对齐 CLI 与 JSON，验证过滤、72 条分页、20 次并发整批发布、40 条涉及规则、删除保留、dirty/flush 与新进程重放
- 两组在线恢复隔离和一组启动重放隔离，验证撤销/恢复失败后的历史、清理重试和新进程重新累计
- 四项相关单测通过 AddressSanitizer/UBSan：历史耗时、规则 telemetry、失败观测和安装状态
- 既有规则状态 2/4 条与 TAP 生命周期、ring/null 健康及线程故障、四组正式 worker 收发回归通过

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
sudo python3 tests/integration/rule_status.py --build-dir build --tap
```

独立 sanitizer 构建使用 `-Db_sanitize=address,undefined -Db_lundef=false -Db_pie=false`。
TAP 仅创建测试自己的临时接口；检查接口、socket、handle 和 mbuf 回收。
测试不包含真实物理 PMD 安装性能、端到端 P99 达标或跨进程 residual flow 清理验收。
失败事件历史列表和跨进程 reconciliation 仍待实现。
