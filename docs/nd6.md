# IPv6 动态邻居发现

2026-10-17，M2 的 IPv6 邻居解析已接入正式 worker：NS/NA 收发、动态缓存、首包等待、
可达性检测 NUD、本机地址重复检测 DAD、管理查询和精确清空形成闭环。静态邻居仍优先，
原 IPv6 ACL、连接跟踪与 NAT66 保持有效。此交付覆盖显式三层路由下的以太网邻居发现，
不包含 RA、SLAAC、Redirect、默认路由自动学习、完整主机协议栈或整个 M2。

## 启用与作用域

在现有 EAL 参数后的应用参数中增加本机接口地址：

```sh
--ports 0,1 --ip-routing /private/routing.bin \
--nd6 0,fe80::10 --nd6 0,2001:db8:1::fe \
--nd6 1,fe80::20 --nd6 1,2001:db8:2::fe
```

`--nd6 PORT,IP6[,VID[,VID]]` 可重复，合计最多 32 个本机地址。本机地址只属于 DPDK
接口，不写入 Linux 地址或路由。只接受普通单播和链路本地单播，拒绝未指定、回环、
组播、IPv4-mapped 地址和未知端口。同一端口可有多个地址，但必须使用同一标签域，
不能重复地址或混用未打标签与 VLAN。启用前为该端口打开组播接收，驱动不支持时
明确启动失败；不要把 TAP 验收等同于所有物理 PMD 已验证。

省略 VID 表示未打标签；一个 VID 使用 0x8100；两个 VID 使用外层 0x88a8、内层
0x8100。VID 范围 1–4094，标签类型、层数和 VID 一起核对，PCP 不参与邻居身份。
控制报文的 PCP 为零，数据包原标签字节保持不变，不进行 VLAN 转换；数据实际标签必须
与出口的 ND 域匹配。每个端口只配置一个 VLAN 域，
不同域须使用不同 DPDK 端口或实例，不把同地址不同链路的邻居合并。

```sh
--nd6 0,fe80::10,37
--nd6 1,fe80::20,37,38
```

ND 不推断 IPv6 路由。继续用 `route6-set` 或双栈整表替换配置下一跳：普通网关使用
配置中的下一跳地址，`direct` 使用数据包目的地址解析。相同 `fe80::1` 可以分别在
不同端口解析。建议把每个端口的链路本地地址放在该端口其他地址之前，NS 使用该
端口第一个已配置并完成 DAD 的地址作为源。

NAT66 公网地址需要在相应外口额外声明为本机地址，才能回应相邻节点发给它的 NS：

```sh
--nd6 1,2001:db8:100::9 --snat6 0,1,2001:db8:100::9,40000-60000
```

只回应显式声明的本机地址，不为任意路由、后端或整段前缀代答，不提供 ND proxy。
IPv6 普通数据的本机目的处理、Echo 服务、ICMPv6 错误生成仍不属于本次交付。

## 收发与状态机

启用 ND 的实例在 ACL 和连接跟踪之前单独消费本链路的 NS/NA。控制报文不跨接口
转发，不进入 NAT，不创建 TCP/UDP 会话，也不受数据 ACL 的默认拒绝项拦截。
数据报文仍走完整 ACL、连接跟踪、路由与改写流程；启用 ND 时规则强制走软件，
硬件 require 请求拒绝，避免硬件入口对象先截走本机控制报文。

接收按 [RFC 4861](https://www.rfc-editor.org/rfc/rfc4861) 的 NS/NA 核心规则检查
Hop Limit 255、Code 0、完整 ICMPv6 伪首部和校验和、目标单播、源与目的作用域、
组播 IPv6/MAC 对应关系和本机单播 MAC。选项按八字节单位有界解析，零长度、截断、
重复或错误的链路地址选项拒绝；选项 MAC 必须是有效单播并与以太网源一致。普通未知
选项在长度有效时忽略，单个 ND ICMPv6 消息最多 256 字节。不支持的扩展头及全部
分片，包括原子分片，均不能用于学习，参见 [RFC 6980](https://www.rfc-editor.org/rfc/rfc6980)。

NS 只在目标为本机可用地址时学习其有效 SLLA，进入 STALE；DAD 的未指定源 NS
不能携带 SLLA。NA 只更新已经存在的邻居候选，未知通告不创建缓存。有效 Solicited
通告确认 REACHABLE；Override=0 的不同 MAC 不能替换旧值，Override=1 才按协议更新。
无 TLLA 且还没有有效 MAC 时不能把候选变成可用邻居。

| 状态 | 行为 |
| --- | --- |
| INCOMPLETE | 最多三次组播 NS，每次间隔一秒，等待地址解析 |
| REACHABLE | 可直接使用，确认期限到后变为 STALE |
| STALE | 保留 MAC，下一次使用进入 DELAY |
| DELAY | 继续转发，五秒后开始单播探测 |
| PROBE | 最多三次单播 NS，每次间隔一秒 |
| FAILED | 拒绝继续使用该 MAC，五秒后有新流量时重新解析 |

默认可达时间基数 30000 毫秒，每次确认在基数的 0.5–1.5 倍范围内随机取值。
`--nd6-reachable-ms N` 可配置 1000–3600000 毫秒。没有上层协议的可达性确认接口，
TCP 流量本身不代替 NA。STALE/FAILED 表项闲置满一分钟可回收。缓存最多 256 项，
满表不驱逐仍在使用的邻居，不把其他端口或 VLAN 的 MAC 借来转发。

## 首包、DAD 与生命周期

静态查表没有邻居时才查询动态缓存。未解析的数据包按邻居最多暂存四个，全实例
最多 128 个，最初等待期限为五秒；满队列明确丢弃，不持有无限数量的 mbuf。
NS 由零号工作队列定时发送，每个工作线程只写自己的 TX 队列；其他 RX 队列收到
有效 NA 后，同一缓存结果可唤醒各自的等待包。

等待包保留原始报文，候选连接和临时 NAT 端口先取消。解析成功后重新执行最新
ACL、NAT 和路由，不保留等待前的允许结果；等待期间也每 100 毫秒重查一次，使
静态邻居补齐或路由/ACL 修改及时生效。重试保持最初期限，不重新计入 RX 和 COUNT。
COUNT 归属第一次匹配的规则，后续策略重查不再累计其他规则。真正转发时才提交
连接状态，改写继续使用共享/多段报文的独占副本保护。

本机地址启动时为 TENTATIVE，先随机等待至多一秒，再发送一次源为 `::` 且没有
SLLA 的 DAD NS，等待一秒无冲突后变为 PREFERRED。有效冲突 NS/NA 使其变为
DUPLICATE，不能再用于发送，端口 IPv6 数据被阻断，`ready` 显示 `nd6-not-ready`。
清动态缓存不能解除本机地址冲突，需消除冲突并重新启动。已可用地址收到其他节点的
DAD NS 时回应发往 all-nodes 的 NA，不把对方的检查本身误判为自己的冲突。
规则参考 [RFC 4862 第 5.4 节](https://www.rfc-editor.org/rfc/rfc4862.html#section-5.4)，
不宣称已实现 SLAAC、增强 DAD nonce 或 SEND 认证。

ND 对象由 runtime 持有。停止工作线程后释放等待包，再关闭端口和内存池；设备移除
时不继续发送控制报文。正常或异常重启都清空缓存和等待队列，重新执行 DAD 和解析。
静态邻居、路由与 ACL 仍按原快照恢复，ND 学习不写盘、不改变路由 generation。
本机地址和 ND 定时参数来自启动配置，运行时不提供地址或 VLAN 域热修改。

## 查询、清空与验收

```sh
./build/dppctl nd6-status
./build/dppctl nd6-list
./build/dppctl nd6-list 8 12
./build/dppctl nd6-flush 12
```

管理协议为 v22，daemon 与 CLI 须配套。状态返回本机地址/DAD 状态、缓存与等待数量、
NS/NA 发送、无效控制报文、发送失败、队列丢弃及解析失败累计值。列表每页八项，按本进程
不复用的 ID 排序，返回端口、完整地址、MAC、状态、探测次数和单调时间。成员、MAC、
状态及 DAD 变化会推进 revision，续页和清空的旧版本返回 ESTALE，应重新查询。
清空只清动态缓存，不修改静态表和本机地址；被清除候选对应的等待包会取消。

`stats` 增加 `nd6_rx_packets`、`nd6_tx_packets`、`nd6_queued`、`nd6_queue_drops` 和
`nd6_not_ready_drops`。控制报文的真实收发同时计入总 RX/TX，首包重试不重复统计。
`nd6_queued` 为成功入队次数，包含等待期间的再次入队，不是独立数据包数量。

```sh
meson test -C build --print-errorlogs
python3 tests/integration/ipv4_traffic.py --build-dir build
sudo python3 tests/integration/nd6_traffic.py --build-dir build
sudo python3 tests/integration/nd6_traffic.py --build-dir build-native
```

两套 DPDK 构建各 47 项单测，AddressSanitizer/UBSan 构建也运行全部 47 项。
专项单测使用独立校验和、可控时钟覆盖 NS/NA、DAD、状态转换、Override、容量、
分页/清空和有界等待。正式单双队列 worker 检查另一队列收到 NA 后唤醒共享分段
首包、NAT66、原包不变和 mbuf 零泄漏。真实三 TAP 覆盖三种标签域、纯跟踪与 NAT66、
完整首包字节/校验和、COUNT、等待期间 ACL 更新、静态优先、线上 MAC 变化、NUD
重试耗尽、队列上限、DAD 冲突和崩溃后重新学习。测试只操作自建空网络命名空间
中的虚拟接口，不修改实体接口、Linux IPv6 地址或系统路由。
