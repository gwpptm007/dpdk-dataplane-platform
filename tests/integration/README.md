# 集成验收

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
