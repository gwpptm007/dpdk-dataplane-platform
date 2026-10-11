# TAP 规则内容与作用范围核验

2026-10-11，在已有原生 cookie 的基础上，新增创建前保存预期内容、创建后回读核验和
离线核验。恢复文件升级为 v5，管理协议仍为 v15，业务规则快照仍为 v2。

## 核验什么

原生模式仍需 `--state-path`、`--recovery-path`、`--tap-owner-cookie` 和
[独立 TAP 驱动补丁](../patches/dpdk-21.11.9-tap-owner-cookie.patch)。默认模式不变。
支持本地 ingress 的 ETH / IPv4 / UDP / TCP，以及单个 DROP 或 QUEUE。

创建前从业务规则生成规范化预期值，与随机 cookie、尝试编号一次写入并 `fsync`，
成功后才进入驱动。创建后必须找到唯一 cookie，同时满足：

- `content=match`：EtherType、IP 协议、源/目的地址及掩码、源/目的端口及掩码、
  DROP/QUEUE 类型和队列编号都相同。掩码忽略的位先清零，不影响规则语义。
- `scope=match`：挂载点、chain、priority、外层 protocol 与预期一致，动作只有一个，
  动作引用数和绑定数均为 1，未观察到硬件作用范围或未知行为字段。

已知字段采用严格允许列表。额外 MAC/VLAN 匹配、额外动作、随机动作、未知属性编号、
硬件执行标志、不能解释的动作选项均不能得到匹配结论。动作绑定数大于 1 时显示
`scope=shared-action`。动态计数和时间等不参与行为比较。

驱动报告成功，但标识、内容或范围不能核实时，事务使用本次返回的真实 handle 回滚，
不发布业务快照。恢复记录仍保存原始驱动创建结果，核验失败不会伪造成驱动失败。
本次同时修复了补丁对普通 `remote=` 内部空动作列表的处理；原生模式仍拒绝 remote。

## 离线输出

```text
correlation attempt=1 rule=980 generation=1 port=0 phase=intent evidence=none status=token-present owner=token-match content=match scope=match
```

即使进程在内核创建完成、创建结果尚未落盘时退出，`intent` 阶段也能使用此前保存的
预期内容核验。各字段独立表达事实：标识匹配不意味着内容或范围仍匹配。

| 字段值 | 含义 |
| --- | --- |
| `match` | 本次读取的受支持字段与预期相同 |
| `mismatch` | 已识别字段与预期不同 |
| `unsupported` | 含未知条件、选项、字段组合或无法解释的范围 |
| `shared-action` | 动作被多条过滤器绑定，范围不能视为独占 |
| `unchecked` | 旧记录或普通模式没有预期内容，或者记录已经 removed |
| `unavailable` | v5 原生记录存在，但没有唯一可核验对象或查询失败 |

`inspect` 仍只发送 GET，持锁核对精确 revision，不写业务快照和恢复文件，也不删除
过滤器。退出码 0 只说明本次读取完成；调用方必须检查每条 correlation 的字段，不能
把退出码当作全部规则验证通过。整体继续输出 `atomic-snapshot=no cleanup-confirmed=no`。

## v5 文件布局与兼容

固定长度 **54336 字节**，原有头部、快照路径、端口信息和尝试元信息不变。
尝试区从偏移 9280 开始，共 256 槽，每槽由 v4 的 112 字节扩展为 **176 字节**。
前 112 字节保留原含义；末尾 64 字节保存预期内容：

| 槽内偏移 | 长度 | 含义 |
| --- | --- | --- |
| 112 | 4 | 内容版本，当前原生模式为 1，普通模式为 0 |
| 116 / 118 | 各 2 | EtherType / QUEUE 编号 |
| 120 / 121 | 各 1 | IP 协议 / 动作类型，1 为 DROP、2 为 QUEUE |
| 122 | 2 | 保留为零 |
| 124 / 128 / 132 / 136 | 各 4 | 源地址、源掩码、目的地址、目的掩码，网络字节序 |
| 140 / 142 / 144 / 146 | 各 2 | 源端口、源掩码、目的端口、目的掩码，网络字节序 |
| 148 / 152 | 各 4 | parent / chain |
| 156 / 158 | 各 2 | priority / 外层 protocol |
| 160 | 16 | 保留为零 |

网络字段之外的多字节整数使用小端。规范编码与 CRC 均须有效。v5 中非零 cookie 必须
同时具有版本 1 的有效预期内容；普通模式必须同时为零，不能把缺失字段当作有效规则。

DPDK 21.11.9 本地 TAP 的外层 protocol 是 `ETH_P_ALL=3`、parent 为 `1:`、chain 为 0。
priority 按 `(group << 12) | (rule.priority + max_rx_queues + 1)` 计算，队列上限来自
设备信息。原生模式拒绝偏移后越过低 12 位的 priority，避免组号混叠；16 队列构建的
业务 priority 上限是 4078，group 上限是 15。普通模式不增加此限制。

仍可读取 v1/v2/v3/v4。待核对的旧文件保持原格式，离线查看不升级；旧 v4 cookie 可
匹配对象，但内容和范围显示 unchecked。旧文件确认干净后，新 daemon 才在同一 inode
升级为 v5，推进 revision 并保留最后尝试编号。旧版程序不能读取 v5。

## 验证与边界

在 Ubuntu 22.04.5、Linux 6.8.0-138、DPDK 21.11.9 的自有 TAP 上验证。独立驱动包含
`net/tap,net/ring,net/null,mempool/ring`，安装在用户私有目录，不覆盖系统 DPDK。
系统与独立驱动构建均使用 `-Werror`，各 30/30 单元测试通过；七项相关测试通过
AddressSanitizer/UBSan。ring/TAP 恢复保护、三个进程内隔离场景及单/双队列健康回归通过。

```bash
meson test -C build --print-errorlogs
python3 tests/integration/tap_owner_cookie.py --build-dir build-native
python3 tests/integration/tap_owner_cookie.py --build-dir build-native --alter-content
python3 tests/integration/tap_owner_cookie.py --build-dir build-native --ordinary-remote
python3 tests/integration/recovery_attempt.py --build-dir build-native --native
```

真实场景覆盖 ETH/IPv4/UDP/TCP、地址掩码与端口、DROP/QUEUE、正常删除、同标识内容变化、
未知 MAC 条件、共享动作、位置变化、重复 cookie、同坐标无标识替换、创建前后崩溃和
无 handle 失败。另用保留 cookie 但篡改动作的测试夹具验证正式创建回读会回滚。
旧驱动拒绝、remote 原生拒绝、普通 remote 创建/删除与旧恢复路径一并回归。

这一步仍不是自动清理授权：cookie 不防有权限的程序复制或伪造；当前读数不能证明
接口从未销毁重建，也不能阻止检查之后被外部修改。共享动作检查只解释当前引用数，
不承诺所有命名空间或其他 PMD 的完整范围。下一步是设计只读精确清理计划，列出满足
条件的候选与拒绝原因，并解决接口实例和检查后变化的问题，再实现实际删除。

实现依据包括仓库独立 DPDK 补丁，以及 Linux 6.8 的
[flower 导出字段](https://github.com/torvalds/linux/blob/v6.8/net/sched/cls_flower.c)、
[gact 引用与动作](https://github.com/torvalds/linux/blob/v6.8/net/sched/act_gact.c) 和
[skbedit 队列动作](https://github.com/torvalds/linux/blob/v6.8/net/sched/act_skbedit.c)。
