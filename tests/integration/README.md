# 集成验收

## 规则失败分类、事件历史、历史耗时与 Telemetry

```bash
python3 tests/integration/rule_telemetry.py --build-dir build
sudo python3 tests/integration/rule_telemetry.py --build-dir build --tap
python3 tests/integration/recovery_isolation.py --build-dir build
```

当前管理协议 v15。真实 ring/TAP 进程查询 DPDK telemetry v2 socket，核对分类、
原始/补偿/最终错误、生效与保存失败、累计值、64 ID 分页和最大 uint64 ID。
72 条规则下并发查询与 20 次软件整批更新，验证完成态关系、过期分页、只读 COUNT/
快照/画像以及重启指标重置。历史耗时逐字段对齐 CLI 与 JSON，软件整批二十次发布
只增加二十个样本和四十条涉及规则，删除保留历史，重启只记录仍需重放的规则。
两组在线隔离与一组启动重放隔离验证未启动或已停止 worker 时仍可诊断，已撤销的
成功创建保留耗时历史，清理重试不抹去样本，从旧快照正常重启后重新累计。
失败事件逐字段对齐 CLI 与 JSON，连续 130 次失败后保留最近 64 条，按同一历史版本
完整读取 16 页；验证过期版本、覆盖缺口边界、最大游标、成功不清除与重启清空。
并发查询覆盖历史窗口变化，三组隔离验证首个补偿错误保留、失败重试新增事件与隔离可读。
2026-10-09，25/25 单测和四个相关单测的 AddressSanitizer/UBSan 通过，
既有状态/健康/四组 worker 回归通过。
只创建测试自己的虚拟端口，ring/TAP 的 1024/8191 个 mbuf 全部归还。
详见 [规则失败与 Telemetry](../../docs/rule_telemetry.md)、[失败事件历史](../../docs/rule_history.md)
与 [历史耗时分布](../../docs/rule_latency.md)。

## 网卡能力画像与探测缓存

```bash
python3 tests/integration/capability_probe.py --build-dir build
sudo python3 tests/integration/capability_probe.py --build-dir build --tap
```

该功能最初在 v12 验证，当前管理协议 v15。默认场景由普通用户运行 ring 进程，检查启动身份和固件未知、画像
只读、完整规则键、缓存命中/刷新/清空/实际过期、require 新校验与 prefer 等价降级，
以及探测不安装、不修改版本/COUNT/快照和重启建立新缓存。`--tap` 单独验证需 root
的真实 TAP 校验与创建/更新/删除、跨端口失效和平台软件对象共存。测试只创建临时
虚拟端口，退出后 socket、接口和 ring/TAP 的 1024/8191 个 mbuf 全部回收。

2026-10-03 两组场景、21/21 单测和三个相关单测的 AddressSanitizer/UBSan 通过。
规则状态 2/4 条、两组恢复隔离、ring/null 健康与线程故障、四组正式 worker 收发
回归通过。隔离允许只读画像，探测与清缓存返回 EUCLEAN。能力矩阵与物理卸载性能
不属于本轮验收，详见 [能力画像与验证](../../docs/capability_probe.md)。

## 规则安装状态

```bash
python3 tests/integration/rule_status.py --build-dir build
sudo python3 tests/integration/rule_status.py --build-dir build --tap
```

当前管理协议 v15。普通用户场景使用独立 ring 端口，验证 2/4 条规则的只读状态、prefer
降级、共享整批耗时、旧版本拒绝、dirty/flush、重放和删除。`--tap` 单独执行需要 root
的真实 rte_flow 场景，验证创建、更新安装端口、删除、平台软件对象共存和快照重放。
临时 TAP 和 8191 个 mbuf 在退出后全部回收，不操作物理网卡。

2026-10-03 三组场景及 20/20 单测通过。四组 `software_traffic.py` 同时验证 COUNT
不会被状态查询清零，以及分配失败后的旧安装记录保持。隔离回归确认新增查询也被
拒绝。耗时只代表后端提交，TAP 的 rte_flow 记录不证明物理硬件卸载，详见
[规则安装状态与验证](../../docs/rule_status.md)。

## 健康与就绪

```bash
python3 tests/integration/health_readiness.py --build-dir build
sudo python3 tests/integration/health_readiness.py --build-dir build --tap
```

当前管理协议 v15。自动检查 `health/ready` 的输出与退出码、实际 worker 状态、只读查询、
快照写入失败与 flush 修复、非空两规则重启，以及注册失败后的主线程故障监控。
单队列使用 `net_ring`，双线程使用支持正式配置要求的 `net_null`；TAP 模式只操作
测试自己创建的两个临时接口，验证 down/up 的未就绪与恢复、快照不变和退出清理。
TAP 预分配接收队列，因此池容量为 8191，其余虚拟端口场景为 1024。

2026-10-03 三组状态/存储恢复和两组线程注册故障通过，19/19 单测、四组 worker
收发和既有隔离/移除/链路错误回归通过。测试包装函数不链接到正式 daemon，所有
故障环境限定于独立测试进程。该结果不证明卡死检测、物理 RSS、端到端可达或物理
热拔插，详见 [健康与就绪验证](../../docs/health_readiness.md)。

## 双端口软件路径

测试拓扑需要一台流量发生主机的两个接口分别连接 dppd 的端口 0 和 1。DPDK 端口已绑定给用户态后，不能在同一端口上直接用 Scapy 发包。

1. 运行 `dppd ... -- --ports 0,1 --queues N --promisc`。
2. 在流量主机执行 `sudo python3 cross_connect.py <tx-iface> <rx-iface>`。
3. 反向交换接口再执行一次。
4. 对 VLAN、IPv4 fragment、TCP、未知 EtherType 和多尺寸包重复测试。
5. 核对 `/dppd/stats`、端口 xstats、丢包和包内容。

## SmartNIC/DPU

每个 PMD/固件组合必须记录：EAL devargs、端点清单、switch domain、逐条 `rte_flow_validate` 结果、规则创建/查询/删除结果、CPU RX 是否仍看到命中流量、规则规模和重启恢复行为。不能把一个设备的 match/action 支持外推到另一设备。

## 跨规则更新：net_ring 管理闭环

自动验证：`python3 tests/integration/batch_update.py --build-dir build`。脚本以普通用户
启动独立实例，检查 2/4 条更新、旧版本拒绝、相同内容更新、容量不足、snapshot 状态和
退出清理；可用 `--lcores 2,3` 指定可用核。2026-09-26 已通过，见
[验证记录](../../docs/validation_batch_update.md)。下面保留手工操作步骤。

先执行 `bash scripts/build.sh`，确认新增 `atomic batch update`、`dppctl batch update arguments`
和原有单测全部通过。在没有同名实例运行时启动独立测试进程；lcore 编号按测试机调整：

```bash
sudo ./build/dppd -l 0-1 --no-huge --no-pci -m 64 \
  --file-prefix=dppd-batch-update \
  --vdev=net_ring0 --vdev=net_ring1 -- \
  --ports 0,1 --queues 1 --mbufs 1024 --cache 64 --rule-capacity 4 \
  --control-socket /tmp/dppd-batch-update.sock
```

另一个终端执行以下命令；需要相同 socket 访问权限。不要复用已有 desired state：

```bash
ctl() { sudo ./build/dppctl --socket /tmp/dppd-batch-update.sock "$@"; }
ctl apply-drop-batch 0 700 701
# 应返回 generation 1/2；net_ring 不支持 rte_flow，prefer 应以 software 回退。
ctl update-drop-batch 0 20 prefer 700 1 701 2
# 应返回 generation 3/4、相同非零 transaction；完整规则 priority 均为 20。
ctl get 700
ctl get 701
ctl update-drop-batch 0 30 prefer 700 1 701 2
# 上一条应以 ESTALE 失败；get 仍为 generation 3/4。
ctl delete-batch 700 3 701 4
ctl list
# 应为空，repository-generation 为 6。最后 Ctrl-C 退出 daemon，检查 socket 已清理。
```

可另起 `--rule-capacity 2` 的空实例验证满容量更新：创建两条软件规则后，即使仅剩一个
临时槽，整批快照替换仍应成功，`get` 返回 generation 3/4。脚本覆盖 2/4 条规则的
宽裕容量与满容量四种组合。PMD create/delete 及补偿故障由 `test_batch_update` 注入；
`test_software_batch` 使用并发读者和解析后的测试报文检查整批软件视图，并注入分配失败。
net_ring 管理闭环本身没有注入报文，不作为实际收发或物理硬件 offload 的证明。

## 纯软件批量更新：实际 RX/TX

```bash
meson compile -C build
python3 tests/integration/software_traffic.py --build-dir build
# 默认验证单队列基线与双队列，需要三个可用 CPU（主线程加两个 worker）
# 只有两个可用 CPU 时可单独运行基线：
python3 tests/integration/software_traffic.py --build-dir build --queues 1
```

需要 DPDK 的 `rte_net_ring` 开发库，默认三个可用 CPU。构建时找不到该库则跳过此测试程序，
不影响其他目标；启动脚本会明确报告缺少程序。脚本自动从 CPU affinity 选择 CPU，
使用独立 file-prefix、`--no-huge --no-pci`，单次运行限时 60 秒，不绑定物理网卡。

测试通过 `rte_eth_from_rings()` 创建独立 RX/TX 队列，使用正式 runtime、worker、
mbuf 解析、软件分类和控制服务，生产代码不替换。两个目标 UDP 端口始终应被 DROP，
更新时交换规则的匹配端口，若存在部分替换导致的规则缺口，出口会收到本应丢弃的报文。
第三个 UDP 端口不匹配规则，必须从正确的配对出口转发，并检查报文长度、入口标识及统计。

覆盖两条和四条规则的满容量更新，各在收包期间更新 1000 次；随后逐一注入副本、
计数器和退役记录分配失败，检查旧版本与 COUNT 继续生效。最后重试成功，验证新版本
计数归零，再发包验证 COUNT。停止线程后检查 RX/TX 队列清空、mbuf 池空闲数恢复。
统计部分还核对端口/队列 RX、TX、字节、丢弃原因和管理查询；额外触发畸形、无路由、出口异常及发送环满，各原因归属正确。故障注入只链接到测试程序，不进入 daemon。这是虚拟 PMD 实际收发测试，不是吞吐基准
或物理硬件卸载验证；命中软件规则的终止动作仍为 DROP，不宣称验证 MARK 报文转发。

双队列测试在每端口创建两个独立 RX/TX ring，分别由两个正式 worker 处理。队列 0
每轮 24 包、队列 1 每轮 48 包，核对每个端口和队列、各队列合计及整体统计。
报文负载包含入口、队列和序号，出口逐包检查，检测串队列、错误出口、重复和遗漏。
更新失败后每条规则每轮准确新增 24 hits/1536 bytes；恢复后的计数也从零重新验证。
停止两个 worker 后确认 QSBR 退役链表回收，所有队列清空且 mbuf 池恢复。

`net_ring` 不提供 RSS，而生产设备初始化要求多队列端口具备 RSS。因此测试先完成
单队列 runtime 初始化，再仅在测试程序中以 mq_mode=NONE 显式配置虚拟 PMD 的两队列，
绑定第二个正式 worker；生产代码不修改。这验证独立队列上的 worker、分类、统计及
生命周期，不证明 `dppd --queues 2` 可直接用于 net_ring，也不验证硬件 RSS 分流。

## 外部 UDP/TCP 发包端与 RSS 验收

`rss_sender.py` 使用 Linux 自带 Python 的原始以太网 socket，无需安装 Scapy 或
tcpreplay。默认发送 4096 包、64 个不同 UDP 源端口的流，每秒 1000 包，负载含流
编号和序号。`--protocol tcp` 可构造带相同负载的 TCP ACK 测试报文，不建立 TCP
连接；`--vary-source-ip` 按流编号递增源 IPv4，不给网卡添加这些地址。
IPv4/UDP/TCP 校验和完整，不修改网卡地址或路由，不绑定接收端网卡。

先在接收端仍由 Linux 管理数据口时确认二层连接，明确指定数据口 MAC：

```bash
sudo python3 tests/integration/rss_sender.py --interface ens160 \
  --destination-mac 00:0c:29:f8:f6:82 --probe-only
```

定向 ARP 必须收到指定 MAC 的响应；Linux 可能从管理口回答其他接口的 IP，
仅 ping 通目标 IP 或收到广播 ARP 响应不足以证明数据口可达。
完成探测并让 DPDK 接管接收端数据口后，才使用：

```bash
sudo python3 tests/integration/rss_sender.py --interface ens160 \
  --destination-mac 00:0c:29:f8:f6:82 --receiver-dpdk
```

`--receiver-dpdk` 跳过 ARP，因为 DPDK 收包程序可能不回答 ARP；该参数不是连通性
证明。`--dry-run` 只构造报文，不打开原始 socket、不发包。`SENT` 仅表示发送端
提交成功，RSS 验收还需接收端逐队列统计、序号和丢包证据。上述 MAC 是本次
VMware 测试环境数据口，其他环境必须替换。多流并不保证 PMD 将包分到不同队列。

2026-10-02：发送工具已放在 `.134` 的 `/tmp/dppd-rss-sender.py`。
最初发送端数据口 `ens160` 使用桥接网络，指定接收端数据 MAC 的定向 ARP 失败；
广播探测在管理口收到的其他 MAC 响应不作为数据口连通证据。
现已将发送端 `ethernet1` 在线切换到接收端数据口使用的 `VMnet3`，禁用桥接链路
状态传播，并保存到 VMware 配置。原 VMX 配置在虚拟机目录保留带时间戳的备份。
Linux NetworkManager 新建 `dppd-data-vmnet3` 自动连接配置，数据口地址为
`192.168.100.2/24`，不提供默认路由；原桥接连接配置保留。
管理口 `192.168.65.134/24` 保持可用，默认路由通过管理口。
接收端 `.135` 的 `ens192` / `192.168.100.1/24` 及管理口配置未修改。

发送端 dry-run，以及流编号 0/63/4095、序号 0/65535/65536 的报文长度、
IPv4/UDP 校验和、端口和负载检查通过；不连通时探测明确失败退出。
切换后指定数据 MAC 的探测成功，数据口 ping 3/3、零丢包。
接收端临时 Python UDP socket 绑定 `192.168.100.1:10000`，实际收到 256 个包、
64 条流，源地址、源端口、流编号、序号和负载全部匹配，无遗漏和重复，随后关闭。
该收包证据来自 Linux 内核网络路径；尚未验证 DPDK RSS 分流。配置已保存，
本轮未重启虚拟机验证重启恢复。

`test_rss_receive` 是独立的真实 PMD 双队列验收程序，随 `-Dtests=true` 构建。
仅允许一个 ethdev，必须用 EAL `-a` 指定数据口；管理口不得绑定给 DPDK。
测试要求 IPv4 RSS，采用与平台设备初始化相同的 IP/UDP/TCP 能力交集，
使用 PMD 默认 RSS key/RETA，轮询两个 RX 队列。测试程序额外启用 PMD 支持的
接收校验和、RSS hash offload 和 RX 队列中断；它不运行平台 worker 或转发。
期望源 IP 从 `192.168.100.2` 按流编号递增，发送端必须指定 `--vary-source-ip`。

在管理员完成数据口绑定、准备内存并安排退出后恢复原驱动及地址后，启动：

```bash
sudo ./build/tests/integration/test_rss_receive -l 0 -a 0000:0b:00.0 \
  --file-prefix=dppd-rss-check
```

看到 `RSS_RECEIVER_READY` 后，在已通过数据 MAC 探测的发送端执行：

```bash
sudo python3 tests/integration/rss_sender.py --interface ens160 \
  --destination-mac 00:0c:29:f8:f6:82 --receiver-dpdk \
  --vary-source-ip --protocol tcp --packets 4096 --flows 64 --rate 1000
```

验收限时 60 秒，要求全部 4096 包、两个队列均非空、RSS hash 标记齐全，且
每条流的队列及 hash 稳定。检查地址、端口、负载及序号，无重复或遗漏才成功。
2026-10-02 接收程序在 `.135` 的 DPDK 21.11.9 上通过 `-Wall -Wextra -Werror`
编译；独立目录 `/tmp/dppd-rss-validation/source` 内全项目 Meson `-Dwerror=true`
构建及 16/16 单测通过。无网卡启动确认返回 1 并报告 `no data NIC`。
UDP/TCP 报文长度、校验和及负载边界检查也通过。

### 2026-10-02 真实 PMD 收包结果

接收端数据口 PCI `0000:0b:00.0`、PMD `net_vmxnet3`，报告最大 RX 16 / TX 8
队列和 `rss_capa=0x514`（IPv4、IPv4 TCP、IPv6、IPv6 TCP，不含 UDP RSS）。
最初只要求 UDP RSS 的测试因此明确拒绝配置；后续改为支持的 IPv4 能力交集，
使用不同源 IP，并分别发送 UDP 和 TCP 流量。

| 配置 | 报文 | 实际接收 | 遗漏 | 队列 0 | 队列 1 | RSS hash 标记 |
|---|---|---:|---:|---:|---:|---:|
| UIO | UDP，64 条流 | 4096 | 0 | 4096 | 0 | 0 |
| UIO，显式 RSS hash offload | UDP，64 条流 | 4096 | 0 | 4096 | 0 | 0 |
| UIO，显式 RSS hash offload | TCP，64 条流 | 4096 | 0 | 4096 | 0 | 0 |
| VFIO No-IOMMU，RX 队列中断 | TCP，64 条流 | 4096 | 0 | 4096 | 0 | 0 |
| VFIO No-IOMMU，RX 队列中断及接收校验和 | TCP，64 条流 | 4096 | 0 | 4096 | 0 | 0 |

各轮有效报文的地址、端口、流编号、负载和序号匹配，无重复或遗漏，但双队列
RSS 验收失败，程序返回非零；`errors=4096` 来自每包缺少 RSS hash 标记。
当前结果只确认真实 PMD 数据口 RX，不证明 RSS 分流、平台 worker 转发或硬件
规则卸载。VMware、PMD 与设备配置的具体根因尚未确定，不据此修改生产初始化。
对照 [DPDK 21.11 vmxnet3 实现](https://github.com/DPDK/dpdk/blob/v21.11/drivers/net/vmxnet3/vmxnet3_ethdev.c)
检查了 RSS hash offload、队列中断及接收校验和相关配置。

测试期间临时配置 128 个 2 MB 大页；VFIO No-IOMMU 模式仅在相关测试期间启用。
结束后确认数据口回到 `vmxnet3`、`192.168.100.1/24` 和原路由，管理口可用，
大页数量恢复为 0，VFIO `enable_unsafe_noiommu_mode` 恢复为 `N`。
恢复后定向 ARP 成功，数据口 ping 3/3、零丢包。未重启虚拟机。
接收端日志为 `/tmp/dppd-rss-validation/rss-uio-tcp.log`、
`rss-vfio-tcp-without-checksum.log` 和 `rss-vfio-tcp-with-checksum.log`。

### Linux 原生驱动对照：缩小 RSS 问题范围

`rss_kernel_probe.py` 不需要 root，不切换驱动，也不修改网卡配置；使用正常
TCP socket 建立 64 个连接，每个连接传输 64 条带流编号及序号的数据记录。
接收端逐字节核对内容，并比较 vmxnet3 的逐队列单播 RX 计数。队列计数包含
TCP 握手、ACK 等网络报文，不等于应用数据记录数；其他同时运行的流量也可能
影响计数，测试时应保持数据网段空闲。只有一个队列收到流量时返回非零。

```bash
# 接收端：出现 KERNEL_TCP_READY 后再启动发送端
python3 tests/integration/rss_kernel_probe.py receive --interface ens192
# 发送端
python3 tests/integration/rss_kernel_probe.py send
```

2026-10-02 对照结果：Linux `ethtool` 显示 Combined=8、receive-hashing=on、
Toeplitz 和覆盖 8 个队列的 RETA，但实际流量仍只有队列 0 收包。
先用构造的 TCP 报文测试，队列 0 单播计数增加 4098（包含额外 ARP 等包），
其余 7 个队列均为 0；再使用正常 TCP 连接，64 个连接和 4096 条数据记录全部
核对通过，队列 0 增加 320 个单播报文，其余队列均为 0。保存后的对照脚本
使用端口 10002 复跑，结果相同，按预期返回 1；日志为
`/tmp/dppd-rss-validation/kernel-probe.log`。

这将问题范围缩小到当前 Workstation/VMnet3 数据路径，说明现象并非只存在于
DPDK 测试程序或构造报文中；尚未证明当前产品版本普遍不支持 RSS，具体宿主
实现原因仍未确定。生产 RSS 配置保持原样，本环境继续以单队列进行真实 PMD
验收；多队列 RSS 验收需要经过对照验证能够实际分流的环境。
资料中的 `ethernetX.pnicFeatures=4` 来自
[VMware/Intel DPDK Summit 2014，第 22 页](https://www.dpdk.org/wp-content/uploads/sites/23/2014/09/DPDK-SFSummit2014-VMwareIntelVirtualization.pdf)，
针对 ESXi 物理网卡 RSS，不据此推断它能修复本次 Workstation VMnet3 问题，
本轮未添加该设置、未重启虚拟机、未修改生产代码或两端网络配置。

## 单队列真实 PMD → 正式 worker → TAP 验收

2026-10-02 已在 `.135` 使用正式 `dppd/dppctl`，将真实 `net_vmxnet3` 数据口
与临时 `net_tap` 出口组成单队列 port-pair。外部 `.134` 从 VMnet3 发包；
`pmd_traffic_capture.py` 在接收端 TAP 上用原始 socket 捕获完整 Ethernet 帧，
逐字节比较 MAC、IPv4/UDP 头、校验和、流编号和序号，检测重复、遗漏及丢弃漏包。
它与 `rss_sender.py` 同目录，无需 Scapy，但捕获需要 root。

管理员准备数据口、内存及退出恢复后，daemon 的本轮启动参数为：

```bash
sudo ./build/dppd -l 0-1 -a 0000:0b:00.0 \
  --vdev=net_tap_sq,iface=dppdsqout --iova-mode=pa -m 128 --no-telemetry \
  --file-prefix=dppd-singlequeue-check -- --ports 0,1 --queues 1 \
  --mbufs 8191 --cache 0 --control-socket /tmp/dppd-rss-validation/singlequeue.sock \
  --state-path /tmp/dppd-rss-validation/singlequeue.state --promisc
```

本轮 `port-show` 确认真实入口为端口 0、TAP 为端口 1；其他环境不能假定编号，
应先核对 driver/MAC。临时 TAP 的 IPv6 在本轮捕获前禁用，以减少背景流量。
发送前启动捕获，确认 `CAPTURE_READY`，在捕获窗口内完成发包：

```bash
sudo python3 tests/integration/pmd_traffic_capture.py --interface dppdsqout \
  --source-mac 00:0c:29:68:de:da --destination-mac 00:0c:29:f8:f6:82 \
  --packets 256 --duration 30
# 发包端另一个终端执行
sudo python3 tests/integration/rss_sender.py --interface ens160 \
  --destination-mac 00:0c:29:f8:f6:82 --receiver-dpdk --packets 256
```

本轮创建的软件规则及 COUNT 查询使用：

```bash
sudo ./build/dppctl --socket /tmp/dppd-rss-validation/singlequeue.sock \
  apply-filter 9001 0 0 udp 192.168.100.2/32 192.168.100.1/32 any 10000 drop count software
sudo ./build/dppctl --socket /tmp/dppd-rss-validation/singlequeue.sock count 9001 1
sudo ./build/dppctl --socket /tmp/dppd-rss-validation/singlequeue.sock delete 9001 1
```

实际 generation 必须读取 apply 结果，不能直接复制本轮值。
DROP 验证使用 `--packets 64 --expect-drop --duration 30`，发包端也发送 64 包。
仅捕获到零包不能单独证明丢弃，必须同时确认入口 RX、规则 COUNT 及
`rule_drops` 增量，并保证发包发生在捕获窗口内。本轮复验满足这些条件。

- 初始 256 包全部转发并逐字节匹配，无重复或遗漏。
- 两次 DROP+COUNT 创建分别得到 generation 1 和 3；每次发 64 包，COUNT
  均为 `hits=64 bytes=3968`，重新创建后的计数重新开始。generation 3 的
  发包期间同步捕获，出口没有匹配报文。删除分别推进到 generation 2 和 4。
- 删除后最终一组 64 包全部转发并逐字节匹配；中间另一组 64 包由出口统计
  确认转发，因捕获窗口结束未逐包核对，不将其作为内容完整性的证据。
- 首轮总计入口 `RX=512/31744 bytes`，规则丢弃 128 包，TAP 出口
  `TX=384/23808 bytes`；无入口畸形、无路由、出口异常或 TX 丢弃。
  TAP 初始化另有 23 个 IPv6 背景包进入反向路径，整体统计为 RX 535 / TX 407，
  不将其算入测试 UDP 流量；端口/队列查询可明确区分。
- 快照 enabled/clean，最终 persisted/current generation 均为 4，repository 为空。
  修正显示问题后重新启动恢复这一空快照，随后额外 64 包转发及逐字节核对通过。

验收发现单队列 `mq_mode=NONE` 时仍把 RSS 能力交集写入 `configured_rss_hf`，
导致 `port-show` 错报 `rss-enabled`。已修正为单队列配置值 0；capability 不变，
多队列逻辑保持原样。严格构建、16/16 单测和真实 daemon 查询通过：入口
`rss-cap=0x514 rss-enabled=0x0`，TAP `rss-cap=0x3afbc rss-enabled=0x0`。

这是 VMware 虚拟网卡真实 PMD、正式 worker、CLI 软件规则和 TAP 出口的验证，
不代表双物理端口、双向报文内容、硬件 flow offload、RSS 或吞吐验收。
两次启动结束后 daemon 正常退出，管理 socket 与临时 TAP 清理，数据口驱动、
地址和路由恢复；大页回到 0，VFIO No-IOMMU 参数回到 N。
日志在 `/tmp/dppd-rss-validation/`：`sq-baseline-capture.log`、
`sq-drop-live-capture.log`、`sq-resume-live-capture.log`、`sq-final-stats.log`、
`sq-fixed-capture.log` 和 `sq-rss-disabled-port-info.log`。

## 非空软件快照：daemon 重启恢复

```bash
python3 tests/integration/software_replay.py --build-dir build
```

覆盖 2/4 条规则和 software/prefer 四种组合。每组在满容量下批量更新到端口 1，
实际停止并重新启动 daemon 两次，核对规则内容、版本、全局版本、安装端口和 clean
持久化状态；随后检查旧请求 ESTALE，以及重启后仍可批量更新。损坏快照 CRC 时必须
拒绝启动、不开放管理 socket，也不覆盖文件。此脚本不使用 flow 故障共享库。

`software_traffic` 另在停止 worker 后保存非空快照，销毁并重建控制服务，再启动
正式 worker 发包，确认恢复后的 DROP、COUNT、转发和 mbuf 回收。该收发部分是同一
EAL 进程内的控制服务恢复，实际进程重启由上述脚本单独验证。

### 真实 vmxnet3 入口：非空快照重启与收包

2026-10-02 在 `.135` 使用单队列真实 vmxnet3 → 正式 worker → TAP port-pair，
启用独立 `pmd-replay.state`，完成一次真正的 daemon 停止和重新启动。期间每次
退出均把数据口交还 Linux；再次启动重新绑定数据口，管理口保持可用。

1. 首个进程创建 rule 9101、generation 1、install-port 0，匹配源
   `192.168.100.2/32`、目标 `192.168.100.1/32`、UDP 目标端口 10000，
   software-only 的 COUNT+DROP。外部发 64 包，入口 RX、policy/rule drops
   均为 64，COUNT 为 64 hits/3968 bytes，发包期间 TAP 捕获无匹配报文。
   快照 enabled/clean，persisted/current generation 均为 1。
2. 停止首个进程并启动新进程，没有重新 apply 规则。查询确认完整规则内容、
   generation 1 和安装端口 0 保持一致，COUNT 为 0 hits/0 bytes；快照 SHA-256
   与启动前一致。过期 generation 0 的 COUNT 查询返回 ESTALE，CLI 退出码 1。
3. 重启后外部再次发 64 包，COUNT 重新准确增加到 64 hits/3968 bytes，入口
   RX 和 policy/rule drops 均为 64，TAP 同步捕获无漏包。
4. 按 generation 1 删除规则，repository generation 推进到 2；再发 64 包，
   TAP 捕获全部 64 个 byte-identical 报文，无重复或遗漏。新进程的测试流量
   统计为入口 RX 128/7936 bytes、规则丢弃 64、出口 TX 64/3968 bytes。
   最终 repository 为空，快照 clean，persisted/current generation 均为 2。

三段外部测试共 192 包：128 软件策略丢弃、64 逐字节验证转发；两个进程另有
21/25 个 TAP 初始化背景包进入反向路径，不混入测试流量统计。证据日志自动
核对规则内容、计数、统计、捕获结果、过期版本及最终持久化状态，通过。
这证明一次正常进程重启后，真实 PMD 入口的软件规则恢复及实际执行；不证明
重启期间持续流量无损，也不把 COUNT 当作跨重启持久计数，不涉及硬件 flow 重放。

操作复现时沿用前一节的 daemon/捕获/发送参数，使用独立的 state-path 和
control-socket；先保存非空规则，再正常停止进程，以同一 state-path 启动，
确认恢复后的规则及零计数后开始发包。发包应发生在捕获窗口内。
最终两个进程正常退出，管理 socket 和临时 TAP 清理，数据口驱动、地址和
数据路由恢复，大页数量 0、VFIO No-IOMMU 参数 N；恢复后定向 ARP 与 ping 3/3 通过。

日志位于 `/tmp/dppd-rss-validation/`：`pmd-replay-before-state.log`、
`pmd-replay-restored-state.log`、`pmd-replay-after-state.log`、
`pmd-replay-final-state.log`、`pmd-replay-stale.log`、三个
`pmd-replay-*-capture.log` 及 `pmd-replay-verification.log`。

## 链路断开、自动恢复与查询失败

2026-10-03 daemon 主循环接入 `rte_eth_link_get_nowait`，使用原子状态通知 worker，
管理协议 v9 的 `port-show` 返回链路状态。API 的状态和错误语义见
[DPDK 21.11 ethdev 文档](https://doc.dpdk.org/api-21.11/rte__ethdev_8h.html)。
设备启动后先查询一次，随后在主循环查询；PMD 支持时 down/up 更新软件出口保护。
断开期间入口仍可收包和执行策略，需要转发的报文按 egress drop 释放。
恢复不重启 worker、不重建规则或计数器，也不改写 snapshot。

`pmd_link_recovery.py` 连接已启动的独立 daemon，入口 port 0、TAP peer port 1、
空账本、启用 snapshot，两个端口最初均 up。脚本需要 root，操作指定临时 TAP 的
up/down；不操作管理口。两端先完成定向 ARP，再由 DPDK 接管真实数据口。

```bash
sudo python3 tests/integration/pmd_link_recovery.py --ctl build/dppctl \
  --socket /tmp/pmd-link.sock --state-path /tmp/pmd-link.state \
  --source-mac 00:0c:29:68:de:da --destination-mac 00:0c:29:f8:f6:82
```

每个 `LINK_READY phase=... protocol=...` 出现后，外部端发送指定协议的 64 包，
流数保持 64。沿用 `rss_sender.py --receiver-dpdk --packets 64 --rate 1000`，
根据 phase 选择 `--protocol udp/tcp`；每阶段最多等 45 秒，收齐后观察 1 秒。
脚本在 down/up 操作后等待 `port-show` 确认状态，再接收该阶段流量。

| 阶段 | 协议 | RX | 转发 | 出口断开丢弃 | 规则丢弃 |
|---|---|---:|---:|---:|---:|
| initial-policy | TCP | 64 | 0 | 0 | 64 |
| baseline | UDP | 64 | 64 | 0 | 0 |
| down1 | UDP | 64 | 0 | 64 | 0 |
| up1 | UDP | 64 | 64 | 0 | 0 |
| down2 | UDP | 64 | 0 | 64 | 0 |
| up2 | UDP | 64 | 64 | 0 | 0 |
| policy | TCP | 64 | 0 | 0 | 64 |

真实 vmxnet3 → 正式单队列 worker → TAP 的完整七阶段已通过。测试合计 448 包、
29312 bytes，192 个 UDP 报文逐字节一致，128 个出口断开丢弃、128 个规则丢弃，
TX 丢弃为 0。rule 14001、generation 1 的 TCP COUNT+DROP 在两次链路恢复后保持
内容和版本，快照 SHA-256 不变；已有 COUNT 64 hits/4736 bytes 保持，最终增加到
128 hits/9472 bytes。删除后空表、clean generation 2。另有 23 个 TAP 初始化背景包、
2882 bytes 单独核对，不计入表格。

断开时 AF_PACKET 捕获可返回 ENETDOWN，脚本仅在出口 down 的阶段把它视为无可读
报文，必须同时核对实际入口 RX、egress drops 和出口 TX 增量为 0；恢复后仍逐包
捕获并检查重复、遗漏和内容。首轮脚本未处理 ENETDOWN 而提前结束，未计入验收。
最后进程正常退出，8191 个 mbuf 全部归还，socket/TAP 清理；数据口驱动、地址、
路由、大页 0 和 VFIO 参数 N 恢复，定向 ARP 和 ping 3/3 通过。
日志在 `/tmp/dppd-rss-validation/pmd-link-final-*.log`。

查询失败使用独立、不可安装且不链接生产程序的 `link_faults.c` 测试库：

```bash
python3 tests/integration/link_failure.py --build-dir build
```

两组 net_ring 进程测试在保存非空软件规则后，显式触发 ENODEV/EIO 查询错误；
确认 daemon 失败码 1 退出、worker 和 socket 清理、1024 个 mbuf 全部归还、快照
字节不变。卸载测试库重新启动后，规则、generation 和 clean 状态恢复，正常退出。
测试库只覆盖 API 报错路径，不证明设备真的拔除；物理热拔插未验收，进程内重新枚举未实现。
初始 ENOTSUP 标记 unsupported 并保持原转发，无法提供链路保护；已监控端口后来
报错则退出。约 100 ms 主循环不是检测延迟的硬上限，切换前在途报文不承诺无损。
软件出口保护不控制硬件 flow。严格构建、17/17 单测、四组普通 net_ring 收发均通过；
新增真实 worker 出口 down 测试验证 egress drop、无 TX 和 mbuf 回收。

## 真实 PMD 与 TAP：同时双向转发

`pmd_duplex_traffic.py` 在一个接口上同时发送和捕获另一端流量；两端各运行一个
实例，共用同目录的 `rss_sender.py`。需要 root、已启动的独立 daemon 和已验证
的实际端口/MAC。先完成数据口 ARP 验证，再把网卡交给 DPDK；测试脚本不绑定网卡
或修改网络。默认各发送 30000 包、64 流、1000 pps，捕获先启动，5 秒后开始发送，
接收截止为延迟加发送时长再加 15 秒。最多发送 120 秒、1000000 包。

捕获排除本地 `PACKET_OUTGOING`，逐包核对完整 MAC、IPv4、UDP/TCP 头、校验和、
流号和序号，并检查重复和遗漏；收齐后仍保留到本地发送完成，再观察 1 秒。
两端的发送和预期接收参数必须互相对应。下面沿用当前测试机端口与 MAC：

```bash
# .135：TAP 端，真实网卡 port 0 ↔ TAP port 1 的单队列 daemon 已启动
sudo python3 tests/integration/pmd_duplex_traffic.py --interface dppdsqout \
  --tx-destination-mac 00:0c:29:68:de:da \
  --tx-source-ip 192.168.100.1 --tx-destination-ip 192.168.100.254 \
  --rx-source-mac 00:0c:29:68:de:da --rx-destination-mac 00:0c:29:f8:f6:82 \
  --rx-source-ip 192.168.100.2 --rx-destination-ip 192.168.100.1 --protocol udp

# .134：外部端，TAP_MAC 填 .135 本次启动后的 /sys/class/net/dppdsqout/address
sudo python3 tests/integration/pmd_duplex_traffic.py --interface ens160 \
  --tx-destination-mac 00:0c:29:f8:f6:82 \
  --tx-source-ip 192.168.100.2 --tx-destination-ip 192.168.100.1 \
  --rx-source-mac "$TAP_MAC" --rx-destination-mac 00:0c:29:68:de:da \
  --rx-source-ip 192.168.100.1 --rx-destination-ip 192.168.100.254 --protocol udp
```

两端尽量同时启动；分别出现 `DUPLEX_READY` 后自动发送，成功输出 `DUPLEX_PASS`。
以 `--protocol tcp` 在两端重复。反向目标 IP `.254` 不配置到任何接口，使用明确
的外部 MAC 投递，供原始 socket 捕获，避免 Linux 为测试 UDP/TCP 生成 ICMP/RST
干扰统计；不修改主机原有地址、路由或防火墙。TCP 仍是原始 ACK 帧，不建立会话。
此拓扑验证真实 PMD 和 TAP 的两个转发方向，不是双物理网卡验收。

2026-10-03 在 Ubuntu 22.04.5 / DPDK 21.11.9、正式 daemon 单队列 worker 上通过：

| 协议 | 真实网卡 → TAP | TAP → 真实网卡 | 每方向测试字节 | 捕获结果 |
|---|---:|---:|---:|---|
| UDP | 30000 | 30000 | 1860000 | 两端逐字节一致，无重复或遗漏 |
| TCP | 30000 | 30000 | 2220000 | 两端逐字节一致，无重复或遗漏 |

两轮各约 30 秒同时双向发送，共 120000 个测试报文、8160000 bytes。
入口 RX 与对端出口 TX 的计数和字节匹配，端口/队列汇总与整体统计一致，畸形、
策略和发送丢弃均为 0。23 个 TAP 初始化背景包共 2882 bytes，在测试前已进入
反向路径；TCP 期间另有 1 个 87-byte 背景包经过正向路径，单独核对并排除于上表。
daemon 最终整体 RX/TX 均为 120024 包、8162969 bytes。

退出时端口关闭后的 socket 0 缓冲池 `available=8191 capacity=8191 in-use=0`，
证明本次测试的 mbuf 全部归还，不作为其他堆内存或长期运行泄漏的证明。
快照保持空表、clean generation 0；退出后 socket/TAP 清理，数据口驱动、
地址、数据路由、大页 0、VFIO No-IOMMU 参数 N 全部恢复，定向 ARP 和 ping 3/3 通过。
严格构建和 16/16 单测通过。流量限速 1000 pps，不作为吞吐、RSS、硬件卸载或
TCP 会话验收。接收端证据在 `/tmp/dppd-rss-validation/pmd-duplex-*.log`，
发送端在 `/tmp/pmd-duplex-{udp,tcp}-external.log`；统计与缓冲池自动核对记录为
`pmd-duplex-verification.log`。

## 真实 PMD：TCP 过滤、UDP 放行与删除恢复

2026-10-02 使用 `.134` 外部发包和 `.135` 单队列 vmxnet3 → 正式 worker → TAP，
完成 TCP 报文逐字节转发、软件 TCP COUNT+DROP、UDP 不受影响、快照恢复及删除后
TCP 转发恢复验证。`pmd_traffic_capture.py` 新增 `--protocol tcp`，识别 TCP 的
测试负载位置，并核对完整帧；默认 UDP 用法保持一致。

```bash
# 独立 daemon 已启用 snapshot，入口 port 0，出口 TAP dppdsqout
sudo build/dppctl --socket /tmp/pmd-tcp.sock apply-filter 13001 0 0 tcp \
  192.168.100.2/32 192.168.100.1/32 any 10000 drop count software

# 接收端：确认 CAPTURE_READY 后立即从发送端发包
sudo python3 tests/integration/pmd_traffic_capture.py --interface dppdsqout \
  --source-mac 00:0c:29:68:de:da --destination-mac 00:0c:29:f8:f6:82 \
  --protocol tcp --expect-drop --packets 64 --duration 15

# 外部发送端：此前已完成数据口定向 ARP，接收端 DPDK 已接管
sudo python3 tests/integration/rss_sender.py --interface ens160 \
  --destination-mac 00:0c:29:f8:f6:82 --receiver-dpdk \
  --protocol tcp --packets 64 --rate 64
```

源端口通配使用 `any`；`0` 是精确端口 0，并不会匹配发送器的 20000–20063。
TCP 测试帧为 74 bytes，UDP 为 62 bytes。UDP 放行时两端改用 `--protocol udp`，
捕获端去掉 `--expect-drop`；TCP 删除恢复同样去掉该选项。先读取实际 generation，
再以精确版本查询 COUNT 或删除规则。

| 验收阶段 | 入口 RX 增量 | TAP 出口 TX 增量 | 规则丢弃 | 捕获结果 |
|---|---:|---:|---:|---|
| 无规则 TCP | 64 | 64 | 0 | 64 包逐字节一致 |
| TCP 规则下 UDP | 64 | 64 | 0 | 64 包逐字节一致，TCP COUNT 保持 0 |
| 快照恢复后 TCP | 64 | 0 | 64 | 无泄漏，COUNT 64 hits/4736 bytes |
| 删除规则后 TCP | 64 | 64 | 0 | 64 包逐字节一致 |

四个通过阶段合计 256 包、192 转发、64 丢弃，分别按阶段统计增量核对，
并非某一个进程的总计。规则 13001 在修正源端口后为 generation 2，重启恢复内容
完全一致且 COUNT 为零；删除后最终 generation 3、空账本、clean snapshot。
初轮错误源端口配置、捕获窗口错过发包及超时退出前的重复发包不计入上表。
首个进程达到预设 240 秒测试时限后退出并恢复网卡，随后提高至 600 秒重启，
重新完成有同步捕获和统计的 TCP DROP 验收。初轮抓到的 TCP 转发来自规则未匹配，
不作为匹配规则泄漏的证据。

TCP 是有效校验和的原始 ACK 测试帧，不建立 TCP 会话；本轮证明解析、过滤和转发，
不证明 TCP 连接、RSS、硬件卸载或吞吐。最终 daemon 正常退出，socket/TAP 清理，
数据口驱动、地址、路由、大页及 VFIO 参数恢复，定向 ARP 和 ping 3/3 通过。
证据保存在 `/tmp/dppd-rss-validation/pmd-tcp-*.log`，
`pmd-tcp-verification.log` 自动核对四个阶段的计数、字节、捕获和最终状态。

## 真实 PMD：满表软件更新与外部连续收包

`pmd_batch_traffic.py` 连接已经启动的独立 daemon；需要 root 捕获 TAP，初始
账本为空且 snapshot 已启用。先完成数据口定向 ARP 验证，再把数据口交给 DPDK。
daemon 使用单队列真实入口 port 0、TAP peer port 1，容量与 `--rules` 相同。
外部发送端沿用 `rss_sender.py`，固定源/目标 IPv4、UDP 和 1000 pps，不修改测试
流的端口或负载。可先关闭临时 TAP 的 IPv6，减少反向初始化背景流量。

```bash
# 接收端：先启动测试，看到 BATCH_TRAFFIC_READY 后再发送
sudo python3 tests/integration/pmd_batch_traffic.py \
  --ctl build/dppctl --socket /tmp/pmd-batch.sock \
  --state-path /tmp/pmd-batch.state --interface dppdsqout --rules 2

# 独立发送端：仅在数据口连通性已确认且接收端 DPDK 接管后使用
sudo python3 tests/integration/rss_sender.py --interface ens160 \
  --destination-mac 00:0c:29:f8:f6:82 --receiver-dpdk --packets 10000 --rate 1000
```

测试不会绑定网卡或启动 daemon，需要调用方负责启动和退出恢复。默认等待首次
收包及全部收包的总时间最多 45 秒；创建满表规则后验证新增 ENOSPC，随后连续
50 次更新，检查共享事务与连续版本、收包重叠、全部规则丢弃、出口零泄漏，
再验证旧版本 ESTALE 和快照不变，最后整批删除到空账本和 clean snapshot。
每组使用新 state-path 和匹配容量，以 `--rules 4` 重复四规则组。

2026-10-02 两组已在 vmxnet3 上通过，各接收/丢弃 10000 包、50 次批量更新，
各 23 个更新区间观察到收包增长，最终持久版本 104/208。测试同时发现并修复
单规则满表新增的 backend/账本不一致问题，容量预检及软硬件回归通过。
旧新表规则均为 DROP，因此本测试不独立证明逐包完整表切换；也不验证 RSS、
硬件 flow 或吞吐。具体证据与边界见 [批量更新记录](../../docs/validation_batch_update.md)。

## 在线恢复隔离：进程级故障注入

```bash
meson setup build -Dtests=true -Dwerror=true  # 已有 build 时使用 --reconfigure
meson compile -C build
python3 tests/integration/recovery_isolation.py --build-dir build
```

`flow_faults.c` 是独立、不可安装的测试共享库，不链接到 dppd。只有上述脚本启动的
隔离测试进程通过 `LD_PRELOAD` 显式加载它。库在 DPDK flow API 边界模拟 handle 和
定点错误，实际 ethdev/worker 仍使用两个 net_ring 端口；这不是物理 PMD 验证，
也不是新增生产 mock 模式。正常运行的 dppd 仍要求真实 DPDK。

覆盖两条路径：

1. 第二条新版本创建失败，第一条新版本回滚删除也失败。
2. 第二条旧版本删除失败，撤销新版本后，恢复第一条旧版本又失败。

每条路径均确认 worker 已停止、普通管理请求返回 EUCLEAN、旧 snapshot 字节不变。
首次 `reconcile-retry` 注入删除失败，进程保持隔离；第二次成功，返回 restart-required
和零残留后退出，退出码为 1、socket 被清理、模拟 handle 无泄漏。脚本随后使用原
snapshot 重启，检查旧 rule/generation/priority 全部恢复，并正常退出。

2026-09-26 两组用例已通过。此重启测试使用模拟 flow handle，不能作为非空 software
snapshot 或真实硬件的恢复证据。脚本支持 `--lcores`，无需 sudo，临时进程与目录自行清理。

## 设备移除通知、回调生命周期与重启

```bash
meson setup build --reconfigure -Dtests=true -Dwerror=true
meson compile -C build
python3 tests/integration/removal_failure.py --build-dir build
```

`removal_faults.c` 是显式 LD_PRELOAD 的独立测试库，不安装且不链接生产程序。
事件用例通过 DPDK 自己的 ethdev 回调分发器调用生产移除回调；测试代理在回调
返回后暂留 500 ms，真实 `rte_eth_dev_callback_unregister` 返回 EAGAIN，验证
主线程等待回调结束后再关闭端口。测试库使用 DPDK 21.11 的 INTERNAL 分发符号，
只用于已验证的测试环境；生产代码只使用公开 ethdev API。
相关语义见 [DPDK 21.11 ethdev API](https://doc.dpdk.org/api-21.11/rte__ethdev_8h.html)
及 [回调实现](https://github.com/DPDK/dpdk/blob/v21.11/lib/ethdev/rte_ethdev.c)。

2026-10-03 在 Ubuntu 22.04.5 / DPDK 21.11.9 上通过 8 组用例：

| 用例 | 触发位置 | 检查 |
|---|---|---|
| callback | port 0、port 1 各一组 | 实际 ethdev 事件分发，在途回调注销等待 |
| callback-no-link | port 1 | 初始 link ENOTSUP 仍接收移除通知 |
| probe | port 1 | 链路查询正常，is_removed 单独触发停机 |
| probe-no-link | port 1 | 初始 link ENOTSUP 仍轮询移除状态 |
| startup-event | port 0、port 1 各一组 | 配置前收到通知，不启动 worker 或开放管理入口 |
| register-error | port 1 | 第二端口注册失败，注销第一端口回调并清理 |

运行期用例先保存两条不同安装端口的非空软件规则，触发后确认失败码 1、socket
清理、1024 个 mbuf 全部归还和快照字节不变。使用未加载测试库的 daemon 重启，
两条规则内容、安装端口和 generation 恢复，clean generation 2、COUNT 从零开始，
正常退出后再次检查 socket 和缓冲池。启动故障用例确认没有生成快照或管理入口，
随后正常启动空表。各用例都不能通过链路查询报错来代替移除检测。

四组 `test_software_traffic` 同时检查：运行中的正式 worker 在未设置普通停止
标记时响应移除标记退出，注销 QSBR 后可清理软件规则；本进程再次启动返回 ENODEV，
缓冲池与 ring 中无遗漏对象。该验证不声称中途移除时持续流量无损。

18/18 单测通过，新增设备清理失败测试覆盖：stop 失败但 close 成功仍回传错误，
close 失败继续清理其他端口且保留共享缓冲池，注销失败不继续关闭端口，以及注销
EAGAIN 重试成功后正常回收。正常路径只有所有端口 close 成功才释放 mbuf pool。

这些测试验证应用处理逻辑和回调生命周期，不改变真实设备状态；没有证明物理
热拔插、SIGBUS 访问保护、运行中重新枚举或硬件 flow 的故障恢复。

### 真实 vmxnet3 路径的通知注入与恢复

同日在 `.134` 外部发包 → `.135` vmxnet3 port 0 → 正式 worker → TAP port 1 上
完成一次通知故障退出及一次未加载测试库的快照重启。故障进程单独加载上述库，
使用 `DPPD_TEST_REMOVAL_MODE=callback`、`DPPD_TEST_REMOVAL_PORT=0` 和独立的
`DPPD_TEST_REMOVAL_FILE`；在完成初始 TCP/UDP 收发验证后创建触发文件。数据口的
绑定、原驱动/地址/路由恢复由外层脚本负责，管理口保持可用。

| 通过阶段 | 协议 | 入口 RX | 逐字节转发 | 规则丢弃 |
|---|---|---:|---:|---:|
| initial-tcp | TCP | 64 | 0 | 64 |
| initial-udp | UDP | 64 | 64 | 0 |
| restored-tcp | TCP | 64 | 0 | 64 |
| restored-udp | UDP | 64 | 64 | 0 |
| released-tcp | TCP | 64 | 64 | 0 |

通过阶段共 320 包、22144 bytes，192 包完整帧一致、无重复或遗漏，128 包规则丢弃。
rule 15003 的 TCP COUNT+DROP 在退出前为 generation 1、64 hits/4736 bytes；
注入真实 ethdev 分发后观察到回调注销 busy 重试、故障进程退出码 1、socket/TAP
清理和 8191 个 mbuf 全部归还。快照 SHA-256 不变。重启后规则内容、安装端口和
generation 保持，COUNT 归零；TCP 仍被阻断并重新计数到 64/4736，UDP 正常转发。
删除规则后 TCP 恢复，最终空表、clean generation 2，正常退出码 0，8191 个 mbuf
再次全部归还。

首次 UDP 捕获窗口错过发包，额外 64 包仅有收发计数，没有完整帧捕获，因此排除于
通过阶段；重新同步后该阶段通过。两个进程另有 TAP 背景 28/25 包、3343/3059 bytes，
单独核对。包含这些额外流量的两个进程最终分别 RX 220/217、TX 156/153，规则
丢弃各 64、TX 丢弃为 0。测试后 vmxnet3、数据地址和路由、大页 0、VFIO 参数 N
恢复，socket/TAP 不存在，定向 ARP 和 ping 3/3 通过。通知由测试库注入，网卡本身
始终存在；不作为物理热拔插、持续流量中途移除或硬件 flow 恢复验收。
日志和逐阶段统计在 `/tmp/dppd-rss-validation/pmd-removal-*.log`、
`pmd-removal-verification.json`。
