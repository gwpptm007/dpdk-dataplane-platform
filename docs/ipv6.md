# IPv6 解析、软件 ACL 与静态转发

2026-10-17，IPv6 已接入正式解析器、软件 ACL、最长前缀查找、静态邻居、MAC/Hop Limit
改写、管理 CLI、规则与路由持久化及重启恢复。IPv4 与 IPv6 可以同时转发；后续已接通 [IPv6 连接跟踪与 NAT66](nat66.md)，
跟踪由 `--conntrack` 显式启用，IPv6 转换按 `--snat6`、`--dnat6` 分别配置。本阶段是有明确边界的软件静态路由基线，不表示
整个 M2 或完整 IPv6 路由器功能已经完成。

## 开启双栈路由

在 EAL 参数之后使用：

```sh
--ports 0,1 --ip-routing /private/routing.bin
```

`--ip-routing` 使用双栈路由与静态邻居，原来的 `--ipv4-routing` 仍是兼容别名。
未启用路由时保留原端口对转发；启用后，各地址族分别查询自己的路由，不会把 IPv6
未命中转回端口对，也不会借 IPv4 默认路由放行 IPv6。初始为空表，需先配置下一跳。

IPv4、IPv6 的路由和邻居共用一个版本和一次快照发布。每个地址族分别最多 256 条
路由、256 条邻居，不按队列复制可修改配置。最长前缀查询与 DPDK QSBR 读者回收共用
原有线程生命周期，更新失败保留完整旧表，不能发布半张双栈表。

```sh
./build/dppctl routing-status
# 先查询 generation，下面的 1、2 仅用于刚创建的空表
./build/dppctl neighbor6-set 1 fe80::1 02:00:00:00:20:01 1
./build/dppctl route6-set 2001:db8:2::/64 1 fe80::1 2
./build/dppctl routing-show
```

支持 `route6-set PREFIX PORT NEXT_HOP|direct GENERATION`、`route6-delete PREFIX GENERATION`、
`neighbor6-set PORT IP MAC GENERATION` 和 `neighbor6-delete PORT IP GENERATION`。
创建或覆盖、删除都要求当前精确全局版本。`/0` 到 `/128` 均可，CLI 清除前缀主机位，
底层 API 拒绝不规范前缀；重复前缀、未知出口、组播/零 MAC 和非法下一跳均拒绝。
直连路由使用目的地址查询该出口的静态邻居。

同一个 `fe80::1` 可出现在不同出口，邻居键由出口端口和完整 IPv6 地址组成。CLI 不接受
`%eth0` 作用域写法，作用域由独立的 PORT 参数给出。链路本地地址可以作为下一跳，
不能作为跨接口转发的报文源或目的。未指定、回环、组播和 IPv4-mapped 地址也不转发。

`routing-replace FILE GENERATION` 一次替换全部双栈配置，文件可混合：

```text
route 198.51.100.0/24 1 192.0.2.1
neighbor 1 192.0.2.1 02:00:00:00:20:01
route6 2001:db8:2::/64 1 fe80::1
neighbor6 1 fe80::1 02:00:00:00:20:01
```

沿用 64 KiB 普通文件限制、逐行校验和精确版本保护。文件只写某个地址族时，另一个
地址族会被清空，这是整表替换的含义；单项修改会保留另一地址族。该命令不与 ACL
文件组成跨文件事务。

## IPv6 ACL

```sh
./build/dppctl acl6-set 6101 0 0 10 udp 2001:db8:1::/64 2001:db8:2::/64 1000-2000 443-445 allow count mark:77
./build/dppctl acl6-set 6102 0 0 100 ipv6 any any any any drop count
./build/dppctl get 6101
```

参数与 `acl-set` 对应，协议可为 `ipv6`、`udp`、`tcp` 或 `ip:N`。IPv6 地址只接受 `any`
或 IPv6 CIDR，不能混入 IPv4。完整 128 位地址和掩码逐字节比较，支持 `/65` 等非整字节
前缀；IR 仍允许任意地址掩码。TCP/UDP 端口范围包含两端，`ipv6`、`ip:N` 的端口参数
必须为 `any`。IP 协议条件使用合法扩展头之后的最终 Next Header。

沿用入口端口隔离、group/priority/ID 顺序、每包第一条匹配、MARK/COUNT、允许后继续
转发及精确版本更新。默认未命中继续转发，需要白名单时显式加低优先级拒绝项。
`acl6-update-batch FILE` 支持两到四条已存在 IPv6 规则的完整原子替换，格式为每行
一组 `acl6-set` 参数但不写命令名。纯软件批量更新仍只发布一次快照，错误或旧版本
不能覆盖新表。规则查询、COUNT、删除和持久化复用原有接口。

IPv6 ACL 当前强制使用软件后端，不承诺硬件等价执行，`require` 硬件请求会拒绝。
未启用跟踪时 IPv6 为 UNTRACKED。后续双栈跟踪支持 NEW、ESTABLISHED 和 INVALID，
状态条件、使用边界和 NAT66 见 [有状态阶段](nat66.md)。仅配置 IPv4 NAT 不会转换 IPv6 地址。

## 解析、改写与错误归因

解析 IPv6 固定头、完整源/目的地址、Payload Length、最终协议、TCP/UDP 端口和 TCP
头长。两层 VLAN、TCP 选项及分段 mbuf 均按实际边界读取，不把以太网尾部填充算作
IPv6 负载。源和目的地址、Traffic Class、Flow Label、扩展头与负载保持原字节。

扩展头处理有数量和 256 字节预算，当前支持首个 Hop-by-Hop、一个 Destination Options
及其中的 Pad1/零填充 PadN。重复或乱序、未知选项、Routing Header、AH/ESP、Mobility
等扩展明确不转发；不跳过不了解的头再猜 TCP/UDP 端口。识别 Fragment Header，包括
原子分片，但不提供端口条件且转发阶段拒绝，不重组、不制造 IPv6 分片。Jumbogram
不支持。No Next Header 可表示零长度负载，普通未知上层协议保留为 OTHER，不解析端口。
这些字段与扩展头范围参照 [RFC 8200](https://www.rfc-editor.org/rfc/rfc8200)，地址范围
参照 [RFC 4291](https://www.rfc-editor.org/rfc/rfc4291)，本实现不声明完整协议栈合规。

ACL 先执行，允许后查路由/邻居、检查出口，再检查并改写报文。转发 TCP、UDP、ICMPv6
时校验完整 IPv6 伪首部和负载，IPv6 UDP 零校验和拒绝；ICMPv6 Echo 可双向转发，
ND/RA/Redirect 等链路控制报文不路由；后续 [ND 阶段](nd6.md)可单独处理本机 NS/NA。Hop Limit 不大于 1 时丢弃，正常减一；IPv6 没有
IPv4 头校验和，也不会因只改变 Hop Limit 而重写传输层校验和。

共享、间接、外部或多段报文沿用独占副本保护，复制失败保留原包；清除旧 TX 校验和
卸载请求。转发结果按真实出口发送，MARK 只写到输出包。无路由和无邻居沿用
`no_route_drops`、`no_neighbor_drops`；新增 `ipv6_hop_drops`、`ipv6_checksum_drops`、
`ipv6_unsupported_drops`、`ipv6_rewrite_drops`。ACL 先拒绝时仍记 `rule_drops`，不改成
后续校验错误；畸形长度记 `rx_malformed`。

## 存储、重启与兼容

此静态转发阶段使用 v20，当前 [动态 ND](nd6.md) 扩展为 v22，daemon 与 CLI 必须配套，旧版本不会错位解释新增结构体。
旧的纯 IPv4 路由文件 v1 保持可读；第一次写入 IPv6 路由或邻居时升级为双栈 v2。
此后即使清空 IPv6 也保留 v2，避免存储不确定状态修复时改变文件布局。继续校验 CRC、
规范化内容、稳定端口映射、权限和文件身份。文件同步失败不发布，目录同步失败保留
磁盘不确定语义、阻断更新并要求 `routing-flush` 修复。

规则快照仅在包含 IPv6 匹配时使用 v3，保存完整的地址与掩码；纯 IPv4 规则仍写 v2，
正常启动可读取 v2/v3，v1 仍须走原离线单端口迁移流程。混合规则重启保留安装入口、
内容和 generation，COUNT 从零开始。快照损坏、截断、版本或设备映射不符均拒绝
启动，不能用空配置覆盖。旧程序无法读取新扩展格式，回滚版本需预先规划格式兼容。

IPv4 与 IPv6 使用同一进程和出口队列，已有 IPv4 ACL、SNAT、DNAT 继续有效。
启用跟踪后 IPv6 TCP/UDP 创建自己的双栈会话；它不消耗 IPv4 NAT 端口，不会被改写成 IPv4 地址。
IPv6 NAT 映射只在显式配置对应策略时创建，两个地址族的端口池隔离。

## 验收和后续范围

两套 DPDK 21.11.9 构建各 45/45 单测、十一项相关 AddressSanitizer/UBSan 检查及
单双队列正式 worker 验收通过。IPv6 最长前缀与独立线性参考对照，ACL 与独立解释器
对照；改写遍历全部 768 个分段点，包含双 VLAN、填充扩展头、奇数长负载、TCP/UDP/
ICMPv6 和共享复制失败。正式 worker 另验证 200 次 IPv6 在线路由切换、MARK/COUNT、
共享分段原包不变、IPv4 NAT 隔离及 mbuf 零泄漏。

```sh
meson test -C build --print-errorlogs
python3 tests/integration/ipv4_traffic.py --build-dir build
sudo python3 tests/integration/ipv6_traffic.py --build-dir build
sudo python3 tests/integration/ipv6_traffic.py --build-dir build-native
```

三 TAP 在自建空网络命名空间核对真实 TCP/UDP/ICMPv6 输出完整字节和校验和、扩展头、
VLAN、/0、/64、/65、/128 路由与直连邻居、范围 ACL 与批量更新、错误包和丢弃原因、
IPv4 SNAT 共存、混合快照重启及存储故障修复。只创建和清理自有虚拟接口，不改变
实体网卡、Linux IPv6 地址或系统路由。

IPv6 跟踪与 NAT66 已在[后续阶段](nat66.md)接通。动态 ND 已在[后续阶段](nd6.md)接通；RA/SLAAC、IPv6 分片重组、完整扩展选项、ICMPv6
错误生成、路径 MTU 发现、IPv6 隧道解析、硬件卸载和物理性能/长期稳定性目标仍未完成。
