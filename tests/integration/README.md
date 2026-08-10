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
