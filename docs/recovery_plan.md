# TAP 只读精确清理计划

2026-10-11 新增 `dppd-recovery plan PATH REVISION`，把恢复记录与当前内核观察整理成
逐条候选、拒绝原因和删除前提。恢复文件仍为 v5，业务快照仍为 v2，管理协议仍为 v15。
本命令只生成诊断结果，所有输出均保持 `executable=no`，没有执行删除的选项。
后续已增加 [查询窗口事件监视](recovery_observation_window.md)，读取期间发生变化或
事件接收不完整时整份计划不产生候选，仍不证明历史接口连续性或排除后续写入。

## 使用方法

先停止占用恢复文件的 daemon，在原主机、原网络命名空间中读取当前 revision：

```sh
sudo ./build/dppd-recovery show /var/lib/dppd/hardware.recovery
# 将 23 替换为上面实际显示的 revision
sudo ./build/dppd-recovery plan /var/lib/dppd/hardware.recovery 23
```

离线工具全程持有恢复文件排他锁。版本不符时，在读取内核之前拒绝；daemon 正在持锁
时返回忙。计划不修改恢复文件、业务快照或内核规则，也不解除启动保护。
候选需要此前启用了 [TAP 原生标识](tap_owner_cookie.md) 和
[创建前预期内容记录](tap_rule_verification.md)，不能事后从内核补造这些证据。

## 输出怎样看

| 行前缀 | 内容 |
| --- | --- |
| `plan` | 输出版本 1、文件 revision、格式、端口数、尝试数和观察是否完整 |
| `scope=` | 仅登记端口的本地 multiq/ingress，非原子快照、未确认清理、不可执行 |
| `observation-window` | 查询期间的变化和接收异常，历史连续性与写入隔离仍未覆盖 |
| `prerequisites` | 接口实例连续性、检查到删除之间的并发保护、标识真实性和删除实现的缺口 |
| `plan-port` | 持久保存的接口索引、名称、启动标识、命名空间，以及本次查询状态 |
| `plan-attempt` | 尝试编号、业务规则和版本、安装端口、阶段、决定、主原因及核验结果 |
| `plan-object` | 唯一匹配对象的实际端口、完整 TC 坐标和完整 cookie |
| `plan-summary` | 候选、拒绝、排除、缺少逐次记录的端口和未关联过滤器数量 |

`plan-port` 中的身份来自恢复文件；只有查询状态才能说明本次能否在该身份下观察。
`plan-object` 也会列出唯一匹配但被拒绝的对象，必须结合对应 `plan-attempt` 阅读。
`token-matches` 统计所有成功读取的登记端口，不限于原安装端口；观察不完整时这个数字
只是已读部分。未关联过滤器只计数，不按相似坐标纳入候选，也不默认属于外部程序。

`decision` 有三种值：

- `candidate`：本次受支持范围内，唯一标识位于原端口，内容和范围均符合预期
- `rejected`：证据不足、相互矛盾或核验不符，不能进入候选
- `excluded`：记录已成功删除，本次已读部分未发现同标识，不安排任何操作

即使出现 candidate，也仍是待进一步核实的对象。零候选、全部 excluded、接口已消失、
甚至恢复文件本来就是 clean，都不会让该命令输出清理完成。

## 分类规则

| 主原因 | 含义 |
| --- | --- |
| `observed-match` | 标识、原端口、内容与当前范围均匹配，可列为观察候选 |
| `removed-record` | 已记录成功删除，排除本条，不能据此确认整个端口已干净 |
| `removed-token-present` | 已删除记录的标识再次出现，保留矛盾，不接管该对象 |
| `missing-token` | 普通模式或旧记录没有原生标识，坐标一致不能替代它 |
| `missing-expected-content` | 旧 v4 等记录没有创建前预期内容，不从当前对象反推 |
| `inventory-incomplete` | 任一登记端口查询不完整或事件监视失败，整份计划不产生候选 |
| `token-absent` | 所有已登记范围读取完成，未找到标识，仍不自动确认清理 |
| `token-ambiguous` | 同一标识出现在多个对象，包括跨端口复制 |
| `wrong-port` | 标识唯一，但仅出现在其他登记端口 |
| `content-mismatch` / `content-unsupported` | 匹配条件或动作改变，或者包含无法解释的字段 |
| `scope-mismatch` / `scope-unsupported` | 挂载位置改变，或者当前作用范围无法解释 |
| `shared-action` | 动作被多个过滤器绑定，不能视为独占 |

主原因先检查已删除状态，再依次检查证据是否齐全、观察完整性、标识位置、内容和范围，
全部通过才给出 observed-match。内容和范围分别显示核验结果，尚未核验的项显示 unchecked。
共享动作若在多个被观察对象上导出同一个 cookie，先报告 token-ambiguous。
`intent` 和 `create-failed` 不直接排除候选：进程可能在创建后、结果落盘前退出，驱动
也可能创建了部分对象后返回失败；此时仍要核对创建前保存的标识和预期内容。

旧 v1/v2 没有尝试明细的端口显示 `reason=no-attempt-record`，汇总中保留
`ports-without-attempts`，不会把“没有逐条记录”解释成“没有残留”。旧文件保持原格式。

退出码 0 只表示完整生成本次只读观察计划，不表示所有条目都是 candidate，更不表示
可以删除。旧 revision、锁冲突、查询错误、环境或接口身份不符会失败退出；查询失败
时仍尽量输出各端口状态，候选数为零。拒绝原因本身不改变退出码。

## 真正删除前还缺什么

当前 plan 始终列出以下缺口：

1. **接口实例连续性**：相同 boot、命名空间、名称和索引，仍不足以证明接口从创建
   至今没有销毁重建；文件 revision 也只约束恢复记录
2. **检查到删除之间的并发保护**：恢复文件锁不能阻止其他 TC 写入者，重复检查也不能
   把后续删除变成条件原子操作，保存计划后更不能直接照着坐标执行
3. **标识真实性边界**：cookie 可被有权限的程序复制，必须明确受控环境和写入者范围，
   不能把随机 cookie 当作防伪凭证
4. **删除与失败恢复实现**：尚无跨进程精确删除入口、删除事件落盘和失败后续接流程

查询期间的事件丢失、中途修改和同名同索引重建已验证为拒绝候选。后续已增加
[独立接口监护与会话证据](recovery_monitor.md)，业务退出后仍可观察接口变化；该证据
尚未接入本命令。[业务握手、会话绑定与活性检查](recovery_monitor_handshake.md) 已完成，
下一步验证受控写入者与恢复交接，再将证据接入本命令和后续删除流程。
remote、其他 PMD 和未登记端口不在本计划的过滤器枚举范围。
目前完成外部清理后仍使用 [现有精确版本确认流程](recovery_guard.md#查看线索并恢复)。

## 验证记录

Ubuntu 22.04.5、Linux 6.8.0-138、DPDK 21.11.9 环境，系统驱动与独立补丁构建均使用
`-Werror`，各 **31/31 单测通过**。计划、内核解析和预期内容三项相关单测通过
AddressSanitizer/UBSan。真实 TAP 仅使用测试自建接口，验证命令如下：

```sh
sudo python3 tests/integration/tap_owner_cookie.py --build-dir build-native
sudo python3 tests/integration/recovery_inspect.py --build-dir build
sudo python3 tests/integration/recovery_attempt.py --build-dir build-native --native
sudo python3 tests/integration/recovery_attempt.py --build-dir build
```

覆盖四种规则的实际候选坐标、跨端口复制和移位、已删除标识重新出现、外部规则、
动作和匹配条件变化、位置变化、未知字段、共享动作、无标识同坐标替换、v1/v2/v4
兼容、锁冲突、版本过期、非法参数、网络命名空间和接口身份不符、接口消失与空记录。
创建前、创建后和无 handle 失败三个窗口分别验证原生及普通模式；核对生成计划后
持久文件不变，原生规则变化场景还逐次对照 multiq/ingress 的真实 TC 列表不变。
