# 常用隧道解析与中转边界

M2 在基础报文解析后增加独立隧道观察阶段，覆盖 VXLAN、Geneve 和 GRE。正式 worker
自动执行，不需要额外启动开关；原有 ACL、连接跟踪、NAT、路由和邻居仍使用外层字段。
本阶段交付解析与中转验证，不提供隧道终端、封装或解封装服务。

## 支持格式

| 格式 | 外层识别 | 提取内容 | 明确边界 |
| --- | --- | --- | --- |
| VXLAN | IPv4/IPv6 UDP，目的端口 4789 | I 位、24 位 VNI、内层以太网 | 接收时忽略保留位，未设置 I 位记不支持，不支持自定义端口或专用扩展解释 |
| Geneve | IPv4/IPv6 UDP，目的端口 6081，版本 0 | VNI、协议类型、0–252 字节选项、关键标志 | 逐项核对长度，未知非关键选项跳过，关键选项不产生有效内层字段 |
| GRE | IPv4/IPv6 协议号 47，版本 0 | 内层协议、C/K/S 标志、可选 key 和 sequence | 支持可选字段布局，不做旧版源路由、不跟踪序号、不核验 GRE 校验和 |

VXLAN 内层使用以太网；Geneve 和 GRE 支持以太网 `0x6558`、原始 IPv4 `0x0800`、原始
IPv6 `0x86dd`。内外层可分别带最多两层 VLAN，保存各自标签类型与 TCI，不混用两个域。
内层 IP 复用基础解析器提取地址、最终协议和 TCP/UDP 端口，不递归观察下一级隧道。
IPv6 扩展头沿用原有有界规则，内层分片只记录分片类型，不猜测端口。

格式依据 [VXLAN RFC 7348](https://www.rfc-editor.org/rfc/rfc7348.html)、
[Geneve RFC 8926](https://www.rfc-editor.org/rfc/rfc8926.html)、
[GRE RFC 2784](https://www.rfc-editor.org/rfc/rfc2784.html) 和
[GRE key/sequence RFC 2890](https://www.rfc-editor.org/rfc/rfc2890.html)。这里只实现上述解析子集，
不以此宣称符合完整隧道终端规范。

## 长度、内存与错误结果

`dppd_packet_parse_buffer()` 的原返回值继续表示外层解析结果，隧道结果独立保存在
`packet.tunnel.status`；`dppd_tunnel_parse_buffer()` 也可独立调用。元数据只保存数值副本，
不持有报文指针，不申请动态内存，不写入或释放 mbuf。

- UDP 隧道以 UDP 声明长度为界，GRE 以外层 IP 声明长度为界，不能借用以太网填充
- 外层分片不进入隧道解析，即使首片中恰好有完整隧道头，也不能当作已重组报文
- 线缆结构不足或 Geneve 选项越过声明范围，隧道结果为 `MALFORMED`
- 未知版本、未知协议、关键选项或观察窗口不足，隧道结果为 `UNSUPPORTED`
- `inner_valid` 只在内层基础解析成功时为真，不表示校验和有效、策略允许或可以解封装
- 基础解析和隧道观察不核验各层校验和，外层校验继续由原有 rewrite/conntrack 阶段负责
- mbuf 读取最多复制 1024 字节，覆盖支持范围内的双 VLAN、两侧 IPv6 扩展头、最大
  Geneve 选项及 TCP 头；长负载无需复制，分段位置不影响解析结果

当前是中转模式。未知关键选项、坏内层或无法解析的隧道不会单独导致丢弃；worker
继续按外层 ACL、路由、TTL/Hop Limit、校验和和状态处理。外层畸形报文仍在策略之前
丢弃。部署隧道终端时必须另行定义内层策略与错误处置，不能把本阶段当作内层防火墙。
内层地址、VNI 和 GRE key 也不作为当前连接跟踪/NAT 的隔离域。

## 管理与可观测性

管理协议升级为 v23，daemon 与 dppctl 必须同版本配套。`dppctl stats` 的汇总、端口和
队列查询，以及统计 telemetry，均增加以下字段：

```text
tunnel_vxlan_packets
tunnel_geneve_packets
tunnel_gre_packets
tunnel_malformed
tunnel_unsupported
```

前三项表示观察到对应外层识别条件的包数，包含其解析失败结果；后两项是这些包的
错误子集，不是实际丢弃数。ND 等待首包重试不重复累计。VNI、key 和内层字段当前供
内部解析 API 使用，CLI 不提供逐包查询。规则快照、路由持久化格式不变，动态统计重启清零。

## 验收

`test_tunnel` 覆盖 456 组双栈内外层、TCP/UDP、三种 VLAN、以太网/原始 IP 及 GRE
可选字段组合，并在每个字节位置拆分真实 mbuf 头部。另有所有短窗口、协议边界、
关键选项、坏内层、UDP 长度与以太网填充分离、252 字节 Geneve 选项、两侧 256 字节
IPv6 扩展头和长负载，以及两万次固定种子随机变异。

真实三 TAP 脚本在新建空网络命名空间执行，核对外层 IPv4/IPv6 转发、三种 VLAN、
完整输出字节、内层保持不变、外层 SNAT/SNAT66 双向还原、计数、关键选项和坏格式
继续按外层中转。内层地址专用拒绝规则验证不会误用于外层匹配。

```sh
sudo python3 tests/integration/tunnel_traffic.py --build-dir build
sudo python3 tests/integration/tunnel_traffic.py --build-dir build-native
```

整体验收入口见 [M2 软件验收](m2_acceptance.md)。物理网卡吞吐、硬件 tunnel offload、
VTEP 学习、隧道终端、重组、租户隔离及嵌套隧道策略均不在本次交付范围。
