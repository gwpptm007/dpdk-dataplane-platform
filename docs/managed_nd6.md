# 动态 ND 统一管理

本项把已有 IPv6 邻居发现接入正式 `--flow-state FILE --managed-routing` 模式
本机 IPv6 地址、VLAN 域和可达时间参数，与规则、路由、静态邻居、CT/NAT 使用同一份配置文件和全局修订号
动态邻居、地址检测结果、等待报文和设备 MAC 都不落盘，重启后重新检测本机地址并学习邻居

## 配置与在线更新

每次替换完整 ND 策略，包含 `reachable_ms` 和 `locals` 数组
最多 32 个本机 IPv6 地址，必须属于当前实例绑定的实际端口
允许普通单播和链路本地地址，拒绝未指定、回环、组播和 IPv4-mapped 地址
每个地址可以指定零层、一层或两层 VLAN，VID 为 1～4094；同一端口的地址必须使用相同 VLAN 域，不能重复地址
声明顺序保留，同端口第一个地址参与探测源地址选择，不能把地址排序当作无影响的格式整理

启用时 `reachable_ms` 范围为 1000～3600000 毫秒
关闭时必须提交 `reachable_ms=0`、`locals=[]`，省略字段不表示沿用旧值
旧 `--nd6`、`--nd6-reachable-ms` 参数仍属于独立路由模式，统一模式从持久请求取得策略

首次 ND 请求将配置升级为第五版 flow-state，旧一至四版仍可读取
管理协议为 v33，daemon 和 dppctl 需要一起升级；旧程序不能读取第五版文件，降级前应保留兼容备份
设备启动后重新获取真实 MAC，端口编号、设备身份和 MAC 绑定检查通过才恢复 ND 运行表

启用新端口的 ND 前，先打开该端口的全部组播接收并读回确认，才能收到 NS/NA
驱动准备失败不发布策略，也不创建持久回执；已经临时打开的端口会恢复原状态
如果恢复失败或无法确认接收状态，daemon 失败退出，避免带着不确定的设备状态继续运行
配置最终没有应用时撤回临时设置；已经应用时，即使保存失败也保留其所需接收能力
成功启用的组播模式保留到设备关闭，后续删除本机地址不会关闭其他地址仍可能使用的接收模式

## 更新为什么需要暂停报文线程

一部分报文可能还在等待下一跳 MAC，另一些刚从等待队列取回，尚未重新检查路由和 ACL
正式入口先停止并等待全部 worker，再替换 ND 策略或精确清理动态缓存，避免旧等待包跨入另一份配置
线程停止失败时不执行修改；停止超时按既有 managed 超时路径退出，不释放仍被线程借用的对象

`reset_neighbors` 必须显式填写：

- `false`：存在动态邻居或等待包时返回 `EBUSY`，原策略、缓存和等待包保持不变
- `true`：版本核对通过后清除动态邻居，回收仍属于队列的报文，并从 TENTATIVE 状态重新执行 DAD

两种方式只要成功替换策略，都会重新执行 DAD；`false` 只是不允许丢弃已有邻居和等待包

候选分配或校验失败不触碰旧状态，同键同内容重试只返回原回执，不再次停线程、清缓存或执行 DAD
新请求即使被拒绝，也可能经过短暂停机检查；本功能不承诺无损在线切换
CT/NAT 表及其中的活跃会话保留，ND 重置不会等同于会话 flush

成功发布且可靠保存后，主循环恢复 worker
此时 IPv4 可以继续转发，配置了 ND 的 IPv6 端口需要等待本机地址全部变为 PREFERRED
`ready` 在 TENTATIVE 或 DUPLICATE 时返回未就绪，但 worker 保持运行，才能发送检测报文和接收冲突证据
收到冲突后不能通过清邻居缓存解除 DUPLICATE，需要修正配置或显式替换策略后重新检测

## 三种检查值

| 字段 | 保护范围 |
| --- | --- |
| `revision` | 整份 managed 配置，规则、路由、CT/NAT 或 ND 更新都可能让它变化 |
| `generation` / 回执 `nd6_generation` | 当前 ND 策略的代次 |
| `neighbor_epoch` + `neighbor_revision` | 本次启动及运行态 ND 状态，保护分页、缓存清理和策略替换 |

ND 状态变化、邻居增删和等待包进出都推进 `neighbor_revision`
查询后继续收发报文可能让提交返回 `ESTALE`，需要重新查询，并重新决定是否允许清空
epoch 在重启后改变，旧进程的清理指令不能用于新进程
计数不是逐包冻结快照，邻居列表也不等同于持久配置

## CLI

```sh
dppctl --socket /run/dppd/control.sock --json nd6-policy-show
dppctl --socket /run/dppd/control.sock --json nd6-status
dppctl --socket /run/dppd/control.sock --json nd6-list 0
```

完整文本策略例如 `nd.conf`：

```text
nd6 30000
local 0 fe80::10
local 0 2001:db8:1::fe
local 1 fe80::20
local 1 2001:db8:2::fe
```

带标签时，地址后面再跟一个 VID 或外层、内层两个 VID，例如 `local 1 fe80::20 100 200`
本机地址只属于 DPDK 接口，命令不会向 Linux 接口添加地址
`nd6-policy-show` 的普通文本输出可以保存后作为下一次完整策略文件

```sh
# 三个版本检查值必须来自本次查询，新请求使用递增序号
dppctl --socket /run/dppd/control.sock --json request-submit "$CLIENT" "$SEQUENCE" \
  nd6-replace nd.conf "$REVISION" false "$NEIGHBOR_EPOCH" "$NEIGHBOR_REVISION"

dppctl --socket /run/dppd/control.sock --json request-get "$CLIENT" "$SEQUENCE"

# 后续分页同时传回版本和启动标识
dppctl --socket /run/dppd/control.sock --json nd6-list "$NEXT_ID" "$NEIGHBOR_REVISION" "$NEIGHBOR_EPOCH"

# 只清动态缓存和等待包，保留本机地址配置与 DAD 状态
dppctl --socket /run/dppd/control.sock --json nd6-flush "$NEIGHBOR_REVISION" "$NEIGHBOR_EPOCH"
```

独立模式仍接受旧的无 epoch 查询/清理形式，统一管理模式的后续分页和清理必须提供精确 epoch 与 revision
缓存 flush 没有持久请求键，响应丢失后应先查询，不要改用新版本盲目再清一次

## HTTP

| 接口 | 权限 | 用途 |
| --- | --- | --- |
| `GET /v1/nd6/policy` | reader 及以上 | 完整配置、全局版本、策略代次和运行态检查值 |
| `GET /v1/nd6/status` | reader 及以上 | 本机地址检测状态、邻居数、等待包数和累计事件 |
| `GET /v1/nd6/neighbors` | reader 及以上 | 动态邻居分页，后页传 `after`、`epoch`、`revision` |
| `POST /v1/nd6/neighbors/flush` | operator 及以上 | 精确清理，JSON 字段为 `epoch`、`revision` |
| `POST /v1/requests`，`operation=nd6_replace` | admin | 完整策略持久替换 |

示例中的检查值必须替换成实际查询结果，所有 64 位标识和版本使用十进制字符串：

```json
{
  "operation": "nd6_replace",
  "sequence": "21",
  "revision": "108",
  "reset_neighbors": false,
  "expected_neighbor_epoch": "123456789",
  "expected_neighbor_revision": "1",
  "policy": {
    "reachable_ms": 30000,
    "locals": [
      {"port_id": 0, "address": "fe80::10", "vlans": []},
      {"port_id": 1, "address": "fe80::20", "vlans": []}
    ]
  }
}
```

客户端身份来自认证凭据，不由请求体指定
检查 `data.status`、`data.applied` 和 `data.durable`，HTTP 200 和外层 `status=0` 只说明请求接口正常处理
例如 `data.status=-16` 表示 EBUSY，`data.applied=false` 表示没有发布新策略

## 路由、CT/NAT 与异常恢复

静态 IPv6 邻居优先，只有缺少静态邻居时才尝试动态解析
等待解析的首包再次取出时重新检查 ACL、路由和出口，未放行的首包不创建成功 CT/NAT 会话
队列满、等待超时、探测失败、策略重置或缓存 flush 都明确处理 mbuf 所有权，不能重复释放或永久占住缓冲池
已有 CT 会话可能在重新执行 DAD 或邻居学习期间暂时无法通信，但身份和映射不会因 ND 更新而主动删除

受理记录先保存，再发布配置，最终策略和完整回执一起落盘
应用后的保存失败保留 dirty，worker 继续停止，修复存储后执行 `persistence-flush` 才能恢复
如果最终配置尚未替换文件就崩溃，重启取消残留 pending 请求，不重新执行旧的清缓存动作
重启恢复本机配置后重新 DAD，动态缓存为空，不把上次的 PREFERRED 或已学习 MAC 当作本次启动的证据

本项不包含 RA、SLAAC、Redirect、默认路由自动学习、多 VLAN 同端口接口模型、ND 缓存持久化或跨进程会话迁移
规则、路由、CT/NAT 和 ND 共用版本与文件，但仍是独立事务，不提供四者同时切换的跨对象原子提交
物理网卡/DPU 的组播支持、实际链路行为和硬件卸载仍需实机验收

## 验收入口

```sh
meson test -C build-perf --print-errorlogs
sudo unshare --net python3 tests/integration/managed_nd.py \
  --build-dir build-perf --report-dir build-perf/managed-nd-tap --tap
```

2026-11-07，在 135 主机隔离目录 `/home/wq7/dppd-managed-nd-20261107/source` 清理构建产物后完整重建并验收

| DPDK / 构建 | 单元测试 | 动态 ND 三 TAP |
| --- | --- | --- |
| 21.11.9 优化版 | 98/98，无跳过 | 11 组通过 |
| 21.11.9 ASan/UBSan | 98/98，无跳过 | 11 组通过 |
| 24.11.8 优化版 | 98/98，无跳过 | 11 组通过 |
| 24.11.8 ASan/UBSan | 98/98，无跳过 | 11 组通过 |

每套注入 47 帧数据：30 帧按完整帧字节核对转发，16 帧按预期丢弃，1 帧在暂停期间留在 TAP，恢复后被 DAD 门禁丢弃
`worker_stopped=1` 描述的是最后这帧的中间状态，不能重复计入注入总数
NS/NA 控制帧另行核对 IPv6 hop-limit、ICMPv6 校验和、目标地址、选项和探测次数，不混入数据帧统计

覆盖无 CT 动态转发、首包等待与 NAT66 双向还原、无效 NA、分页及过期版本、保留 CT 的重置、NUD 有界探测、
队列上限与超时、重试重新检查 ACL、DAD 冲突、双层 VLAN、CLI/HTTP 一致性、SIGKILL 后恢复、存盘失败后修复和 dirty 带等待包退出
四套均确认测试子进程退出；mbuf 单测检查引用归还与重复清理，不据此宣称做过完整分配器泄漏检测
ASan 使用 `detect_leaks=0`，本次没有 LeakSanitizer 验收结论

两套优化版均通过既有 managed 路由 6 组、managed NAT 8 组、M5 的 4 步验收及其嵌套 M2/M3/M4 软件回归、
HTTP/TLS 4 组和独立模式 ND 报文回归
M5 的嵌套双 worker 测试至少需要三个 CPU，最初的两核验收分配在运行报文前被拒绝，改为每套四核后通过，无产品代码变更

26 份含嵌套验收的 `report.json` 均成功并核对同一份 356 项源码
源码摘要为 `db8d3f23ffa416d45daf02be5475d4892421326b30dd5020fa3efbe959407584`
四份运行库记录核对 daemon 和报文夹具实际依赖：系统构建使用 21.11.9，现代构建全部使用私有 24.11.8，无缺库或混用旧版 DPDK
原始证据保存在本地忽略目录 `build/native-check/managed-nd/`，汇总为 `summary.json`，归档为 `host-135-evidence.tar.gz`
归档 SHA256 为 `7e13dab4ef86c116d8076a5b1a65b0f95e713c177eb4278dcc6934d8ccaa85ff`

本次未在 134 主机重新运行，也未重做原始整套性能指标和发布包可复现验收，不扩大先前 M5 报告或物理硬件验收范围
