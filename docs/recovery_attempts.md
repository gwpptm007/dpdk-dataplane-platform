# 逐次安装记录与 TAP 候选线索

本文保留逐次记录 v3 阶段的字段和验证。当前新建恢复文件为 [v4](tap_owner_cookie.md)，
增加可选的本地 TAP 原生随机标识，兼容 v1/v2/v3；下文的普通模式与候选坐标边界仍适用。

2026-10-09，在端口保护和 TAP 只读核对之上，新增可持久保存的逐次安装记录。
现在能回答“哪个规则版本曾经尝试安装、驱动是否返回成功、删除是否成功、当时观察到
哪个新增 TC 对象”。这些记录仍不证明当前内核对象属于本项目，不允许据此自动删除。

管理协议保持 v15，规则快照保持 v2，独立恢复文件升级为 v3。仍使用现有
`--recovery-path` 开关和 `dppd-recovery show/inspect`，不增加默认开启的设备操作。

## 安装和清理顺序

1. 先按原流程核对端口与 TAP 内核身份，保存本轮可能安装过规则的端口标记
2. 为本次驱动调用分配唯一尝试编号，保存规则 ID、generation、安装端口和 `intent`，完成 fsync 后才允许创建
3. 对 TAP 读取创建前的本地 TC 列表，调用真实驱动，再读取创建后的列表
4. 保存真实创建返回值及观察结果，完成 fsync 后才向事务报告创建成功
5. 普通删除、事务回滚、隔离重试和退出清理都按原 handle 调用驱动，再保存删除结果

尝试编号与业务规则 ID 分开。同一业务版本因补偿而重新创建，也会获得新编号。
编号在同一个恢复文件内持续递增，清理确认后仍保留高水位，耗尽时拒绝新尝试。
删除重建文件、复制旧文件或人为回退不属于编号保证范围。

| phase | 记录的事实 | 后续行为 |
|---|---|---|
| `intent` | 意图已经持久保存，创建结果尚未成功保存 | 可能尚未创建，也可能内核已有对象，必须外部核对 |
| `created` | 驱动明确返回创建成功 | 保留精确尝试和删除线索 |
| `create-failed` | 驱动返回创建错误 | 不推断没有残留；若留下 handle，仍按原事务回滚 |
| `removed` | 同一次尝试的真实 handle 删除成功且结果已持久保存 | 该记录槽位可以复用 |

`create-error` 保留驱动创建错误，`remove-error` 保留最新删除错误。
删除失败不改变原阶段，也不允许覆盖该槽位；删除成功才标记 `removed`。
正常退出只有所有逐次记录都为 `removed`，并满足原有清理条件时才自动标记 clean。
创建失败且没有返回 handle 时，没有可验证的成功删除结果，正常退出也保留待核对标记。

记录容量为所有端口合计 **256 个槽位**，只复用已成功删除的记录，不覆盖仍可能存在的
对象。没有可复用槽位时返回 `ENOSPC`，此次不调用驱动；删除已有规则可以腾出槽位。
这是启用恢复保护后的额外控制面容量，不是网卡容量。软件规则不占槽位，未启用
`--recovery-path` 时不受此限制。`removed` 记录可能被复用，因此它不是完整历史审计日志。

## 写盘故障的处理

意图同步失败时不调用驱动。创建已成功而结果同步失败时，真实 handle 仍在 backend，
创建请求向事务返回错误并执行原回滚，不能因为记录失败而丢失对象。

删除结果同步失败时，已经完成的驱动删除仍作为删除成功处理，继续释放真实 handle；
恢复保护故障保持锁存，后续硬件创建被拒绝，退出不能把文件标成 clean。记录失败不会
阻止其余已知 handle 的尽力清理。原始驱动创建失败时保留原始错误，不被记录错误覆盖。

这类故障不会新增 health 状态位，也不会阻止软件规则操作；`ready=yes` 不能证明后续
受保护的硬件创建可以成功。持久化失败会在 daemon 日志中报告，并在退出时保持失败。
处理仍遵循 [外部核对与显式确认](recovery_guard.md#查看线索并恢复)。

## 如何读取线索

旧进程结束后使用同一恢复文件和 `show` 返回的当前修订号：

```bash
sudo ./build/dppd-recovery show /var/lib/dppd/hardware.recovery
sudo ./build/dppd-recovery inspect /var/lib/dppd/hardware.recovery 6
```

这里的 `6` 仅是示例。安装意图、创建结果和每次删除结果都会推进修订号，不能沿用
旧版按端口数量计算修订号的方法。工具仍持排他锁，版本不符返回 `ESTALE`。

`show` 新增以下类型的行：

```text
attempts=2 last-attempt=2 capacity=256
attempt=1 rule=910 generation=1 port=0 phase=created create-error=0 remove-error=0 evidence=single-addition observation-error=0 ownership=unproven
candidate attempt=1 parent=00010000 handle=00123456 chain=0 priority=11 protocol=0003 kind=flower
```

`attempts` 是当前保留的记录数，可能包含已删除但尚未复用的槽位，不是残留规则数量。
`evidence` 的含义如下：

| evidence | 含义 |
|---|---|
| `none` | 尚未保存观察结果 |
| `single-addition` | 创建前旧坐标全部仍在，且创建后恰好多出一个不同坐标 |
| `ambiguous` | 没有新增、多个新增、旧对象消失、重复坐标等情况，不能选出单一候选 |
| `unavailable` | 驱动不支持检查或内核查询失败，具体错误见 `observation-error` |

观察失败只影响证据等级，不把真实创建成功改成失败，也不伪造空列表。
安装耗时仍只计驱动 create，前后查询和记录同步不计入原驱动耗时分布。

`inspect` 继续显示实际 TC 规则，并为保留的尝试输出 `correlation` 行。已有候选坐标
当前存在时显示 `coordinate-present`，不存在显示 `coordinate-absent`，检查失败显示
`inspection-unavailable`；没有候选显示 `no-candidate`，已删除记录显示 `removed-record`。
所有情况均为 `owner=unknown`，查询不会修改记录、确认清理或删除任何内核对象。

## 为什么还不能自动删除

前后列表是分步读取，其他程序可能在同一时间增删规则。即使恰好新增一条，也只能说明
时间上相关。同一坐标的 match/action 还可能被修改；原对象删除后，另一个程序也可能
使用相同的 parent、handle、chain、priority、protocol 和 kind 创建新对象。
这里不保存进程地址，不读取私有 `rte_flow` 内存，不按优先级推断归属。

当前 DPDK 21.11 TAP 驱动的公开 flow 操作没有可回读的逐对象所有权接口，见
[TAP flow 操作实现](https://github.com/DPDK/dpdk/blob/v21.11/drivers/net/tap/tap_flow.c)。
上述持久记录是后续归属核验的基础，不是归属证明本身。下一步需要驱动在同一次创建
操作中写入可回读的所有权标识，并核对规则内容、接口实例及全部作用范围，再设计精确删除。
现有 `remote=` 范围、其他 PMD、自动确认和设备复位仍不在此阶段内。

## v3 文件布局和旧文件

v3 固定 33856 字节：前 9248 字节沿用 v2 的头部、快照路径和 16 个端口槽布局；
数值版本变为 3，总长度和 CRC32 覆盖完整 v3 文件。后接 32 字节逐次记录头和
256 个 96 字节槽位，所有整数为小端，错误码按 32 位有符号整数保存。

记录头前 8 字节为 `last-attempt`，接着 4 字节为记录数，其余为零。
槽位字段相对偏移如下：

| 偏移 | 字节数 | 字段 |
|---|---|---|
| 0 / 8 / 16 | 各 8 | 尝试编号 / 规则 ID / generation |
| 24 | 2 | 安装端口 |
| 26 / 27 | 各 1 | 阶段 / 证据等级 |
| 28 / 32 / 36 | 各 4 | 创建错误 / 删除错误 / 观察错误 |
| 40 / 44 / 48 | 各 4 | 候选 parent / handle / chain |
| 52 / 54 | 各 2 | 候选 priority / protocol |
| 56 | 32 | 候选 kind，NUL 终止 |
| 88 | 8 | 零保留位 |

无单一候选时候选区域全零。解码核对阶段与错误组合、端口引用、编号唯一性、编号上限、
保留位、未使用槽位、字符串及完整规范编码；损坏不能变成空记录。

v1/v2 待核对文件原样读取且不升级：v1 没有内核身份，v2 有接口身份但没有逐次线索。
旧文件的外部确认仍写回原格式。只有干净 v1/v2 由新 daemon 打开时才原位升级 v3，
推进修订号并保持原文件锁。离线查询不升级，旧程序不能读取 v3，不能直接降级运行。

## 验证

Ubuntu 22.04.5 / DPDK 21.11.9 / GCC 11.4 / Meson 0.61.2：

- `-Werror` 全量构建、28/28 单测通过
- 意图/结果/删除同步故障、原始错误保留、真实 handle 回滚回收、256 槽容量、删除后复用和编号耗尽通过
- 重新计算 CRC 后的坏阶段、错误码、端口引用、字符串、保留位和空槽内容仍被拒绝
- v1/v2 只读兼容及干净文件升级通过，旧待确认文件不自动升级
- 真实 TAP 的候选坐标与内核列表一致，外部规则和同坐标重建规则均保持归属未知
- 三个独立进程场景通过：创建前 SIGKILL、内核创建后返回前 SIGKILL、创建失败且无 handle；均保留不确定状态和原快照
- ring/TAP 恢复保护、三组原隔离流程、健康及 worker 注册失败回归通过
- 逐次记录、检查器、恢复保护、硬件 backend、控制服务五项 AddressSanitizer/UBSan 通过

```bash
meson setup build -Dwerror=true
meson compile -C build
meson test -C build --print-errorlogs
sudo python3 tests/integration/recovery_attempt.py --build-dir build
sudo python3 tests/integration/recovery_inspect.py --build-dir build
python3 tests/integration/recovery_guard.py --build-dir build
sudo python3 tests/integration/recovery_guard.py --build-dir build --tap
```

暂停夹具只由测试显式预加载，不链接生产程序。验证仅创建和删除自有 TAP，不绑定或
复位物理网卡，也没有完成物理 NIC/SmartNIC 的归属或自动恢复验收。
