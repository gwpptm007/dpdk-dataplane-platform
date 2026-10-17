# M2 软件数据面验收

本轮按 [路线图](roadmap_v3.md) 的原始 M2 交付项逐一核对，不将硬件卸载、生产性能或
所有可能的网络协议扩展混入软件阶段。验收表只在对应代码和测试通过后标为完成。

2026-10-18，原始 M2 软件交付已全部通过阶段验收。系统和本地 TAP 适配 DPDK 21.11.9
两套构建各完成 12 步完整验收，ASan/UBSan 构建完成全部单测与正式 worker 核心验收。
三份报告对应相同 210 个源码文件，和已提交的 `ce041c6` 源码内容一致。

| 交付项 | 当前证据 | 本轮工作 |
| --- | --- | --- |
| 独立 exact match | 完整 IPv4/IPv6 TCP/UDP 五元组哈希，与 ACL 统一排序 | 已通过本轮独立及组合验收 |
| 独立 ACL | 任意地址掩码、端口范围、状态条件、独立 IR 参考对照 | 已通过本轮独立及组合验收 |
| LPM、rewrite、neighbor | 双栈路由、静态邻居、IPv6 ND、跨段写时复制 | 已通过本轮独立及组合验收 |
| conntrack/NAT | 双栈 TCP/UDP、SNAT、静态 DNAT、端口转发 | 已通过本轮独立及组合验收 |
| 批量计数 | 工作线程收发统计与规则 COUNT 都按批归并 | 已通过本轮独立及组合验收 |
| IPv6 与常用隧道解析 | IPv6、VXLAN、Geneve、GRE 有界解析 | 已通过本轮独立及组合验收 |
| 并发与异常语义 | QSBR、原子规则批次、分片拒绝/透传、校验和与老化测试 | 已通过本轮独立及组合验收 |
| 软件 QUEUE | 明确拒绝软件执行，硬件计划仍需 PMD validate | 不等价决定和三策略/三模式矩阵验收完成 |

## 验收范围

- 每个阶段有独立单元或属性测试，精确匹配、ACL、LPM 的结果与独立参考核对
- 规则条件、动作和计数器随同一不可变快照发布，旧读者通过 QSBR 保护
- 正式 worker 覆盖单队列与多队列、IPv4/IPv6、ACL、NAT 与邻居的组合
- 分片、校验和、多段 mbuf、会话老化和容量耗尽具有可复现的行为
- 系统与本地适配 DPDK 构建通过全量单测，sanitizer 和真实 TAP 流量验收通过
- 新增代码有通俗中文注释，通过项目格式检查，文档说明部署及限制

## 阶段外边界

IPv4 动态 ARP、RA/SLAAC、Redirect、完整主机协议栈、分片重组、ICMP RELATED/NAT 错误报文转换、
hairpin、NPTv6、动态 NAT 策略变更和会话 HA 不属于本轮原始 M2 必交付范围。
隧道交付是解析阶段，不自动包含封装/解封装、VTEP 控制面或内层规则硬件卸载。
物理网卡吞吐、P99 和 SmartNIC/DPU transfer 继续由 M3/M4 对应验收负责。

## 阶段与证据对应

| 阶段 | 独立测试或参考 | 组合验证 |
| --- | --- | --- |
| exact match | `test_exact` 四万次独立 IR 对照、同键排序和冲突 | `test_software_backend` 精确与通配优先级，正式规则收发 |
| ACL | `test_acl` 直接解释双栈 IR，核对掩码和范围 | 在线规则切换、允许/拒绝、MARK/COUNT、状态 ACL |
| LPM/静态邻居 | `test_ipv4_router` 线性参考、双栈 `test_ipv6_control` | 路由与邻居同版本发布、持久恢复、实际出口和 MAC |
| 动态邻居 | `test_nd6` 可控时钟、DAD/NUD、VLAN 和容量 | 三 TAP NS/NA、跨队列首包、ACL 更新与 NAT66 |
| rewrite | `test_ipv4_rewrite`、`test_ipv6_packet` 独立完整校验和 | 单双队列共享、间接与外部多段包，原始引用保持不变 |
| conntrack/NAT | `test_conntrack`、`test_dnat`、`test_nat66` | 双向状态、端口池隔离、失败不提交、容量、老化及崩溃恢复 |
| 计数 | `test_stats`、`test_software_count_batch` | 规则版本、并发增量、入口/出口汇总、ND 不重复计数 |
| 隧道 | `test_tunnel` 组合、截断、分段和随机变异 | VXLAN/Geneve/GRE 中转、外层 NAT、内层不改写 |

规则条件、动作和计数对象共用一个快照，exact 与 ACL 不分别发布；路由与静态邻居
也以同一版本发布。规则表和路由表仍是独立控制对象，不承诺跨这两张表的整条 pipeline
事务。动态会话及 ND 使用各自的同步和生命周期，不能把规则发布当作会话快照发布。

## 异常处理约定

| 情况 | 明确行为 |
| --- | --- |
| 外层头截断、结构损坏 | 在软件策略之前丢弃，不使用未解析字段 |
| 二层端口对遇到分片 | 不重组、不提取端口，可由以太网/IP/协议条件处理，否则沿端口对中转 |
| 无跟踪的 IPv4 路由遇到分片 | 逐片查路由和邻居、修改 MAC/TTL/IPv4 头校验和，不修改片内传输层 |
| IPv6 路由或双栈跟踪遇到分片 | 明确拒绝，包括 IPv6 atomic fragment，不按完整 TCP/UDP 建会话 |
| 校验和 | 基础解析不验证；IPv4 路由验证头校验和，IPv6 路由验证支持的传输协议校验和，TCP/UDP 跟踪验证完整传输校验和；IPv4 UDP 零校验允许，IPv6 UDP 零校验拒绝 |
| 不支持的扩展头/选项 | 解析与三层阶段明确标记或拒绝，不猜测字段；隧道观察的错误按 [中转边界](tunnels.md) 独立记录 |
| 路由或邻居未命中 | 不回退较短前缀或固定端口对；显式 ND 只在缺少静态 IPv6 邻居时有界等待 |
| 改写或复制失败 | 保留原有共享引用，失败包回收，不创建成功会话或占住 NAT 端口 |
| 发送不足/出口不可用 | 未交给驱动的包立即释放并计数，不隐式无限重试 |
| 会话与邻居老化 | 有界回收会话和映射；ND 重新探测，超时或队列满时释放等待包 |
| 规则更新/保存失败 | 发布前失败保留旧视图；持久化不确定状态显式上报并按既有 dirty/修复语义处理 |

规则和路由可持久恢复，会话、NAT 映射、ND 动态邻居与统计重启清空。新规则版本
COUNT 从零开始。完整边界继续分别见 [双栈转发](ipv6.md)、[连接跟踪](conntrack_nat.md)、
[静态 DNAT](dnat.md)、[NAT66](nat66.md) 和 [ND](nd6.md)。

## 本轮验收记录

2026-10-18，独立 exact match 已接入正式规则快照，不增加第二次发布。完整双栈
TCP/UDP 地址与端口进入哈希表；通配、范围、状态或额外协议条件继续使用 ACL。
两阶段按 group、priority、ID、原槽位选择同一唯一动作，只有胜出规则累计 COUNT。
分片和错误地址族不进入精确键；冲突链、同键排序、满表及不支持条件均有测试。

四万组随机样本直接解释原 IR，与精确表结果逐项核对。系统 DPDK 21.11.9 与本地
适配构建各 48/48 单测通过，ASan/UBSan 全部 48 项通过；两套构建的正式 worker
二层原子更新、双栈 ACL/转发、SNAT/DNAT/NAT66、跨队列 ND 等待首包与多段共享包
回归均通过，内存池归还完整。该次 exact match 交付时尚未计入批量计数和隧道。

同日完成规则 COUNT 批量归并。每个正式接收批次先按计数对象累加，最多保留 32 个
不同版本对象，满时提交后继续；处理完全部包后在发送和 QSBR 静默前提交。没有命中、
没有 COUNT 和 ND 重试不会增加计数；DROP 或后续转发失败仍保留规则命中。控制面查询
可以暂时看不到尚在处理的接收批次，两个统计字段仍独立采样，不提供整批原子读数。

新增测试覆盖 256 次同版本合并、40 条规则跨容量提交、不同长度、重复 flush、ND 重试、
更新期间新旧版本指针保护与两个线程同时累计两万次。两套构建各 49/49 单测、ASan/
UBSan 全部 49 项、两套正式二层和双栈 worker 回归，以及 sanitizer 正式双栈 worker
全部通过，未发现 mbuf 泄漏。该次 COUNT 交付时尚未结束隧道和最终整体验收。

同日完成 [VXLAN/Geneve/GRE 解析](tunnels.md)、管理 v23 和统一验收入口。456 组隧道
组合、每字节分段、截断、最长支持头链、两万次随机变异，以及三 TAP 外层中转/NAT
和内层保持不变通过。QUEUE 的软件不等价决定和拒绝矩阵一并验收。

## 最终验收结果

| 构建 | 全量单测 | 正式 worker | 真实 TAP |
| --- | --- | --- | --- |
| 系统 DPDK 21.11.9 | 50/50，无失败、跳过或超时 | 二层/双栈单双队列全部通过 | 八组脚本全部通过 |
| 本地 TAP 适配 DPDK 21.11.9 | 50/50，无失败、跳过或超时 | 二层/双栈单双队列全部通过 | 八组脚本全部通过 |
| ASan/UBSan DPDK 21.11.9 | 50/50，无失败、跳过或超时 | 二层/双栈单双队列全部通过 | 本次未重复执行 TAP，完整 TAP 由前两套覆盖 |

两套完整运行的十二步分别为构建、单测、二层 worker、双栈 worker、IPv4 路由、ACL、
连接跟踪/SNAT、DNAT、IPv6、NAT66、ND 和隧道。报告均成功且源码未变化；sanitizer
报告明确为四步核心验收，没有伪装成含 TAP 的完整报告。另行实测统一入口超时后的
同进程组清理，拒绝正常退出的自建子进程也不会继续运行。

源码摘要表按键排序、紧凑 JSON 编码后的 SHA-256 为：

```text
7834ce634b16b274ce80e037f8fcd73f16e43a5b0d4bd4ccdbee6470a8855840
```

三份 `report.json` 与当前源码逐项核对一致。测试后没有遗留 `dppd` 进程，八组 TAP
脚本均确认私有命名空间接口已清理，宿主仅保留原有 lo、ens192、ens33、ens34。
项目 167 个 C 源码和头文件通过 clang-format 检查，新增中文注释结尾和文档链接检查通过。

本项完成的是原始 M2 软件能力及上述明确边界。阶段外能力仍按本文限制登记，不能将
本结果推广为完整 RFC 路由器、隧道终端、硬件卸载或生产性能验收。

## QUEUE 等价语义决定

硬件 flow `QUEUE` 在接收侧选择 RX 队列，影响 worker 归属和队列观察。当前软件
worker 已经从固定 RX 队列取到报文，选择某个 TX 队列不能补回原来的接收侧行为。
跨 worker 再分发还涉及顺序、所有权、背压和队列统计，当前没有提供这套语义。

因此本阶段不实现软件 QUEUE。`SOFTWARE_ONLY` 明确拒绝，`PREFER_HARDWARE` 也不将
QUEUE 降级到软件；有候选硬件时继续由 PMD validate 确认，失败必须返回错误。启用
连接跟踪或 ND 时强制经软件的规则同样不能绕开此限制。新增 planner 矩阵核对三种
策略、硬件有无及普通/跟踪/ND 模式，不把 RX 队列语义悄悄换成 TX 队列选择。

## 可重复执行的验收入口

先按 [构建说明](build_and_run.md) 配置 `tests=true`、`werror=true`。完整验收要求 Linux
root 和至少三个可用 CPU，运行时只在脚本自建的空网络命名空间中创建和清理 TAP。
物理网卡、宿主路由、现有接口及 DPDK 设备绑定均不改动。

```sh
sudo python3 scripts/verify_m2.py --build-dir build --tap
sudo env LD_LIBRARY_PATH=/path/to/native/lib:/path/to/native/pmds \
  python3 scripts/verify_m2.py --build-dir build-native --tap
meson setup build-asan -Dtests=true -Dwerror=true -Db_sanitize=address,undefined -Db_lundef=false -Db_pie=false
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 scripts/verify_m2.py --build-dir build-asan
```

没有 `--tap` 的运行只报告核心验收，JSON 中 `complete_m2_validation=false`，不冒充完整
验收。ASan/UBSan 核对全部单测和正式二层/双栈 worker；当前 DPDK 夹具关闭 LeakSanitizer，
实际 mbuf 归还另由 worker 的内存池计数核对，不把这一点描述为启用了全部泄漏检测。

入口依次构建、全量单测、单双队列正式收发，再执行 IPv4 路由、ACL、连接跟踪/SNAT、
静态 DNAT、IPv6、NAT66、ND 和隧道的真实 TAP 验收。任一失败、超时、单测跳过或源码
在验收期间变化都会使结果失败，不会继续报告整体验收通过。

每套构建在 `m2-acceptance/` 保存逐步日志和 `report.json`，含命令、返回码、耗时、DPDK
依赖信息及源码 SHA-256。报告是运行产物，不加入版本库；最终阶段记录保存在本文。
