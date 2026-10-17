# IPv4 连接跟踪与出站 SNAT

2026-10-17，M2 的连接跟踪已先完成双向状态、超时、容量、管理和多队列闭环，随后接通
TCP/UDP 出站 SNAT 和对应回包的反向转换。正式 daemon、状态 ACL、路由、校验和、
查询、清空、资源回收和重启行为均已接通。此交付是静态启动策略下的 IPv4 软件基线，
不表示完整 NAT 家族或整个 M2 已完成。

后续已完成 [静态 DNAT 与端口转发](dnat.md)，包含整地址映射、单端口/端口段、
对应回包及与 SNAT 的端口保留。本页保留连接跟踪和出站 SNAT 的原始验收记录。

## 启动与配置

在现有 EAL 参数后的应用参数中启用：

```sh
# 已有 --ports 0,1 和 --ipv4-routing /private/routes.bin 时追加
--conntrack 8192 --ct-timeout 300
# 需要 SNAT 时再追加：内口、外口、公网地址、端口池
--snat 0,1,203.0.113.9,40000-60000
```

`--conntrack` 指定 1–65536 个会话的全局容量，默认禁用，要求显式 IPv4 路由。
`--ct-timeout` 是已建立会话的空闲秒数，默认 300，范围 1–86400。
`--snat` 要求已启用跟踪，内外口必须是配置集合中的两个不同端口，公网地址必须为
有效单播地址，端口池为包含端点的 1–65535 范围。NAT 不修改 Linux 接口地址、ARP 或
物理网卡；部署需自行提供正确的两向路由、静态邻居及外部可达性。

NAT 策略在本进程启动时固定，来自启动参数，需由部署配置保留；运行中不支持改池、
动态规则；静态端口转发通过另行声明的 `--dnat` 配置。规则与路由仍分别通过原有快照保存。运行时会话、映射和
流量计数不持久化，重启后清空，不把旧 TCP 流静默恢复成已建立状态。

跟踪模式强制所有规则经过软件处理，禁止硬件直转和不具软件等价语义的动作。
`require` 规则会拒绝；`prefer` 的等价入口规则选软件；持久化规则启动重放也检查这一点。
保存了 NEW/ESTABLISHED/INVALID 状态规则时，省略 `--conntrack` 会拒绝启动，不能
让依赖状态的 ACL 在无跟踪条件下看似恢复成功。

## 连接状态与 ACL

| 状态 | 含义 |
| --- | --- |
| UNTRACKED | 未启用跟踪，或纯跟踪模式中的非 TCP/UDP IPv4 报文 |
| NEW | 合法的新 UDP 流或 TCP SYN，尚无已接受的反向报文 |
| ESTABLISHED | 已有合法的双向观察，TCP SYN/ACK 已算回包，另有 TCP 协议状态检查 |
| INVALID | 无效握手、错误入口、无 NAT 映射或其他无法接受的跟踪尝试 |

`ESTABLISHED` 表示已见合法回包，不等于 TCP 三次握手已经全部完成。TCP 另外记录
`syn`、`syn-ack`、`open`、`fin`、`closed`。SYN/ACK 的 ACK 号和第三次握手的双方序号
必须与初始 SYN 对应，重传使用相同握手序号。中途首个 ACK、SYN/FIN、SYN/RST、FIN/RST
等矛盾标志拒绝；FIN 后该方向不再接受新负载，RST 后仅保留短期关闭观察。
握手语义参考 [RFC 9293](https://www.rfc-editor.org/rfc/rfc9293)。本实现不替代端系统的
完整 TCP 接收窗口、序号范围、重传计时、同时打开或 TCP Fast Open 处理。

ACL 仍然只执行排序后的第一条匹配，可增加状态修饰符：

```sh
# 内口允许创建连接和后续流量
./build/dppctl acl-set 1100 0 0 10 ipv4 any any any any allow count state:new,established
# 外口只允许已建立连接的回包
./build/dppctl acl-set 1101 1 0 10 ipv4 any any any any allow count state:established
# 明确拒绝其余 IPv4 流量
./build/dppctl acl-set 1200 0 0 100 ipv4 any any any any drop count
./build/dppctl acl-set 1201 1 0 100 ipv4 any any any any drop count
```

支持 `new`、`established`、`invalid`、`untracked` 的逗号组合，拒绝空项、重复项和未知
名称。状态条件可与地址、协议和端口范围同时使用，原子批量更新、版本和持久化沿用
[ACL](acl.md)。未配置 ACL 仍是未命中继续转发，连接跟踪本身不等于默认拒绝防火墙。

TCP/UDP 进入跟踪前验证 IPv4 头、TTL、完整传输层伪首部和负载校验和。坏包可以被
INVALID ACL 记录，但允许动作不能放行校验错误或无映射回包。IPv4 UDP 零校验和合法，
转换后继续保持零。跟踪模式明确拒绝所有 IPv4 分片；纯跟踪模式的其他 IPv4 协议为
UNTRACKED，SNAT 模式只接受完整 TCP/UDP，不把 ICMP 等未实现协议按内网原地址发出。

## 提交次序与双向转换

每个会话使用原始五元组、协议和入口端口定位正向，使用转换后的反向五元组和回包
入口定位反向。两个索引指向同一份状态，跨 RX 队列也只创建一个会话。
首版以表锁串行保护索引、候选状态和端口池，查询不会读到半次更新；当前没有分片锁、
无锁会话表或线速性能承诺。

1. 解析并校验报文，查找会话或准备尚未发布的候选
2. 回包先恢复逻辑内网目的地址，供 ACL 和路由查询，线缆数据暂不修改
3. ACL 允许后查路由、邻居和出口状态
4. 在独占副本中完成地址/端口、MAC、TTL 和校验和改写
5. 提交会话状态及观察计数，再交给原有发送批次

ACL 拒绝、无路由/邻居、出口错误或改写分配失败都会取消候选；既有会话不刷新，新的
临时端口归还。后续驱动 TX 排队失败仍计入发送丢弃，不回滚已经观察到的有效报文，
因此会话计数不是对端已收到报文的证明。

出站只转换内口源地址和源端口。回包必须匹配协议、外口、远端地址/端口和公网端口，
随后还原原内网目的地址和端口。同一原始流复用映射，不同远端建立不同条目；TCP 和
UDP 使用独立端口池，同协议内公网端口独占。外网不能通过无映射包创建 NAT 会话，
其他接口也不能冒用内外口身份。既有流的两个接口固定，路由改到其他接口会拒绝，
应等待过期或显式清空后新建，不会悄悄把既有会话改绑到另一出口。

IPv4 头校验和重算，TCP/UDP 校验和按 [RFC 1624](https://www.rfc-editor.org/rfc/rfc1624)
增量更新地址及端口的一补数差值，包含伪首部变化；UDP 非零校验结果为零时编码为
0xffff。VLAN、TCP 选项、负载和以太网填充保持不变。共享、间接、外部及多段 mbuf
复用独占副本机制，失败保留原包，旧 TX offload 请求清除，避免重复计算。
出站地址与端口转换的范围参考 [RFC 3022](https://www.rfc-editor.org/rfc/rfc3022)，
不宣称完整 RFC NAT 行为矩阵已实现。

## 超时、查询、清空和重启

未见回复或未完成握手最多空闲 30 秒，已建立会话使用 `--ct-timeout`，FIN 最多 30 秒，
RST 后最多 2 秒，均不超过配置的基础超时。每个有效且被接受的报文刷新对应空闲期限，
拒绝报文不刷新。后台和逐包路径按预算清理，到期流被查询或重新命中时也会核对期限；
空闲后台实际释放时间可能晚于期限，查询不会把过期条目当成有效会话。
满表补充扫描限速，避免每个新五元组强迫工作线程扫描整张大表。

```sh
./build/dppctl conntrack-status
./build/dppctl nat-status
./build/dppctl conntrack-list
./build/dppctl conntrack-list 4 9
./build/dppctl conntrack-flush 9
```

列表每页四条，按本次进程不会复用的会话 ID 排序，返回 `revision`、`next-id` 和
`more`。续页可携带精确 revision，成员增删后返回 ESTALE；状态和流量计数仍可继续
变化，不能把多页当作同一时刻的全表状态。时间为本次启动的单调毫秒，不是墙上时间，
不跨重启比较。清空必须指定当前修订号，错误版本不修改会话；清空归还映射，但不能
撤回已经进入发送批次的报文。

本阶段管理协议为 v18，后续静态 DNAT 扩展为 v19，daemon 与 CLI 须配套。
`stats`/telemetry 增加 `ct_invalid_drops`、`ct_full_drops`、
`ct_unsupported_drops`、`ct_zone_drops`、`ct_checksum_drops`、`nat_pool_drops` 和
`nat_miss_drops`。校验和失败在跟踪模式中统一归 `ct_checksum_drops`，TTL 沿用三层
计数。ACL 先拒绝时计入 `rule_drops`，会话状态命令另外记录资源失败累计值。

正常和异常重启都不保存会话，规则与路由按原版本恢复，NAT 配置需保留相同启动参数。
旧外口回包不会自动证明 ESTABLISHED，需由状态 ACL 或无映射检查拒绝。新连接可以
重新分配端口；端口复用后相同五元组的迟到 UDP 无法仅凭报文字段与新流完全区分，
本阶段不提供高可用会话复制、跨重启延续或无损迁移。

## 验收及边界

Ubuntu 22.04、DPDK 21.11.9 系统和原生 TAP 标识补丁构建均使用 `-Werror`，各 42/42
单测。六项相关 AddressSanitizer/UBSan 检查和单双队列正式 worker 验收通过：

- 双向状态、握手重传/错误 ACK、FIN/RST、取消候选、容量、索引复用、分页/清空和超时
- 四线程 40000 次双向命中同一会话，计数完整且没有重复分配
- NAT 同协议独占、TCP/UDP 池隔离、远端/入口校验、端口耗尽、取消及过期回收
- 改写遍历每个分段点、双 VLAN、填充选项、奇数负载和 UDP 零校验，以完整负载重算
  校验和独立对照，复制失败保留原包
- 正式一队列/两队列 worker 的跨队列共用映射、回包还原、共享原包不变和零 mbuf 泄漏

```sh
meson test -C build --print-errorlogs
sudo python3 tests/integration/conntrack_nat.py --build-dir build
sudo python3 tests/integration/conntrack_nat.py --build-dir build-native
python3 tests/integration/ipv4_traffic.py --build-dir build
python3 tests/integration/software_traffic.py --build-dir build
```

真实三 TAP 在自建空网络命名空间验收状态 ACL、UDP/TCP 握手、完整输出字节和校验和、
拒绝不建连/不刷新、无路由取消、错误报文、会话/端口池耗尽、分页/清空、空闲回收与
单端口池复用、SIGKILL 后会话清空与策略重放，以及禁用跟踪时拒绝恢复状态规则。
测试只创建和删除自己的虚拟接口，不改实体网卡。既有 ACL、二层及 IPv4 路由回归保留。

静态 DNAT/端口转发已在[后续阶段](dnat.md)接通。IPv6、RELATED/ICMP 错误转换、
分片重组、hairpin、ALG、完整 TCP 窗口验证、会话复制和硬件有状态卸载仍未实现。
它们不计入 TCP/UDP 跟踪及出站 SNAT 闭环，后续按独立目标推进。
