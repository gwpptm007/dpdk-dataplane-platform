# M3 硬件与测试环境盘点

盘点日期：2026-10-18。所有检查均为只读，没有重绑网卡、启用 SR-IOV、更改网络或复位设备。
本记录只确认可见环境，不代表 M3 的实机验收通过。

| 环境 | 实测结果 | 对 M3 的影响 |
| --- | --- | --- |
| Windows 宿主机 | Lenovo 82WM；物理网卡为 Realtek PCIe GbE `10ec:8168` 和 RZ616 Wi-Fi `14c3:0616`；VMware Workstation 17 运行两个虚拟机 | 未发现可见的 Mellanox/BlueField/SmartNIC |
| `192.168.65.135` | VMware 虚拟机；Ubuntu 22.04.5；kernel `6.8.0-138-generic`；8 vCPU、7.7 GiB 内存 | 可继续进行构建、TAP、软件和模拟后端测试 |
| `192.168.65.134` | SSH 服务可达；BatchMode 缺少可用密钥，历史 `wq` 凭据认证失败；用户重启设备后再次只重试该凭据，仍返回 `Authentication failed` | 无法读取内部设备信息，不能据此推断其硬件情况 |

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

## 可落地范围和验收边界

当前环境能验证 representor/拓扑/规则图等纯逻辑、PMD 失败处理、模拟恢复，以及 TAP 上的管理与软件路径。
当前环境不能完成 [M3 路线图](roadmap_v3.md) 要求的 mlx5/BlueField 与物理非 mlx5 实机报告，
也不能证明硬件 transfer、真实 representor、offloaded packets 绕过 CPU worker 或设备 reset 后的硬件恢复。
这些项目必须保留为未验收，不能用模拟或 TAP 结果替代。

134 重启后的复查仅完成 SSH 握手和一次已授权凭据认证，未能登录。
因此其 DPDK 版本、TUN/虚拟网络条件以及 PCI 设备均为尚未观察，不能登记为缺少或已支持。
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
