# Intel NIC / IPU 验收重点

- 明确使用的 PMD、device id、NVM/firmware 和 DPDK 版本；
- 区分传统 NIC SR-IOV representor 与 IPU/DPU 部署模型；
- 记录 switch domain 与端口角色，不按 port id 顺序猜 PF/VF；
- 对 ingress steering、transfer、represented-port、mark/count/age 分别 validate；
- 验证规则数量、优先级/group、reset、link flap 和进程重启行为；
- 软件 baseline 与硬件 offload 使用相同流量和计数口径对照。

具体 devargs 和支持矩阵必须以目标 PMD 版本文档及实测为准。
