# TAP 残留规则的只读核对

2026-10-09，跨进程恢复增加第一个驱动适配器：`net_tap` 的本地 Linux TC 规则检查。
异常退出后，工具能把当前内核中的规则列出来，并区分接口消失、接口被替换、运行环境
不一致与无法检查。检查不会删除规则，也不会解除恢复保护。管理协议保持 v15，规则
快照保持 v2；独立恢复记录升级为 v2，仍能读取旧 v1 文件。

## 使用

daemon 必须在安装前启用 `--state-path` 和 `--recovery-path`，具体部署要求见
[恢复保护](recovery_guard.md)。旧进程结束后，使用同一主机、同一网络命名空间和有权
读取记录的账户执行：

```bash
sudo ./build/dppd-recovery show /var/lib/dppd/hardware.recovery
# 将 2 换成 show 返回的当前 revision
sudo ./build/dppd-recovery inspect /var/lib/dppd/hardware.recovery 2
```

`show` 只显示保存的线索，新增 `format` 和 TAP 的 `identity` 行。`inspect` 持有同一
恢复文件的排他锁，先核对修订号，再向内核发送只读 Netlink GET 请求，不初始化 EAL。
daemon 正在运行时返回 `EBUSY`，修订号过期返回 `ESTALE`。

示例结果中的关键字段：

```text
inspection revision=2 ports=2 mode=read-only scope=tap-local-multiq-and-ingress
ownership=unproven remote-scope=excluded atomic-snapshot=no cleanup-confirmed=no
port=0 device=net_tap0 driver=net_tap ifindex=27 ifname=dtap0 status=coordinates-match complete=yes error=0 filters=1
filter port=0 parent=00010000 handle=00123456 chain=0 priority=11 protocol=0003 kind=flower owner=unknown
```

`filters` 是此次成功读取的本地 TC 对象数量，排除了没有具体 handle 的分类器标题。
每条规则显示挂载点、句柄、链号、优先级、协议号及分类器类型；handle、parent、protocol
以十六进制显示。这里没有业务规则 ID 的对应关系，`owner=unknown` 表示归属尚未证明。

| status | 含义 | 返回行为 |
|---|---|---|
| `coordinates-match` | 当前接口的索引、名称和 TAP 类型匹配，完成指定范围的查询 | 成功，显示规则数 |
| `interface-absent` | 同一启动环境、同一网络命名空间内，原索引和原名称都不存在 | 成功，显示本地零规则；不确认清理 |
| `identity-mismatch` | 原索引对应其他接口，或原接口消失后同名接口以新索引出现 | `EXDEV`，不显示零规则 |
| `context-mismatch` | 系统启动标识或网络命名空间与记录不符 | `EXDEV`，不查询该接口的规则 |
| `identity-unavailable` | TAP 旧记录没有可靠内核定位信息 | `ENODATA` |
| `unsupported-driver` | 尚无对应 PMD 的检查适配器 | `ENOTSUP` |
| `unavailable` | 内核查询失败、消息损坏、查询中断、数量超限或检查期间接口变化 | 明确错误，丢弃部分列表 |

只有所有端口均完成查询时命令退出码才是 0；其他情况下仍打印各端口结果，退出码为 1。
没有待核对端口时返回成功且不访问设备。`complete=yes` 只说明该次指定范围的查询完成，
不表示可以重放、更不表示设备全局已干净。文件内容和修订号都保持不变。

## 接口身份和检查范围

TAP 安装前，先通过 `rte_eth_dev_info_get()` 取得真实内核 ifindex，再向内核确认接口
是 TAP，保存接口名、系统启动 UUID、当前线程网络命名空间的设备号和 inode。
无法采集这些信息就拒绝这次驱动 create；同端口后续安装仍核对身份，不能复用已经变化
的接口标记。采集只发生在控制线程，逐包路径不增加查询或文件访问。

检查先核对环境，然后按索引和名称定位接口。即使坐标全部匹配，也无法排除同一启动中
接口被删除并以相同索引、相同名称重新创建。因此命令只报告“坐标匹配”，不证明接口
实例连续存在，也不证明任何规则属于本项目。

当前范围是本地 TAP 的 `1:` multiq 与 `ffff:` ingress 挂载点。先读取 qdisc，再分别
读取其过滤器；已知挂载点换成其他类型时失败。缺少挂载点表示该挂载点当前没有对象。
查询全部链，不展开规则 match/action。每端口最多 256 个具体对象，超过返回 `E2BIG`，
不以截断列表冒充完整结果。接收验证内核来源、序号、长度及属性边界，并拒绝截断、
dump 中断和错误完成消息；接收超时两秒、最多 128 批消息。查询结束再核对一次接口。
这些分步读取不是原子快照，其他程序仍可能同时修改 TC。

DPDK 21.11 TAP 驱动用进程内地址生成 TC 句柄，因此业务规则 ID、generation 或优先级
都不足以证明跨进程所有权。该限制来自
[TAP flow 实现](https://github.com/DPDK/dpdk/blob/v21.11/drivers/net/tap/tap_flow.c)。
`remote=` 还可能在另一接口安装重定向规则，见
[TAP 驱动文档](https://doc.dpdk.org/guides-21.11/nics/tap.html)。本次不检查远端接口、
其他挂载点、全局动作表或物理 PMD；本地 TAP 消失不代表这些范围没有残留。

外部核对和清理完成后，仍须按 [显式确认流程](recovery_guard.md#查看线索并恢复)
执行 `acknowledge-clean`，再用原配置重启。本次不提供自动确认、flush、删除或复位。
后续需要先建立可持久验证的逐规则归属证据，再设计精确清理和失败恢复。

## 恢复文件兼容性

新建恢复文件使用 v2，长度 9248 字节。头部和快照路径与 v1 一致，16 个端口槽位从
224 字节扩展为 320 字节；magic 仍是 `DPPREC1\0`，格式以头部数值版本区分。

新增端口字段偏移相对于各槽位起点：

| 偏移 | 字节数 | 内容 |
|---|---|---|
| 216 | 4 | 内核 ifindex，小端；零表示未知 |
| 220 | 16 | 接口名，NUL 终止 |
| 236 | 40 | 启动 UUID，NUL 终止 |
| 276 | 4 | 零保留位 |
| 280 | 8 | 网络命名空间设备号，小端 |
| 288 | 8 | 网络命名空间 inode，小端 |
| 296 | 24 | 零保留位 |

ifindex 非零时驱动必须为 `net_tap`，索引、名称、UUID 格式和命名空间 inode 均须有效；
未知身份及空槽位的扩展字段必须全零。仍对完整文件执行 CRC32 和规范重编码核对。

v1 待核对文件保持原格式、原修订号，离线查看和检查都不升级；缺失的身份不能猜测。
显式清理确认仍写回 v1。只有干净 v1 被新 daemon 打开时，才原位升级为 v2 并推进一次
修订号，保持原 inode 和文件锁。旧版程序不能读取 v2，不能直接降级运行。

## 验证记录

Ubuntu 22.04.5、内核 6.8.0-138、DPDK 21.11.9、GCC 11.4、Meson 0.61.2：

- `-Werror` 构建及 27/27 单测通过
- 解析器覆盖 TC 字段、属性损坏、容量、其他接口过滤，以及消息截断、错序号、dump 中断、丢包和超时
- 恢复记录覆盖身份保存、同端口身份变化拒绝、v1 待确认保护及干净文件升级
- 自有持久 TAP 上安装真实驱动规则和一条外部测试规则，SIGKILL 后保留实际残留；检查列表与 `tc -j` 逐条对齐
- 查询前后内核规则、恢复文件和规则快照一致，外部规则同样标记归属未知
- 实际新网络命名空间、同名重建接口、接口删除、旧修订号、锁竞争及 v1 缺失身份拒绝通过
- 既有 ring 夹具和真实 TAP 恢复保护场景通过
- 检查器、恢复保护、硬件 backend、控制服务四项单测通过 AddressSanitizer/UBSan

```bash
meson setup build -Dwerror=true
meson compile -C build
meson test -C build --print-errorlogs
sudo python3 tests/integration/recovery_inspect.py --build-dir build
python3 tests/integration/recovery_guard.py --build-dir build
sudo python3 tests/integration/recovery_guard.py --build-dir build --tap
```

测试只创建和删除自有虚拟接口，结束后回收；没有绑定、清空或复位物理网卡。
这些结果证明本地 TAP 的只读残留核对，不代表物理 NIC/SmartNIC 的规则归属或自动恢复验收。
