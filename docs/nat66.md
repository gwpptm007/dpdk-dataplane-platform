# IPv6 连接跟踪与 NAT66

2026-10-17，M2 已在正式 worker 接通 IPv6 TCP/UDP 双向会话、状态 ACL、查询、清空、
超时回收与重启语义，并在此基础上接通有状态 SNAT66、静态 DNAT66 和端口转发。
IPv4 与 IPv6 共用会话容量和管理入口，地址族参与完整五元组索引，转换策略和端口池
分别隔离。原 IPv4 ACL、SNAT、DNAT 与双栈静态路由继续有效。

这里的 NAT66 是明确配置的有状态地址及端口转换，不是 NPTv6 前缀转换，也不表示
完整 IPv6 路由器、完整 NAT 行为矩阵或整个 M2 已完成。

## 先启用跟踪，再按需配置转换

在现有 EAL 参数后的应用参数中使用：

```sh
--ports 0,1 --ip-routing /private/routing.bin --conntrack 8192 --ct-timeout 300
# 按需追加：内口、外口、转换后的 IPv6 地址、端口池
--snat6 0,1,2001:db8:100::9,40000-60000
# 按需追加：外口、内口、协议、公网地址、公网端口、后端地址、后端端口
--dnat6 1,0,tcp,2001:db8:100::9,8443-8445,fd00:1::8,443-445
--dnat6 1,0,udp,2001:db8:100::a,any,fd00:1::9,same
```

`--conntrack` 默认禁用，启用后同时处理 IPv4 和 IPv6 TCP/UDP，容量 1–65536 为两种
地址族合计，不会各自额外获得一张同样大小的表。必须启用显式三层路由；默认超时
300 秒，范围 1–86400。不配置 IPv6 转换时，已有 IPv4 NAT 不会对 IPv6 应用转换。

`--snat6` 与 `--snat` 可同时配置。两个地址族的 TCP、UDP 各有独立端口池，相同数字
端口可以同时用于 IPv4 和 IPv6，也可以分别用于 TCP 和 UDP。同一个地址族和协议中，
动态公网端口独占。地址使用完整普通单播，不接受零地址、回环、组播、链路本地或
IPv4-mapped 地址，内外口必须是已配置的不同端口。

`--dnat6` 可重复，与 IPv4 `--dnat` 合计最多 32 条。支持单端口、等长端口段以及
`any` 配 `same` 的完整地址映射。端口段按相同偏移转换，禁止截断或取模。相同地址族、
协议、外口、公网地址不能出现重叠服务；同一后端入口也不能有无法区分的公网身份。
不同地址族不会误判冲突。与 SNAT66 同公网地址和外口的静态端口长期保留，清空或过期
只回收动态端口；任何一个协议的动态池被静态保留完全占满时拒绝启动。

策略在启动时固定，由部署保留启动参数，不写入规则或路由快照。运行时不提供策略
热更新。部署还需提供正确的双向路由、静态邻居和外部可达性，本程序不修改 Linux
地址、路由或实体接口。动态邻居已在 [ND 阶段](nd6.md)接通，RA 和 SLAAC 仍未提供。

## 状态 ACL 与提交顺序

```sh
./build/dppctl acl6-set 2100 0 0 10 ipv6 any any any any allow count state:new,established
./build/dppctl acl6-set 2101 1 0 10 ipv6 any any any any allow count state:established
./build/dppctl acl6-set 2200 0 0 100 ipv6 any any any any drop count
./build/dppctl acl6-set 2201 1 0 100 ipv6 any any any any drop count
# 静态入站服务还需允许外口 NEW，匹配转换后的后端地址和端口
./build/dppctl acl6-set 2102 1 0 5 tcp any fd00:1::8/128 any 443-445 allow count state:new
```

IPv6 支持 `new`、`established`、`invalid`、`untracked` 及其组合，沿用同一优先级、
MARK/COUNT、范围匹配、精确版本、两到四条原子批量更新和持久恢复。仅第一条命中
生效，未命中仍继续路由；连接跟踪本身不会自动建立默认拒绝防火墙。

TCP 与 IPv4 共用握手状态机：首个包须为无负载 SYN，核对 SYN/ACK 和第三次握手的
序号，支持握手重传、FIN、RST 和关闭回收。ESTABLISHED 表示已经接受过合法反向包，
并不等于 TCP 一定完成第三次握手，详细状态由 `tcp=syn/syn-ack/open/fin/closed`
表示。不实现完整窗口验证、同时打开或 Fast Open。

处理顺序为完整报文与校验和检查、候选会话、逻辑地址 ACL、路由与邻居、独占报文
改写、会话提交、原有发送批次。ACL 看 SNAT 前的内网身份、DNAT 后的后端身份，反向
流也使用相应逻辑地址；线缆地址和端口在最终放行时才修改。

ACL 拒绝、无路由/邻居、错误出口、Hop Limit 耗尽或复制失败都取消候选，既有状态
不刷新，新端口立即归还。已建立流的内外接口固定，路由改到另一接口时拒绝，须等待
过期或显式清空后重新建立。TX 排队失败仍按发送失败统计，不回滚已经观察到的有效
报文，会话计数不保证对端已收到。

SNAT66 回包须同时匹配外口、协议、远端完整地址/端口与已分配公网地址/端口。
DNAT66 回包从后端还原公网地址及端口。无映射回包、错误入口和静态后端无会话回包
均不能被重解释成新的出站 NAT 流；不支持 hairpin 和双向主动建连的一对一静态 NAT。

## 报文与校验和边界

继续使用 [IPv6 解析与静态转发](ipv6.md) 的有界扩展头、双 VLAN、静态路由和共享
报文保护。完整检查 TCP/UDP 伪首部及负载校验和后，再修改 IPv6 地址、端口、MAC 和
Hop Limit。增量更新参考 [RFC 1624](https://www.rfc-editor.org/rfc/rfc1624)，IPv6
伪首部及 UDP 非零要求参考 [RFC 8200 第 8.1 节](https://www.rfc-editor.org/rfc/rfc8200#section-8.1)。
UDP 输入零校验和拒绝，转换结果计算为零时编码为 0xffff。流标签、流量类别、填充
扩展头、TCP 选项、负载和以太网填充保持不变，旧 TX 校验和卸载标志清除。

多段、共享、间接或外部缓冲区沿用先检查整链、必要时完整复制的机制。分配失败不改
原包，MARK 仅写输出包。不支持的扩展头和全部分片，包括原子分片，均不建立会话。
纯跟踪模式的普通非 TCP/UDP IPv6 保持 UNTRACKED，普通 ICMPv6 可按原静态路由转发；
启用 IPv6 NAT 后只接受完整 TCP/UDP，不能把未实现转换的 ICMPv6 按原地址漏发。
IPv4 NAT 的启用不影响上述 IPv6 协议选择。

## 管理、重启与验收

此阶段使用 v21，当前 [ND 阶段](nd6.md) 扩展为 v22，daemon 和 CLI 必须配套，旧协议明确拒绝。原有 `conntrack-status`、
`conntrack-list [AFTER_ID [REVISION]]`、`conntrack-flush REVISION` 管理同一双栈集合。
每页四条，按不复用的会话 ID 排序，成员增删使旧修订号失效；流量计数和状态可继续
变化。列表增加 `family=ipv4/ipv6`，IPv6 会话地址使用方括号并完整显示转换前后端点。
`nat-status` 增加 `nat6` 配置行，`dnat-list` 同时返回两种地址族的静态规则。

超时沿用原有规则：未见回复或握手未完成最多 30 秒，FIN 最多 30 秒，RST 最多 2 秒，
均不超过基础超时。后台按预算回收，命中和查询也核对期限，时钟回退不提前过期。
会话、映射和计数不持久化，正常或异常重启后清空；规则 v2/v3、双栈路由 v1/v2 格式
保持不变，重放含状态规则时必须继续启用跟踪。迟到 UDP 在端口和完整五元组复用后
无法仅凭报文字段与新流区分，本阶段没有会话复制或跨重启无损迁移。

跟踪模式的坏校验和统一记 `ct_checksum_drops`，不支持协议记 `ct_unsupported_drops`，
错误出口记 `ct_zone_drops`，容量、池耗尽与无映射分别记 `ct_full_drops`、
`nat_pool_drops`、`nat_miss_drops`。IPv6 Hop Limit 和改写失败分别使用
`ipv6_hop_drops`、`ipv6_rewrite_drops`；ACL 先拒绝仍记 `rule_drops`。

```sh
meson test -C build --print-errorlogs
python3 tests/integration/ipv4_traffic.py --build-dir build
sudo python3 tests/integration/nat66_traffic.py --build-dir build
sudo python3 tests/integration/nat66_traffic.py --build-dir build-native
```

两套 DPDK 构建各 46/46 单测，AddressSanitizer/UBSan 构建也通过全部 46 项，
未启用 LeakSanitizer；内存池回收由正式 worker 的零 mbuf 泄漏检查单独核对。
验收覆盖双栈状态机、并发、128 位索引、容量、超时、分页/清空、SNAT66 与静态 DNAT66
双向映射、地址族/协议端口隔离、静态保留和失败回滚。独立完整校验和参考遍历全部
768 个分段点、双 VLAN、扩展头、TCP/UDP、奇偶负载及 UDP 零结果编码；正式单双队列
worker 核对共享映射、MARK、原共享分段包不变和 mbuf 零泄漏。真实三 TAP 分别验收
纯 IPv6 跟踪、NAT66 与 IPv4 SNAT 共存、状态 ACL/批量更新、完整线缆字节、错误路径、
SIGKILL 后策略重放和会话清空、老化与端口复用。测试只创建并清理自己的私有接口。

动态 ND 已在[后续阶段](nd6.md)接通；RA/SLAAC、RELATED/ICMPv6 错误转换、分片重组、NPTv6、hairpin、ALG、完整
TCP 窗口验证、会话 HA、硬件有状态卸载、隧道和物理性能/长期稳定性目标仍未完成。
