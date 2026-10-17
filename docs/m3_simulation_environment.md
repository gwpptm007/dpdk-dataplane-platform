# M3 功能模拟环境准备

本记录说明 `192.168.65.134` 的开发工具和私有 DPDK 环境。
这是软件功能模拟的准备工作，不是 SmartNIC、硬件 transfer 或 CPU 旁路验收。
最初的设备与系统观察见 [环境盘点](m3_environment_20261018.md)，功能边界见
[transfer 说明](m3_transfer.md)。

## 主机和安装范围

134 为 VMware 虚拟机，Ubuntu 23.10、kernel `6.5.0-42-generic`，8 vCPU、7.7 GiB 内存。
准备前 GCC `13.2.0`、make、Python `3.11.6`、libnuma 开发文件已经存在，
Meson、Ninja、pkg-config 和 DPDK 开发环境缺失。
测试账号的 sudo 已验证；本次仅用于安装必要开发依赖，没有变更接口、路由或设备绑定。

系统软件源原本已指向 Ubuntu 官方 `old-releases.ubuntu.com`，无需替换全局源或迁移系统。
先读取 apt 缓存并模拟安装，再执行收敛后的最小方案：

| 方案 | 模拟结果 | 处理 |
| --- | --- | --- |
| 全套 libdpdk-dev、工具和 python3-venv | 226 个新增包、7 个升级、0 个删除 | 未执行 |
| 最小工具和源码构建依赖 | 12 个新增包、0 个升级、0 个删除 | 已安装成功 |

实际安装的直接依赖为 `meson ninja-build pkg-config libelf-dev python3-pyelftools`，
连同 `libpkgconf3 libzstd-dev pkgconf pkgconf-bin python3-distutils python3-setuptools zlib1g-dev`，
下载约 2.8 MB、安装后额外占用约 11 MB。没有执行系统升级，也没有安装全套 DPDK 硬件驱动包。

| 工具 | 安装后版本 |
| --- | --- |
| GCC | 13.2.0 |
| Python | 3.11.6 |
| Meson | 1.2.1 |
| Ninja | 1.11.1 |
| pkg-config / pkgconf | 1.8.1 |
| pyelftools | 0.29 |

## 私有 DPDK

DPDK 采用 135 已缓存的 `21.11.9` 源码归档，在 134 的独立目录解压和构建。
135 的项目源码和构建目录未被修改。
为支持已有 TAP 规则归属与隔离测试，134 的私有源码副本应用了同一份已验证 TAP owner-cookie 补丁；
该补丁是项目私有扩展，不是 DPDK 标准 API。

| 项目 | 路径或标识 |
| --- | --- |
| 工作目录 | `/home/wq/.cache/dppd-m3-20261018` |
| 源码 | `/home/wq/.cache/dppd-m3-20261018/dpdk-stable-21.11.9` |
| 构建目录 | `/home/wq/.cache/dppd-m3-20261018/dpdk-build` |
| 构建日志 | `/home/wq/.cache/dppd-m3-20261018/dpdk-build.log` |
| 安装日志 | `/home/wq/.cache/dppd-m3-20261018/dpdk-install.log` |
| 私有安装前缀 | `/home/wq/.local/dppd-m3-20261018` |
| 源码包 SHA-256 | `051664744579097af7ea7b4960d3362f0ff93307a3c33166df6a4657984e2f07` |
| TAP 补丁 SHA-256 | `71985ad00d85e76afb6d69203eb43975d059e08bf24cff89cd73222d7b4a54fe` |

网络驱动只启用 ring/null/TAP，同时保留 vdev/PCI 总线依赖和 ring mempool，保留共享 `rte_net_ring` 库，
使用通用 x86 指令集和 `RTE_MAX_LCORE=128`，不编译或加载内核模块。
DPDK 自身测试和例子未编译；后续项目构建仍须启用项目测试和严格告警。

```bash
meson setup /home/wq/.cache/dppd-m3-20261018/dpdk-build \
  /home/wq/.cache/dppd-m3-20261018/dpdk-stable-21.11.9 \
  --prefix=/home/wq/.local/dppd-m3-20261018 --libdir=lib \
  -Dbuildtype=release -Dplatform=generic -Dmax_lcores=128 \
  -Dtests=false -Dexamples= \
  -Denable_drivers=bus/vdev,mempool/ring,net/ring,net/null,net/tap
ninja -C /home/wq/.cache/dppd-m3-20261018/dpdk-build -j4
meson install -C /home/wq/.cache/dppd-m3-20261018/dpdk-build --no-rebuild
```

私有 DPDK 已完成编译和普通用户安装。后续在独立项目目录构建时，加载以下环境即可，
不需要修改系统动态库配置或用户全局启动文件：

```bash
. /home/wq/.local/dppd-m3-20261018/environment.sh
pkg-config --modversion libdpdk
```

该环境文件只把私有 pkg-config、编译期库目录和动态库目录加在现有搜索路径之前，保留原来的变量值。
新 shell 加载后的 `PKG_CONFIG_PATH` 前缀为 `/home/wq/.local/dppd-m3-20261018/lib/pkgconfig`，
`LIBRARY_PATH` 前缀为 `/home/wq/.local/dppd-m3-20261018/lib`，
`LD_LIBRARY_PATH` 前缀为 `/home/wq/.local/dppd-m3-20261018/lib:/home/wq/.local/dppd-m3-20261018/lib/dpdk/pmds-22.0`。
已用预置路径验证保留行为，没有修改 `.profile`、SSH 密钥或系统配置。

实际返回 `21.11.9`，libdir 为 `/home/wq/.local/dppd-m3-20261018/lib`。
`librte_net_ring.so`、`librte_net_null.so`、`librte_net_tap.so` 的共享库和链接均已安装，
TAP 库的动态依赖没有缺失。项目仍应使用 `tests=true` 和 `werror=true`，
出现 GCC 13 严格告警时修正项目代码，不通过关闭检查绕过。
项目已同步至独立的 `/home/wq/dppd-m3-simulation`，构建目录为其下的 `build`。
首次配置发现 Meson 的 `find_library('rte_net_ring')` 不会借用 pkg-config 或运行时搜索路径，
因此仅在私有环境文件补充 `LIBRARY_PATH`，重新配置后确认 `rte_net_ring found: YES`。
两个 worker 流量测试也已生成，没有通过跳过它们来报告验收成功。

## 实际可用性检查

检查用的独立 C 程序位于 `/home/wq/.cache/dppd-m3-20261018/verify_dpdk.c`，
不属于项目源码。它通过 pkg-config 获取参数，以 `-std=c11 -Wall -Wextra -Werror`
编译并显式链接共享 `rte_net_ring` 库。

| 检查 | 实际结果 |
| --- | --- |
| 普通用户 ring/null | EAL 使用 `--no-pci --no-huge`；`net_ring` 与 `net_null` 均完成队列配置、start、stop、close |
| 私有 namespace TAP/null | sudo 仅在新建的独立网络 namespace 中运行检查；`net_tap` 与 `net_null` 均完成队列配置、start、stop、close |
| 报文池 | 两种场景退出时均为 `pool_available=1024 capacity=1024` |
| 核数配置 | 运行时确认 `RTE_MAX_LCORE=128`，可使用当前虚拟机的 8 个 CPU |
| 接口清理 | TAP 所在私有 namespace 前后均只剩 lo；宿主接口始终为 ens160、ens33、lo，没有残留临时接口 |

这些结果证明安装后的库、PMD 和最小生命周期可用，尚不代表完整项目或硬件 transfer 验收通过。

## 项目核心验收

冻结版本的项目源码已完成 224 项逐文件归一化 LF 的摘要核对，并在 GCC 13.2 下以
`tests=true`、`werror=true` 配置编译成功，没有关闭严格告警或修改源码来适配这台机器。

```bash
cd /home/wq/dppd-m3-simulation
. /home/wq/.local/dppd-m3-20261018/environment.sh
meson setup build -Dtests=true -Dwerror=true
meson compile -C build
python3 scripts/verify_m3.py --build-dir build --report-dir build/m3-installed-environment-acceptance
```

已存在构建目录时直接执行 compile 和验收即可，不需要重复 setup。
统一入口的 inventory、regression、transfer_process、transfer_simulation 四步均通过。
回归包含 55 项单测和单双队列 worker 流量验证；单测无跳过、失败、超时或预期失败。
功能模拟包含七种模板、报文计数、失败补偿和重启演练，正式进程负向验收同时保持未知角色与缺少交换域的拒绝。
结束后没有 dppd、dppctl 或模拟进程残留，构建目录没有残留 socket。

| 结果文件 | 位置 |
| --- | --- |
| 核心验收与逐步日志 | `/home/wq/dppd-m3-simulation/build/m3-installed-environment-acceptance` |
| 正向功能模拟报告与日志 | `/home/wq/dppd-m3-simulation/build/m3-simulation` |
| 供统一核对的日志包 | 135 的 `/tmp/dppd-m3-134-functional-acceptance.tar.gz` |

该轮没有添加 `--tap`，没有重复完整 M2 TAP 验收；报告明确保持
`hardware_acceptance=not_run` 和 `complete_m3_validation=false`。
可复用的软件环境、核心功能模拟通过，不等于路线图要求的 M3 实机整体验收完成。

## 可做与仍不能做的验证

现在可在独立项目目录执行严格构建、控制逻辑与驱动契约模拟、net_ring 正式进程负向验收，
并在明确具备权限时执行独占临时 TAP 的软件流量测试。
测试所用接口和文件应由测试自身创建与清理，不使用现有管理口作为可重绑测试设备。

134 的可见网络设备仍是 VMware 模拟 e1000 和 vmxnet3，没有 NVIDIA/Mellanox PCI 设备，
没有可用的 SR-IOV/representor 或 IOMMU group。安装软件不会改变这个硬件事实。
真实 mlx5/BlueField、物理非 mlx5、硬件 transfer、真实 device reset 后恢复、硬件计数与 CPU 旁路
仍须额外设备和实机证据。

现有 `ens160=192.168.100.2/24` 到 135 数据口的先前 ping 未收到回复，双机数据链路仍未验收。
本次工具准备不修改这一链路的地址、路由或 VMware 网络配置。
