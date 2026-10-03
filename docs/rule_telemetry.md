# 规则失败分类与 Telemetry

规则修改失败时，现在可以查出失败发生在哪个阶段、哪条规则，以及最初错误和补偿错误。
查询同时说明账本是否已经完整发布，避免把“规则已经生效，但文件没有保存成功”误认为回滚成功。
管理协议为 **v13**，`dppd` 和 `dppctl` 必须一起更新；v12 及更旧客户端返回 `EPROTO`。
规则快照格式仍为 v2。

## 查看累计结果与最近失败

```bash
./build/dppctl rule-metrics
./build/dppctl --socket /tmp/demo-control.sock rule-metrics
```

输出包括累计值、各类失败次数和最近一次失败。没有失败时，序号和错误为零，名称为 `none`。
成功、幂等重放和只读查询都不会清除最近失败。指标只属于当前进程，重启后重新计数，
不写入规则快照；配置 `--state-path` 时，首次创建空快照或重放算一笔 `restore`。

| 字段 | 含义 |
|---|---|
| `operations` | 已完成的公开控制操作数 |
| `succeeded` / `failed` | 成功与失败操作数，二者之和等于 `operations` |
| `unchanged` | 成功且无需发布新账本的单条幂等 apply 或不存在规则的幂等删除 |
| `applied` | 完整发布账本的操作数，包括新增、更新、删除和非空状态重放 |
| `fallback_rules` | 原本优先硬件、最终随完成发布落到软件的规则数；失败前已发布但保存失败的也计入 |
| `compensation_failures` | 同时出现补偿错误的失败请求数，一笔批量请求最多增加一次 |
| `failed_after_apply` | 完整发布账本以后才失败的操作数 |

计数范围是单条 apply/remove、批量创建/更新/删除、启动重放、显式 flush 和清理重试。
一批四条规则算一笔操作，内部事务、补偿重建和自动补写不会另算一笔。
退出清理不计入公开请求。COUNT、状态、能力画像、诊断探测与 telemetry 查询都不增加这些指标。
客户端解析失败、协议版本错误，以及服务端隔离检查直接拒绝的普通请求没有进入控制 API，
也不增加指标；因此 `failed` 不是全部管理连接错误的总数。

## 读懂三个错误和生效标志

| 字段 | 含义 |
|---|---|
| `cause-error` / JSON `cause_error` | 最初导致操作失败的原始负 errno |
| `response-error` / JSON `response_error` | 控制 API 最后返回的错误 |
| `compensation-error` / JSON `compensation_error` | 第一处回滚或旧对象重建失败的原始错误，没有则为零 |
| `compensation-rule` / JSON `compensation_rule_id` | 可以定位时的补偿失败规则 ID |
| `last-applied` / JSON `last_applied` | 此失败请求是否已经完整发布账本 |

例如第二条创建返回 `EIO`，撤销第一条又返回 `EFAULT`，最终 API 返回 `EUCLEAN`：
主错误仍保留 `EIO`，补偿错误另存 `EFAULT`，并进入恢复隔离。
这时 `last_applied=false`，但可能还有残留对象，必须使用 `reconcile-status/reconcile-retry`。
**false 只表示没有完整发布，不能证明后端没有副作用或已经全部回滚。**

若文件保存返回 `ENOTDIR`，规则已经完整发布，则主错误是 `ENOTDIR`，最终修改 API
返回 `EUCLEAN`，`last_applied=true`，`failed_after_apply` 增加。显式 flush 直接返回
原始文件错误，不把它包装成 `EUCLEAN`。修复保存路径后再 flush，最近失败仍保留，
当前是否 dirty 应另看 `persistence-status` 或 `/dppd/rules`。

## 分类与阶段

分类来自原始 errno；加载/保存阶段优先归为 `persistence`，直接调用控制 API 被隔离
拒绝时归为 `isolated`。分类不意味着自动重试或自动回退。

| 分类 | 常见原因 |
|---|---|
| `invalid-input` | 已进入控制 API 的输入、结构或范围错误 |
| `conflict` | 旧版本过期、重复 ID、已经处于所请求的状态 |
| `not-found` | 必须存在的旧规则找不到 |
| `unsupported` | 规划或驱动不支持所需语义 |
| `resource` | 内存、本地槽位不足或版本空间溢出 |
| `device` | `ENODEV/EIO/EFAULT` 等底层设备操作错误 |
| `temporary` | `EAGAIN/EBUSY/ETIMEDOUT/EINTR` |
| `permission` | 非加载/保存阶段的 `EACCES/EPERM` |
| `persistence` | 加载或保存文件失败，原始 errno 仍单独提供 |
| `inconsistent` | 没有更早原因可供定位的账本与实际对象失配 |
| `isolated` | 控制 API 在恢复隔离期间拒绝操作 |
| `internal` | 其余未归类错误 |

阶段包括 `preflight`、`plan`、`validate`、`prepare`、`commit`、`remove`、`publish`、
`persist`、`load`、`reconcile` 和 `isolation`。补偿单独保存，不能覆盖最初失败阶段。
`compensate` 保留为明确的阶段枚举，当前内部补偿使用独立错误字段。

最近失败保留 operation、规则 ID、版本、事务 ID、请求条数、端口和后端是否已知。
版本预检失败记录用户提交的期望版本，规划和安装失败记录候选或旧对象的实际版本。
整体容量检查、整批软件发布、整批账本发布和完整批量文件保存无法定位单个成员，
此时 ID/版本为零并明确标记未知身份，不把最后检查的成员当成罪因。

## DPDK Telemetry 查询

启用 EAL telemetry 的实例可通过对应 file-prefix 的 telemetry v2 socket 查询。
DPDK 使用 Unix socket 和 JSON，逗号分隔命令参数；EAL 的 `--no-telemetry` 会禁用服务。
连接方式见 [DPDK 21.11 Telemetry 指南](https://doc.dpdk.org/guides-21.11/howto/telemetry.html)。

```text
/dppd/stats
/dppd/rule_failures
/dppd/rules
/dppd/rules,1000
/dppd/rules,1000,75
/dppd/rule,1001
```

| 命令 | 输出 |
|---|---|
| `/dppd/stats` | 原有软件报文累计统计 |
| `/dppd/rule_failures` | 完成态累计值、失败分类与最近失败 |
| `/dppd/rules[,AFTER_ID[,GENERATION]]` | 规则汇总和最多 64 个升序 ID |
| `/dppd/rule,RULE_ID` | 当前发布副本中指定规则的实际后端与安装记录 |

规则汇总包含 repository generation、软件/硬件/不可用规则数、本地实际对象数、
恢复状态、保存是否启用、dirty、已保存版本和失败概况。本地对象数用于解释补偿残留，
不代表网卡资源容量。

分页回应提供 `rule_ids`、`returned`、`has_more`、`next_after`。后续页应同时携带
上一页的 `repository_generation`；版本改变时重新从第一页读取。最大 uint64 规则
ID 可以精确传递，但最大 uint64 不能用作显式 repository generation，后者保留为内部 ANY。
缺少规则、非法参数、过期分页或尚未绑定实例时，DPDK 将回调错误编码为该命令下的
JSON `null`，不附带 errno；需要精确错误码时使用管理 API。

每个规则回应包含 rule ID、generation、port、实际 backend、fallback、COUNT 配置、
安装耗时是否可用、提交耗时和提交条数。COUNT 配置说明是否安装计数动作，**不是
命中数**；查询不会调用驱动 COUNT 或改变报文计数。纯软件批量更新共享整批提交耗时，
其语义见 [规则安装状态](rule_status.md)。

所有规则回应带 `publication`，标识整份镜像的发布次数。不同请求可能读到不同
publication；同一回应内部的汇总、规则数组、版本和指标来自一份完整副本。
repository generation 只约束规则内容，不能约束没有改变账本的失败指标或 flush 状态。
发布号和累计值同样只在本进程有效，不能跨重启比较。

## 并发与恢复边界

管理线程处理完控制请求后，在锁外构建完整规则副本，再用短锁交换双缓冲。
telemetry 线程只在短锁内复制需要的值，离开锁后生成 JSON，不直接读取变化中的
规则仓库、驱动对象或软件回收队列。驱动/保存操作尚未完成时仍可查询上一份完整副本，
不能把它当成正在进行请求的最终结果。管理回应可能先于副本发布，可根据 operation
计数或规则版本等待；未变化时不会反复重建。

副本按完整规则容量预分配两份，查询可以访问第一页之外的规则。构建仍使用当前
仓库和安装记录的线性查询，规则规模扩大时需另行测量管理成本；本轮没有规模或吞吐承诺。
记录指标不增加逐包路径的锁、分配或时钟调用。

启动重放的回滚失败时也注册只读 telemetry，但不启动 worker。在线隔离仍保留
期望账本，规则行的 `status_error=EUCLEAN`、backend 为 `unknown`，汇总独立显示
残留对象数；不把无法核对的实际状态报告为安装成功。`rule-metrics` 在隔离仍可读取。
退出先解绑查询入口，等待统计查询结束，再释放副本、控制层和运行实例。

## 验证记录

2026-10-03，在测试机 Ubuntu 22.04.5、内核 6.8.0-138-generic、DPDK 21.11.9、
GCC 11.4、Meson 0.61.2 下通过：

- `-Werror` 全量构建和 23/23 单元测试
- 分类、原始/补偿错误、单规则部分创建失败隔离、删除恢复失败、存储故障、整批软件分配失败测试
- telemetry 部分注册失败后重试、分配失败、严格参数、完整 uint64 ID、64 条分页、300 次并发整批发布、慢驱动期间查询及退出借用保护
- 两项新单测和事务单测的 AddressSanitizer/UBSan 检查
- 正式 ring/TAP 进程的只读查询、72 条分页、20 次并发批量更新、dirty/flush、COUNT 不变、指标重置和快照重放
- 两组在线恢复隔离和一组启动重放隔离，失败/成功重试、失败码退出、旧快照不变与残留 handle 清空
- 既有规则状态 2/4 条、ring/null 健康与线程故障、能力画像 ring/TAP、四组正式 worker 收发回归

```bash
meson setup build -Dwerror=true
meson compile -C build
meson test -C build --print-errorlogs
python3 tests/integration/rule_telemetry.py --build-dir build
sudo python3 tests/integration/rule_telemetry.py --build-dir build --tap
python3 tests/integration/recovery_isolation.py --build-dir build
```

TAP 只创建本测试自己的临时接口，验证后接口、管理 socket 和 8191 个 mbuf 全部回收；
ring 回收 1024 个 mbuf。测试共享库只加载到独立故障实例，不链接生产程序。
这些结果不证明真实物理 PMD 的故障恢复、硬件卸载性能或跨进程 residual flow 清理。
历史安装耗时分布、失败事件历史列表和跨进程 reconciliation 仍待实现。
