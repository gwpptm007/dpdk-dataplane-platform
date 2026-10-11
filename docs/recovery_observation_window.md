# 恢复计划的查询窗口监视

2026-10-11，`dppd-recovery plan PATH REVISION` 在读取任何端口之前订阅链路与 TC
事件，全部查询结束后检查已接收的事件。检测到中途变化或无法可靠接收事件时，整份
计划的候选数归零并失败退出。文件格式、命令参数及只读边界保持不变。

## 解决的具体问题

接口名称和索引是可复用的定位信息。测试先删除自建持久 TAP，再通过 `TUNSETIFINDEX`
创建同名、同索引 TAP，后续普通身份查询仍能得到坐标匹配；因此这些字段不能证明
接口始终是同一个实例。该能力可在 [Linux 6.8 TUN 实现](https://github.com/torvalds/linux/blob/v6.8/drivers/net/tun.c)
中核对，本项目已在 Linux 6.8.0-138 上实际验证。

类似地，某个端口已经读取完成后，其他程序可以修改它的规则，再恢复原来的内容。
逐端口读取或前后比较可能得到相同结果，却遗漏中途变化。现在只要监听期间收到
相关通知，变化标志就保持到本次查询结束，不因名字、索引或规则恢复原值而清除。

## 如何判断

新增一行输出：

```text
observation-window status=no-change-observed error=0 link-changed=no tc-changed=no history-covered=no writer-exclusion=no
```

| 状态 | 含义 |
| --- | --- |
| `no-change-observed` | 本次事件读取暂未发现相关变化，不等于已经证明没有任何并发修改 |
| `changed` | 观察到相关接口变化或本命名空间 TC 通知，计划失败，候选为零 |
| `unavailable` | 订阅失败、接收丢包、消息损坏、中断或未知通知，计划失败，候选为零 |
| `not-needed` | 恢复记录没有端口，不创建事件订阅，也不声称已确认清理 |

接口通知按保存的名称或索引判断相关性，命中任一项就记录变化。无关接口的普通链路
通知可以忽略。TC 的 qdisc、class、filter、chain 和 action 通知则按整个当前网络
命名空间处理：动作可能跨接口共享，不能仅过滤掉其他接口的通知后报告候选有效。
这会保守拒绝一些无关 TC 活动，调用方应在环境稳定后重新生成计划。

监视结果不覆盖各端口实际查询的状态。出现窗口错误时，端口仍可能显示坐标匹配，
但整体 `inventory-complete=no`，可核验的未删除原生记录报告 `inventory-incomplete`。
之前已读取的唯一匹配坐标仍可用于诊断，不能作为后续删除的依据。

## 接收异常的处理

事件 socket 为非阻塞模式，设置接收缓冲区，但不启用会隐藏 `ENOBUFS` 的选项。
只接受内核发送者，检查消息长度和对齐；截断、未知消息、异常结束或达到连续接收
上限都使本次窗口失效。错误一旦出现，后续读取成功也不会把这次窗口恢复为可信。

通知序号可能来自其他写入者的请求，不是本程序的连续事件编号，不能用“序号连续”
证明没有丢包。实现依据见 [TC 通知发送](https://github.com/torvalds/linux/blob/v6.8/net/sched/cls_api.c)
和 [Netlink 接收拥塞处理](https://github.com/torvalds/linux/blob/v6.8/net/netlink/af_netlink.c)。
本次同时补强现有查询解析器：先检查原始消息长度没有超过实际数据，再进行对齐计算，
避免极大长度发生整数回绕后绕过边界检查。

## 仍然不能自动删除

这个窗口只覆盖订阅开始到查询结束时的事件检查。它没有从 daemon 安装规则之前持续
运行，没有跨进程保存接口生命周期，也不阻止检查结束之后的写入。内核没有成功生成
的通知、监听启停边界和管理员操作也不能靠这次订阅补证，所以仍输出
`history-covered=no writer-exclusion=no executable=no`。

实际删除的前提仍按 [只读清理计划](recovery_plan.md) 执行。下一阶段采用以下实现边界：

1. 独立的监护进程在接口登记和首次安装之前建立会话，业务进程崩溃时继续保留事件
   观察；会话身份、失效原因和恢复记录关联，监护进程死亡或历史缺口不能事后补猜
2. 接口重建、事件丢失、环境变化或监护会话更换都使原会话不可自动恢复，测试要覆盖
   监护进程退出、业务进程退出及双方退出的不同窗口
3. 明确受控网络命名空间和允许写入者，业务安装与恢复操作使用同一串行入口或经过
   验证的交接流程；单纯增加文件锁、重复查询或保存名字和索引不能替代写入隔离
4. 在连续性证据和写入约束同时成立后，再加入精确删除、删除结果落盘和失败后续接

后续已落地 [独立接口监护与会话证据](recovery_monitor.md)，支持业务退出后继续观察和
持久失效诊断，[业务握手与安装前后检查](recovery_monitor_handshake.md) 也已支持显式启用。
受控写入者和恢复交接仍未实现，不能把当前观察记录或新鲜回复当作写入隔离已经成立。

## 验证

系统 DPDK 21.11.9 与原生标识补丁构建均完成 `-Werror` 编译和 **32/32 单测**。
监视、计划和查询解析三项测试通过 AddressSanitizer/UBSan，覆盖接收错误保持、长度
回绕、同批多条消息、事件风暴上限、非内核来源、丢包及资源释放。

真实 TAP 测试在两次查询之间暂停测试版离线工具，然后执行受控修改：

- 无变化及无关接口的链路通知仍保留四个原生候选
- 相关接口属性修改后恢复、过滤器修改后恢复、动作单独修改后恢复均撤销候选
- 无关测试接口的 TC 活动按命名空间范围使计划失效
- 只对事件 socket 注入 `ENOBUFS`，正常 GET 保持成功，计划仍拒绝
- 自建 TAP 删除后复用原名称与原索引，本次窗口失效，后续坐标查询不能证明历史连续

所有场景核对恢复文件和业务快照没有变化。暂停和故障包装只存在于独立测试入口，
正式工具没有这些开关。旧格式、版本和锁检查、原生/普通模式六个创建窗口一并回归：

```sh
sudo python3 tests/integration/tap_owner_cookie.py --build-dir build-native
sudo python3 tests/integration/recovery_inspect.py --build-dir build
sudo python3 tests/integration/recovery_attempt.py --build-dir build-native --native
sudo python3 tests/integration/recovery_attempt.py --build-dir build
```
