# SmartNIC 与 DPU 开源项目调研

记录日期：2026-10-28

本次调研用于判断当前项目在 SmartNIC/DPU 方向的价值，并寻找更适合参考或复用的开源实现。
优先对标 `ironcore-dev/dpservice`：它与当前项目同样使用 DPDK 和 `rte_flow`，同时提供虚拟网络、
NAT 和负载均衡等明确的业务场景。OVS/OVN 适合研究完整虚拟网络系统，DOCA Platform 适合研究 DPU 集群管理，
FlexTOE、DPFS 和 Corundum 则展示了协议卸载、存储卸载及网卡硬件设计的不同方向。

以下功能描述来自项目仓库和官方文档，适用性判断是本次分析；未在本项目环境中部署这些项目或复测其性能。
记录是后续选型参考，不自动转为实现任务，不扩大现有路线图或验收范围。

## 当前项目的定位与差距

当前已验收能力主要覆盖软件转发、规则控制、持久化、恢复和测试体系。
M3 已完成当前支持的软件范围，实体设备卸载、CPU 旁路及真实移除/复位恢复仍缺少验收证据。
具体范围见 [路线图](roadmap_v3.md)、[M3 软件验收](m3_software_acceptance.md) 和 [M5 最终验收](m5_final_acceptance.md)。

从 SmartNIC/DPU 角度，下一次评估应回答三个具体问题：

- 哪些主机侧工作转移到了网卡硬件或 DPU 上
- 相同业务负载下，节省多少主机 CPU，吞吐和尾延迟怎样变化
- 是否形成可演示、可使用的网络、存储或安全服务

当前的软件阶段完成度不能用来证明 DPU 产品竞争力。持久化、事务和恢复是工程基础，
但仍需业务场景及实机收益证明其价值。尚无依据宣称当前项目在性能、成熟度或完整业务能力上领先这些项目。

## 候选项目比较

| 项目与仓库 | 官方描述的主要能力 | 对当前项目的参考价值与边界 |
| --- | --- | --- |
| [ironcore-dev/dpservice](https://github.com/ironcore-dev/dpservice) | DPDK Graph 数据面；首包软件处理、后续流通过 `rte_flow` 卸载；SR-IOV VF 虚拟端口；IP-in-IPv6 封装/解封装；Maglev 负载均衡与 NAT Gateway | 最接近当前技术栈，优先研究从业务配置到软件处理、硬件卸载的完整路径；具体功能组合仍需核对硬件支持 |
| [Open vSwitch](https://github.com/openvswitch/ovs) 与 [OVN](https://github.com/ovn-org/ovn) | OVN 将逻辑网络配置转为 OpenFlow，提供分布式逻辑交换机、路由器、ACL 等；OVS 提供转发及硬件卸载路径 | 适合研究完整虚拟网络系统及控制与转发的分工；OVN 软件功能不能全部视为已支持硬件卸载 |
| [NVIDIA/doca-platform](https://github.com/NVIDIA/doca-platform) | 通过 Kubernetes API 管理 BlueField 部署、生命周期及 DPU 服务，支持服务链 | 适合研究如何管理一批 DPU；主要是编排平台，不是可以直接比较吞吐的转发引擎，支持范围需按版本核对 |
| [tcp-acceleration-service/FlexTOE](https://github.com/tcp-acceleration-service/FlexTOE) | 将已建立连接的 TCP 数据路径卸载到 SmartNIC，通过细粒度并行处理提高效率 | 适合研究卸载边界、CPU 节省及尾延迟；属于研究实现，仓库运行要求包含特定 Netronome 网卡及 SDK |
| [IBM/DPFS](https://github.com/IBM/DPFS) | 文件系统实现运行于 DPU CPU，主机通过 PCIe 上的 `virtio-fs` 设备使用文件服务 | 直观展示存储服务卸载；官方明确标为研究项目，BlueField-2 路径依赖 SNAP、原型固件及相关修改 |
| [corundum/corundum](https://github.com/corundum/corundum) | 开源 FPGA 网卡，提供 DMA、队列、调度器、PTP、自定义硬件逻辑接口和系统仿真 | 适合深入网卡内部设计；需要 RTL/FPGA 技能，路线不同于 BlueField 上的软件应用开发 |

## 优先研究 dpservice

dpservice 的参考价值在于将数据面原语组织为可使用的云网络服务：虚拟机或裸金属接入 VF，
跨主机通信使用封装与解封装，通过 gRPC 管理网络接口和路由，并提供负载均衡及 NAT。
在卸载模式下，首包由软件处理，之后将流下发至硬件；非卸载模式则通过软件处理流量。
这些能力见 [项目说明](https://github.com/ironcore-dev/dpservice)。

它所在的 IronCore 网络体系还包含上层资源协调、节点网络管理和路由分发。
理解这种分工有助于判断当前项目适合继续作为独立产品，还是保留为规则控制与验证组件。
架构说明见 [IronCore Networking](https://ironcore.dev/iaas/architecture/networking.html)。

无需先购置 DPU 就能评估其软件业务场景：
[ironcore-in-a-box](https://github.com/ironcore-dev/ironcore-in-a-box) 提供本地 Kind 演示环境，包含 VM、VPC 和虚拟 IP 示例。
项目团队在 [DPDK 官方介绍](https://www.dpdk.org/building-kubernetes-native-sdn-with-dpdk-the-dpservice-story/)
中说明了其中基于 TAP 的 dpservice 路径。实际运行仍需满足容器、Linux 内核功能及虚拟化环境要求，
软件演示不能替代硬件卸载验收。

## 其他项目值得借鉴的具体内容

OVS 的 [TC flower 卸载](https://docs.openvswitch.org/en/latest/howto/tc-offload/)
与 [DPDK rte_flow 卸载](https://docs.openvswitch.org/en/latest/howto/dpdk/) 是不同路径。
支持的匹配、动作和连接跟踪组合取决于路径、驱动和硬件，应逐项验证。
[OVN 官方说明](https://www.ovn.org/en/) 列出的逻辑网络能力属于系统功能，不能直接推导出所有功能都能卸载。

[DOCA Platform 官方说明](https://networking-docs.nvidia.com/doca/archive/3-5-0/doca-platform-framework-dpf)
展示了主机与 DPU 集群分工、基础设施服务部署及服务链。
它适合回答 DPU 如何被批量管理、服务如何独立于主机工作负载运行的问题；
不能因编排框架开源就推断所部署的全部服务、SDK 和固件也采用相同许可。

[FlexTOE 项目说明](https://github.com/tcp-acceleration-service/FlexTOE) 报告：在作者的特定实验配置下，
Memcached 相比 TAS 扩展性最高改善 38%，相比 Chelsio 节省最高 81% 的主机 CPU 周期。
两项数字使用不同对照，不能合并为对同一基线的提升，也不能套用到当前环境。
值得借鉴的是用应用负载、主机 CPU 和尾延迟来证明卸载效果，而不只是统计支持多少功能。

[DPFS 项目说明](https://github.com/IBM/DPFS) 展示了主机使用标准文件设备、DPU 承担后端文件服务的分工。
其研究状态和专用依赖使它更适合架构学习，不能直接当作即装即用的生产替代方案。

[Corundum 项目说明](https://github.com/corundum/corundum) 提供从 PCIe/驱动模型到以太网接口的仿真，
以及自定义数据路径和 DMA 逻辑的接口。可以先研究和仿真，但真实时序、带宽及板卡兼容性仍需实体 FPGA 验证。

## 当前选型建议

优先对照 dpservice 的业务模型和卸载路径，再通过其软件演示判断实际效果。
需要完整虚拟网络体系时研究 OVS/OVN；需要 DPU 集群管理时研究 DOCA Platform；
需要协议、存储或硬件方向的技术研究时分别参考 FlexTOE、DPFS 和 Corundum。

当前项目保留为规则控制与验证基础，是否继续自研由具体业务缺口和对比结果决定。
此前讨论的混合事务、进程托管集成、凭据热轮换和发布签名，不因本次调研成为必做项；
其他增强建议也不自动进入开发计划。
