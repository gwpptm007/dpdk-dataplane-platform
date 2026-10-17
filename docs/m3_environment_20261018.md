# M3 硬件与测试环境盘点

盘点日期：2026-10-18。所有检查均为只读，没有重绑网卡、启用 SR-IOV、更改网络或复位设备。
本记录只确认可见环境，不代表 M3 的实机验收通过。

| 环境 | 实测结果 | 对 M3 的影响 |
| --- | --- | --- |
| Windows 宿主机 | Lenovo 82WM；物理网卡为 Realtek PCIe GbE `10ec:8168` 和 RZ616 Wi-Fi `14c3:0616`；VMware Workstation 17 运行两个虚拟机 | 未发现可见的 Mellanox/BlueField/SmartNIC |
| `192.168.65.135` | VMware 虚拟机；Ubuntu 22.04.5；kernel `6.8.0-138-generic`；8 vCPU、7.7 GiB 内存 | 可继续进行构建、TAP、软件和模拟后端测试 |
| `192.168.65.134` | 用户更正凭据后已成功登录；VMware 虚拟机 `wanqi-vm`；Ubuntu 23.10；kernel `6.5.0-42-generic`；8 vCPU、7.7 GiB 内存 | 已完成普通用户只读盘点，仍无可用于 M3 实机卸载验收的物理设备 |

## 135 的可见网络设备

| PCI 地址 | 接口 | 身份与驱动 | 使用情况 |
| --- | --- | --- | --- |
| `0000:02:01.0` | `ens33` | `8086:100f`，子系统 `15ad:0750`，`e1000` | VMware 模拟 82545EM；`192.168.65.135/24`，默认路由和 SSH 管理链路 |
| `0000:02:02.0` | `ens34` | `8086:100f`，子系统 `15ad:0750`，`e1000` | VMware 模拟 82545EM；未配置 IP |
| `0000:0b:00.0` | `ens192` | `15ad:07b0`，`vmxnet3` | VMware 虚拟网卡；`192.168.100.1/24` |

不能把上述两块模拟 Intel 网卡登记为已经验收的物理非 mlx5 PMD。
`ens34` 没有 IP 也不表示可以自动重绑；本次未变更任何接口。

## 135 的构建和设备条件

| 项目 | 实测结果 |
| --- | --- |
| Mellanox PCI | `lspci -nn -d 15b3:` 无输出 |
| devlink | root 查询 `devlink dev show`、`devlink port show` 均无条目 |
| SR-IOV 与 representor | 三个网络设备没有可读的 `sriov_totalvfs/sriov_numvfs` 属性，没有可见的 switch ID 或 representor 端口名 |
| IOMMU / VFIO | IOMMU group 数为 0；`/dev/vfio` 只有控制节点，没有 group 节点；内核启动参数未显式启用 IOMMU |
| 内核模块 | 未发现已加载的 `mlx5`、`vfio`、`uio` 模块 |
| DPDK | 系统 `libdpdk` 为 `21.11.9`；mlx5、TAP、e1000、vmxnet3 PMD 库存在；库存在不能证明有对应硬件 |
| 构建工具 | Meson `0.61.2`、Ninja `1.10.1`、C 编译器、pkg-config 可用 |
| 测试构建 | `/tmp/dppd-tap-content-validation/source` 下 system/native/ASan 三套构建目录存在 |
| 内存与磁盘 | 7.7 GiB 内存；盘点时约 5.6 GiB 可用，`/tmp` 所在文件系统约 69 GiB 可用 |
| hugepage / TUN | hugepage 数为 0；`/dev/net/tun` 存在；可以继续采用 no-huge TAP 测试 |
| 权限与工具 | 已验证测试账号可 sudo；`ip`、`tc`、`ethtool`、`unshare`、`nsenter` 可用 |
| 测试残留 | 盘点时没有 dppd 进程或命名网络 namespace |

## 134 的可见网络设备与测试条件

此前的认证失败记录已由用户更正凭据后的成功登录更新。本次通过标准输入执行只读盘点脚本，
未把凭据或临时脚本写入项目、未 sudo、未安装软件，也未改变网络配置。

| PCI 地址 | 接口 | 身份与驱动 | 使用情况 |
| --- | --- | --- | --- |
| `0000:02:01.0` | `ens33` | `8086:100f`，子系统 `15ad:0750`，`e1000` | VMware 模拟 82545EM；`192.168.65.134/24`，默认路由和 SSH 管理链路 |
| `0000:03:00.0` | `ens160` | `15ad:07b0`，子系统 `15ad:07b0`，`vmxnet3` | VMware 虚拟网卡；`192.168.100.2/24` |

| 项目 | 实测结果 |
| --- | --- |
| 设备身份 | DMI 为 `VMware Virtual Platform` / `VMware, Inc.`；两块网络设备均为明确的虚拟网卡 |
| Mellanox 与 representor | PCI 无 `15b3`；devlink 设备和端口均为空；接口 `phys_switch_id/phys_port_name` 返回 `Operation not supported` |
| SR-IOV / IOMMU / VFIO | 网络设备无 SR-IOV 属性和 IOMMU group；系统 IOMMU group 数为 0；`/dev/vfio` 只有控制节点 |
| hugepage / TUN | hugepage 数为 0；`/dev/net/tun` 为可读写字符设备；未进行 TAP 创建或特权测试 |
| DPDK 与构建工具 | GCC `13.2.0` 和 make 可用；Meson、Ninja、pkg-config 不在 PATH，也未发现用户常用前缀中的 Meson/Ninja |
| 已安装包与库 | dpkg 未登记 dpdk/libdpdk、pkg-config/pkgconf、meson、ninja-build；常见系统和 `/usr/local`、`/opt/dpdk` 前缀无 `libdpdk.pc`；动态库缓存无 librte |
| DPDK 版本 | 盘点结果为 `missing`，表示无法通过 pkg-config 读取版本；结合包与常见前缀查询，目前没有找到可直接用于严格构建的 DPDK 开发环境 |
| 现有网络工具 | Python 3、ip、tc、ethtool、tcpdump、scp、unshare、nsenter、ping 可用 |
| 内存与磁盘 | 7.7 GiB 内存，盘点时约 6.4 GiB 可用；`/tmp` 所在文件系统约 80 GiB 可用 |
| 双机数据链路 | 已有 `192.168.100.2/24` 地址与直连路由；向 135 的 `192.168.100.1` 发送两次 ping 均无回复，尚未取得双机数据链路连通证据 |
| 测试残留 | 盘点时没有 dppd 进程或命名网络 namespace |

134 目前适合继续准备软件测试环境；尚不能直接运行本项目严格 DPDK 构建。
本次没有搜索任意非标准安装目录，因此不把上述检查扩大解释为整机绝对不存在任何 DPDK 文件。
安装软件可以补齐编译与仿真工具，但不能把这些 VMware 设备变成真实 mlx5 或物理非 mlx5 卸载设备。

## 可落地范围和验收边界

当前环境能验证 representor/拓扑/规则图等纯逻辑、PMD 失败处理、模拟恢复，以及 TAP 上的管理与软件路径。
当前环境不能完成 [M3 路线图](roadmap_v3.md) 要求的 mlx5/BlueField 与物理非 mlx5 实机报告，
也不能证明硬件 transfer、真实 representor、offloaded packets 绕过 CPU worker 或设备 reset 后的硬件恢复。
这些项目必须保留为未验收，不能用模拟或 TAP 结果替代。

134 已在更正凭据后完成只读盘点，其两块虚拟网卡也不能作为物理非 mlx5 实机验收证据。
软件仿真可以覆盖控制逻辑和错误恢复，但不能代替真实设备的硬件卸载与 CPU 旁路验收。

## 可复用的只读盘点

在 Linux 测试机运行 `python3 scripts/inspect_m3_environment.py`，从标准输出保存 JSON 即可。
脚本不 sudo、不写 sysfs、不修改网络，也不执行 bind、reset 或 SR-IOV 操作。
若当前用户没有权限，输出会保留 `permission_denied`；不存在的路径保留 `missing`。
`candidate` 仅表示值得进一步核查的设备，`hardware_acceptance` 始终为 `not_run`。
脚本返回零只表示完成了盘点，不表示硬件验收成功。

脚本会标记默认路由和当前 SSH 本地地址所在接口，所有接口都禁止自动重绑。
mlx5 库检查仅覆盖 Debian/Ubuntu 常见目录，其他路径下的库不能由这一项排除。
`--root` 只替换读取文件的根目录，查询命令仍在当前宿主机运行；完整离线单测通过依赖注入替换命令。
独立运行 `python3 tests/unit/test_m3_inventory.py` 可验证分类、权限和缺失证据处理，不需要网卡或特权。
