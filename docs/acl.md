# IPv4 软件 ACL 闭环

2026-10-17，M2 的 IPv4 软件访问控制已接通正式 worker、管理 CLI、版本更新、规则持久化
和重启恢复。默认不命中 ACL 时继续原有二层或三层转发；需要白名单时，先放置较高
优先级的允许项，再增加较低优先级的 IPv4 拒绝项。允许只结束本次 ACL 匹配，随后仍要
通过路由、邻居、TTL、校验和和出口状态检查。

## 规则语义

- 只处理送达 CPU worker 的报文，按安装入口端口隔离
- IPv4 源/目的地址可使用 CIDR，底层 IR 继续支持原有非连续地址掩码
- TCP、UDP 支持源/目的端口的闭区间，包括端点、零端口和 65535
- `ip:N` 匹配 IPv4 原始协议号，`ipv4` 匹配所有 IPv4 协议
- 每包只选择一条规则，group、priority、ID 较小者优先；不是多组跳转
- 同一 ID 的两个版本短暂并存时保留既有槽位顺序；纯软件批量更新一次替换整个快照
- `allow` 或 `drop` 是终止动作，可附加 `count` 和 `mark:N`
- 只有最终选中规则累计 COUNT，允许包后来因路由失败丢弃也保留这次命中
- 正式 worker 在每个接收批次归并规则 COUNT，发送和 QSBR 静默前提交；查询可能暂时
  看不到正在处理的批次，命中数和字节数仍分别采样，新版本不继承旧版本计数
- 分片不重组，也不猜测 TCP/UDP 端口；所有分片均不命中 TCP/UDP 范围项，可由 IPv4/
  协议号规则处理，或落到兜底拒绝项
- 畸形报文在 ACL 前丢弃；IPv4 选项等后续转发限制沿用 [三层转发](ipv4_routing.md)

本阶段是软件 worker 的 ACL，不接管已经由硬件处理完的报文。硬件 DROP、QUEUE、
transfer 等对象仍受原有能力和恢复规则约束，软件允许项不能撤销已发生的硬件 DROP。
部署白名单时应先核对现有硬件规则，不能把未送达 CPU 的流量计作经过软件 ACL。
SmartNIC/DPU 上全路径策略一致性及硬件 ACL 适配属于后续阶段。

## 使用 CLI

`acl-set` 固定使用软件后端，创建使用旧版本 0，更新必须指定查询到的精确旧版本，
不接受 `any`。ID、版本和计数与已有 rule repository 共用，没有第二套 ACL 账本。
ACL 首版为 v17，当前 [隧道统计](tunnels.md) 已扩展为 v23，daemon 与 CLI 必须配套。
`state:new,established` 等状态条件已接通，启用跟踪后所有规则强制经软件执行。

```sh
# 端口 0：允许指定地址范围的 UDP，源端口 1000–2000，目的端口 8000–9000
./build/dppctl acl-set 101 0 0 10 udp 198.18.0.0/24 10.2.0.0/16 1000-2000 8000-9000 allow count mark:77
# 数值越大优先级越低，用作剩余 IPv4 流量的兜底拒绝
./build/dppctl acl-set 102 0 0 100 ipv4 any any any any drop count
# TCP 精确端口和仅按 IPv4 协议号匹配
./build/dppctl acl-set 103 0 0 5 tcp any 10.4.0.0/16 any 443 allow count
./build/dppctl acl-set 104 0 0 5 ip:17 any 10.7.0.0/16 any any allow count
./build/dppctl list
./build/dppctl get 101
./build/dppctl rule-status 101
./build/dppctl count 101 1
```

命令格式为 `acl-set ID PORT EXPECTED PRIORITY PROTOCOL SRC_CIDR DST_CIDR SRC_RANGE DST_RANGE
allow|drop [count] [mark:N] [state:STATES]`。范围接受 `any`、单值或 `LOW-HIGH`。`ipv4` 和 `ip:N` 的两个
端口参数必须为 `any`，不能让无端口语义的规则悄悄忽略端口限制。
示例中的版本 1 仅用于全新账本，实际更新、COUNT 和删除应使用 `get` 返回的版本。
`delete ID GENERATION`、规则详情、安装状态、失败历史、统计和 telemetry 继续复用原接口。

`acl-update-batch FILE` 接受两到四条已存在规则的完整替换，文件每行写一组 `acl-set`
的参数，不写命令名：

```text
# ID PORT EXPECTED PRIORITY PROTOCOL SRC_CIDR DST_CIDR SRC_RANGE DST_RANGE ACTION [count] [mark:N]
101 0 1 10 udp 198.18.0.0/24 10.2.0.0/16 1000-2000 8000-9000 drop count
102 0 2 100 ipv4 any any any any allow count
```

文件最多 64 KiB，单行最多 511 字节，支持空行和 `#` 注释。非普通文件、符号链接、
嵌入 NUL、重复 ID、无效范围、零旧版本、半条规则或多余字段均拒绝；全部解析成功才
发送请求。服务端再次核对全部版本和容量，任何前置检查或分配失败均保留完整旧 ACL。
纯软件两到四条规则更新只发布一个快照，满规则表也可更新；不承诺多个独立命令原子
执行，也不把单条创建/删除或规则与路由跨文件操作包装成整体事务。

## 编译、发布和回收

新增独立 `acl` 模块，将 IR 编译为紧凑匹配项，按入口端口和规则优先顺序存放。
查询先二分定位端口，再顺序检查该端口的规则，首个匹配即返回动作槽位。查询复杂度为
O(log N + 本端口检查项数)，控制面插入排序构建为 O(N²)，尚未做线速或大规模性能验收。
匹配表随规则快照一次分配，编译无额外堆分配且无需 EAL，便于无网卡单元验证。
后续加速实现必须保持当前语义和独立参考模型的结果一致。

ACL、动作和共享计数器引用属于同一张不可变快照，编译全部完成后再原子切换 active。
worker 不获取管理锁、不在逐包路径分配内存，旧表仍通过 DPDK QSBR 在完整端口扫描后的
静默点回收。未更新的规则版本保留 COUNT，新版本从零累计；共享/间接/外部报文的
改写副本保留 ACL MARK，原始共享报文不被写入新标记。

规则 IR 追加 UDP/TCP 范围、IP 协议和 ALLOW 类型，旧类型编号及 snapshot v2 的记录
布局保持兼容；新增字段有显式磁盘编码和完整校验。旧程序遇到新类型会拒绝恢复，不能
直接用旧二进制读取包含新规则的快照。新增软件语义的硬件翻译返回 ENOTSUP，不把
ALLOW 伪装成硬件直转或 QUEUE；通用规则接口的 prefer/require 仍遵守已有降级约束。

## 持久化及故障

指定 daemon `--state-path` 后，ACL 沿用 [规则 snapshot v2](persistence_snapshot_v2.md)：
CRC、私有权限、同目录临时文件、文件同步、原子替换和目录同步。启动完整恢复原规则
版本并重新编译 ACL，COUNT 从零开始，损坏快照必须拒绝启动，不能按空 ACL 放行。

规则持久化在规则生效后执行。保存失败返回 EUCLEAN，当前规则可能已经更新，
`rule-metrics/rule-history` 保留 applied 标志，`get/rule-status` 可核对当前版本。
`ready` 报 `persistence-dirty`。下一次修改先尝试补写当前完整账本，故障持续时拒绝新的
修改；修复存储后也可显式执行 `persistence-flush`。这与路由文件“保存成功再发布”的
提交时点不同，不能用失败响应推断 ACL 已回滚。

文件同步失败时磁盘仍可能是旧版；替换后目录同步失败时新版可能已经可见。未修复前
崩溃，启动按实际完整磁盘快照恢复，不编造尚未保存的新规则。正常退出会沿用既有规则
补写流程，不能用正常退出替代 SIGKILL 崩溃窗口测试。规则与路由分别持久化，互不组成
跨文件事务。

## 验收

Ubuntu 22.04、DPDK 21.11.9 系统构建和原生 TAP 标识补丁构建均使用 `-Werror`。
两套各 41/41 单测，五项相关 AddressSanitizer/UBSan 检查通过：

- 256 条规则与独立 IR 解释器对照 30000 次查询，覆盖任意掩码、范围、协议、入口和排序
- 范围上下界、零/65535、非 IPv4、分片、硬件拒绝、无效动作及容量边界
- 两条/四条允许与拒绝规则在并发读者下各更新 2000 次，任何半次优先级切换都会被发现
- 每个相关分配失败位置保持旧表、旧动作、旧计数，重放失败不发布部分恢复
- CLI 范围、精确版本、协议号、整批文件错误及嵌入 NUL 拒绝

```sh
meson test -C build --print-errorlogs
sudo python3 tests/integration/acl_traffic.py --build-dir build
sudo python3 tests/integration/acl_traffic.py --build-dir build-native
python3 tests/integration/ipv4_traffic.py --build-dir build
python3 tests/integration/software_traffic.py --build-dir build
```

真实 daemon 在自建空网络命名空间的三个 TAP 上验收完整输出字节、VLAN、允许/拒绝、
UDP 范围边界、TCP 443、协议号和分片、优先级与 COUNT、TTL/校验和后续拒绝、精确版本、
整批更新、SIGKILL 重启、删除和坏文件拒绝。两种同步故障均核对当前已生效规则、磁盘
状态、就绪失败、持续故障阻断、flush 修复，并检查未保存 ACL 崩溃后恢复旧快照。
故障只在测试版入口包装 fsync，正式程序没有故障开关。

net_ring 使用正式 worker 验证一队列/两队列的 ALLOW、范围、MARK、COUNT 及共享原包
不变，保留原有四端口 17 类报文、1000 次路由热更新、零 mbuf 泄漏和二层回归。
测试创建并清理自己的 TAP，不改实体网卡。TCP/UDP 状态 ACL、连接跟踪和出站 SNAT
已在后续 [有状态阶段](conntrack_nat.md) 接通；[IPv6 ACL 与转发](ipv6.md) 也已接通，完整硬件策略一致性和性能目标仍未完成。
