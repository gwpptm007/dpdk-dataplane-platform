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
