# M3 AGE 闲置老化

AGE 用来回收一段时间没有再被报文命中的规则。比如 `age:5` 表示闲置五秒后可以回收，
不是“创建五秒后无论有没有流量都删除”。持续命中的规则会不断延后闲置起点。

当前已经接通规则表示、真实 DPDK 适配器、正式管理查询、生产主循环的轮询删除、
快照保存和独立功能模拟。现有机器没有可用于此项验收的真实 SmartNIC，模拟结果
不能证明具体 PMD 或 ASIC 已经正确执行 AGE。整个 M3 仍按 [阶段验收](m3_acceptance.md)
记录未完成项。

## 规则中的 AGE

规则 IR 使用 `DPPD_ACTION_AGE`，`conf.age_timeout_seconds` 保存完整秒数，合法范围为
**1 到 16777215 秒**，对应 DPDK AGE 超时的 24 位范围。零、负数、越界值和重复 AGE
都会拒绝，不能把超大值截断后交给驱动。

AGE 是附加动作，不决定报文去向。规则仍须保留唯一的末尾去向动作，例如明确的
represented-port 输出。transfer 模板按下面的顺序构造：

```text
可选 COUNT → 可选 AGE → 明确的目的端点
```

COUNT 和 AGE 可以分别使用，也可以一起使用。COUNT 的包数和字节数不会代替 AGE
结果，查询计数也不会让规则续期。没有 AGE 的规则不会因为 COUNT 长时间不变而被
自动删除。双向图的两条规则分别命中、分别计时和回收，不承诺同时过期或原子删除。

带 AGE 的规则没有等价软件回退。PMD 拒绝完整规则或不提供 AGE 查询时，系统保留
明确错误，不用 CPU 时间或计数变化猜测硬件是否闲置。模拟器中的时钟只存在于独立
测试目标，正式 `dppd` 不会使用它冒充硬件 AGE。

## 生产程序怎样回收

正式 `dppd` 的管理主循环调用 `dppd_control_age_poll()`。正常情况下两次扫描至少
间隔 100 毫秒，每次最多读取 64 条账本规则，只查询其中带 AGE 的规则；更大的表
会按 ID 分页继续。这是有限预算的主动查询，不是中断或事件触发的硬件回收服务。

控制服务拿到的是当前账本中的 ID、精确 generation 和安装端口，再定位对应的
驱动 handle。只有驱动明确返回 `aged=yes` 才调用正式删除接口，删除时仍携带同一
精确 generation。这样不会因为旧一次查询就误删同 ID 的新版本。

| 观察结果 | 控制服务的行为 |
| --- | --- |
| 驱动报告尚未到期 | 保留规则，后续继续查询 |
| 驱动明确报告到期 | 删除该精确版本，推进账本并保存快照 |
| 查询失败或缺少驱动能力 | 保留规则，记录查询错误，稍后重试 |
| 驱动删除失败 | 保留可定位的 handle 和账本记录，记录删除错误后重试 |
| 删除后快照保存失败 | 保留 dirty 状态，停止后续自动删除，等待存储修复 |
| 已在恢复隔离中 | 停止自动删除，不绕过既有恢复流程 |

主循环的调度、表大小、PMD 响应和保存快照都需要时间，因此 100 毫秒不是到期删除
的时延承诺。规则也不会在 ASIC 超时的同一瞬间完成账本和磁盘更新。

出现查询或删除错误后，后续扫描至少间隔一秒。错误跨分页保留，只有完整一轮扫描
没有错误，才清除当前 `last-error`；“还没到下一次轮询时间”不会清掉错误。累计错误
次数仍保留，清掉当前错误不等于抹掉历史。

`expired` 只累计正式删除成功且保存成功的回收。若删除已生效但保存失败，不能根据
这个计数断言对象还存在，应同时检查当前规则、`persistence-status` 和 dirty 标志。
修复存储后使用 `persistence-flush` 保存当前完整账本，再核对结果；不能盲目重建或
重启，造成旧磁盘快照中的规则重新出现。

## 正式命令

当前管理协议为 **v25**，daemon、模拟器和 `dppctl` 需要配套更新。旧客户端会被拒绝，
不能把历史 v24 的结构体直接送给当前服务。

transfer 命令增加可选的 `age:SECONDS`：

```text
transfer-plan ID INSTALL SRC DST TEMPLATE oneway|bidir [count] [priority:N] [age:SECONDS]
transfer-apply ID INSTALL SRC DST TEMPLATE oneway|bidir EXPECTED [count] [priority:N] [age:SECONDS]
age-status
rule-age RULE_ID EXPECTED_GENERATION
```

`transfer-apply` 的 `EXPECTED` 是 plan 返回的全局账本版本，不能写 `any`。AGE 自动
删除也会推进这个版本；如果在 plan 与 apply 之间发生回收，apply 可以返回 `ESTALE`，
此时应重新 plan。`rule-age` 的版本参数则是该条规则的 generation，允许精确版本或
`any`，不能把两个版本含义混用。

`age-status` 只读取控制服务状态，不访问 PMD，也不触发一次回收：

- `tracked`：当前账本中配置了 AGE 的规则数
- `polls`、`queries`：自动扫描轮次与其中发起的 AGE 查询数
- `expired`：自动回收成功的规则数
- `query-errors`、`delete-errors`：两类自动处理失败的累计次数
- `last-error`、`last-rule`：尚待完整一轮成功核对的错误及最后出错规则

`rule-age` 会向驱动查询指定的当前版本，但自身不执行删除。它输出 `aged=yes|no`
和 `idle-seconds`。驱动没有提供有效闲置秒数时显示 `unknown`，不能把它读作零秒；
`aged` 是独立结果，闲置秒数未知时仍可能明确到期。没有 AGE 返回 `ENODATA`，旧版本
返回 `ESTALE`，规则不存在返回 `ENOENT`，驱动能力缺失返回 `ENOTSUP`。

## 快照 v4 与重启

包含 AGE 动作的规则快照写为 **v4**，保存超时配置、安装端口和规则版本，继续沿用
CRC、私有文件权限和原子替换的保存流程。没有 AGE 的 IPv4 规则仍可保存为 v2，
包含 IPv6 且没有 AGE 时仍可保存为 v3；当前加载器兼容 v2/v3/v4。不能把含 AGE 的
文件改一个版本号就伪装成旧格式，旧版本程序也不应读取新的 v4 文件。

快照不保存驱动 handle、最近命中时间、当前 AGE 结果、COUNT 或模拟时钟偏移。
重启后，仍在 desired 账本中的规则会创建新对象并重新开始闲置计时。它保留的是
“这条规则需要五秒闲置超时”的配置，不保留“重启前已经闲置四秒”的运行状态。
具体 PMD 的重新创建和计时行为仍需实机核对。

已经完成删除并成功保存的过期规则，不在恢复快照中，因此不会重启复活。若进程在
保存失败后被强制结束，旧快照仍可能包含它；这属于已明确报告的持久化不确定状态，
不能声称跨崩溃的到期截止时间已经得到保证。

## 不接网卡的 AGE 演练

先按 [模拟服务说明](m3_simulation.md) 构建测试工具，再在第一个终端启动一个新的
专用目录：

```sh
python3 scripts/simulate_m3.py serve --build-dir build --directory /tmp/dppd-m3-age-demo
```

第二个终端以新目录首次运行、plan 实际返回全局版本 0 为例：

```sh
./build/dppctl --socket /tmp/dppd-m3-age-demo/ctl.sock transfer-plan 200 0 0 2 uplink-vf oneway count age:5
./build/dppctl --socket /tmp/dppd-m3-age-demo/ctl.sock transfer-apply 200 0 0 2 uplink-vf oneway 0 count age:5
./build/dppctl --socket /tmp/dppd-m3-age-demo/ctl.sock rule-age 200 any
python3 scripts/simulate_m3.py advance --io-socket /tmp/dppd-m3-age-demo/io.sock 4
python3 scripts/simulate_m3.py inject --io-socket /tmp/dppd-m3-age-demo/io.sock \
  --source 0 --frame 02000000000202000000000188b5
python3 scripts/simulate_m3.py advance --io-socket /tmp/dppd-m3-age-demo/io.sock 4
./build/dppctl --socket /tmp/dppd-m3-age-demo/ctl.sock count 200 any
./build/dppctl --socket /tmp/dppd-m3-age-demo/ctl.sock age-status
```

演练使用真实单调时间加上进程内偏移。上面的注入若在首次闲置超时前命中，就会刷新
起点；第二次推进四秒后仍应保留规则，再闲置到五秒才回收。手动敲命令花费的真实
时间同样会计入，因此不要把这个示例当作暂停时钟。需要重复、确定的边界测试时，
运行完整 `demo`，由脚本连续完成步骤并核对结果。

```sh
python3 scripts/simulate_m3.py demo --build-dir build
```

可通过独立模拟通道组合故障与时钟推进：

```sh
python3 scripts/simulate_m3.py fault --io-socket /tmp/dppd-m3-age-demo/io.sock age-query 1 --advance 1
python3 scripts/simulate_m3.py fault --io-socket /tmp/dppd-m3-age-demo/io.sock remove 1 --advance 5
```

两条是独立示例，需要在相应规则仍存在时分别使用。组合命令先完整检查操作、次数、
秒数和溢出，再在同一次事件处理中设置故障、推进和轮询，避免后台先消费故障。
对应的私有文本消息为 `fault age-query 1 advance 1`。响应携带本次处理后的错误、
installed 和 desired 快照，方便看见一次失败保留对象的状态；普通 `fault` 和
`advance` 仍可分别使用。`clear-faults` 只清计划，不清规则、计数或控制服务的历史。

模拟 `query` 故障只影响 COUNT，`age-query` 故障只影响 AGE。AGE 查询或回收仍有错误、
持久化 dirty 或处于任意恢复隔离状态时，模拟器会保守拒绝报文注入。模拟时间推进
不改变系统时间，也不向真实网卡发包。超出原生 24 位范围的闲置秒数显示未知，
到期判断仍使用完整闲置时长。

## 本轮验收结果

2026-10-18，AGE 本轮的三套最终构建完成同版本核心验收：

| 构建 | 单测 | 核心软件回归 | 正向功能模拟 |
| --- | --- | --- | --- |
| 135 系统 DPDK 21.11.9 / GCC 11 | 57/57，无失败、跳过或超时 | 四步通过 | 16 组通过 |
| 135 ASan/UBSan | 57/57，无失败、跳过或超时 | 四步通过 | 16 组通过，无 sanitizer 错误 |
| 134 私有 DPDK 21.11.9 / GCC 13.2 | 57/57，无失败、跳过或超时 | 四步通过 | 16 组通过 |

三份统一报告、软件回归和模拟报告的 228 个源码文件摘要逐项一致，并与本地源码核对。
摘要表按键排序并紧凑 JSON 编码后的 SHA-256 为：

```text
721d15cafdb4d8bd1c3676f00f8e1b1eb24b28d80f317ac461bf642c30ee6270
```

本轮完整覆盖原有正向演练及 AGE 的自然闲置、无 COUNT 的 AGE、无 AGE 规则保留、
命中续期、公开查询、时钟坏输入与溢出、两类查询故障独立、删除重试及过期快照重启。
180 个 C 源码和头文件通过格式检查；Python 语法、注释结尾和文档链接由统一检查核对。
三套只执行四步核心回归，没有重新计入历史八组完整 TAP，不能据此更新完整 M2 TAP
或 M3 实机验收结论。135 与 134 验收后无遗留测试进程。报告复制件位于本地
`build/native-check/m3-age-artifacts/`，不提交构建产物。ASan/UBSan 关闭 LeakSanitizer，
worker 的 mbuf 归还由内存池记录另行核对。

## 尚未完成的硬件与后续能力

这些结果没有证明 mlx5/BlueField 或物理非 mlx5 PMD 的 AGE 支持、ASIC 定时精度、
硬件报文续期、硬件资源回收、CPU 旁路或性能。当前也没有实现 AGE 事件通知、
indirect/shared action 或设备复位后的完整 reconciliation；后续阶段分别实现并验收，
不能因为 DPDK 头文件中有相关接口就计为完成。

主要实现见 [规则 IR](../include/dppd/rule.h)、[AGE 控制接口](../include/dppd/age.h)、
[轮询与精确删除](../lib/control/age.c)、[DPDK 适配器](../lib/offload/rte_flow.c)、
[快照](../lib/control/persistence.c) 和 [模拟器](../tests/integration/transfer_sim.c)。
