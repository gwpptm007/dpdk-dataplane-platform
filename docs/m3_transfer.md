# M3 端点角色与 transfer 规则图

当前已实现有来源的端点角色发现、七种规则图模板、只读规划和正式整批安装入口。
现有模拟与虚拟端口测试用于验证控制逻辑、拒绝路径和失败补偿，不能据此宣称真实 SmartNIC 卸载验收完成。
环境事实见 [2026-10-18 盘点](m3_environment_20261018.md)，底层方向与计数约束见
[驱动调用契约](m3_flow_contract.md)。

管理协议已升至 **v24**，daemon 与 `dppctl` 必须由同一版本配套构建和更新。
端点角色及 transfer 请求改变了本地结构体布局，旧协议会被明确拒绝为 `EPROTO`，
当前没有版本协商或跨架构 ABI 兼容承诺。现有 net_ring 验收包含旧版本拒绝检查。

## 端点身份来自哪里

`port-show PORT` 同时展示 DPDK 端口、representor 标志、交换域、角色及其来源，
并保留 `role-error`、`ifindex`、Linux 接口名、PCI 设备名和原始 `phys-port-name`。
端口编号只用于定位 ethdev，不能据此猜测物理上联口、PF、VF 或 SF。

角色发现目前针对 `mlx5_pci` / `net_mlx5`，会核对当前网络命名空间的 netlink、sysfs、
接口编号和 PCI 设备身份，再解析原始名称。典型名称如下：

| 原始名称 | 角色 | 额外条件 |
| --- | --- | --- |
| `p0`、`c0p0` | uplink | 不得带 representor 标志 |
| `pf0`、`c0pf0` | PF representor 对应的 PF 角色 | 必须带 representor 标志 |
| `pf0vf1`、`c0pf0vf1` | VF | 必须带 representor 标志 |
| `pf0sf1`、`c0pf0sf1` | SF | 必须带 representor 标志 |

数字溢出、附加字符、接口身份不一致或角色与 representor 标志矛盾都会保守拒绝。
读不到身份或驱动不适用时保留 `unknown` 和错误码，不把普通 ethdev 自动当作 PF。
纯名称解析函数只验证语法；正式发现流程负责证明该名称确实属于当前设备。
公共图构造接口依赖调用方提供经过发现核对的拓扑，不会另行核查纯解析调用者的设备证据；
仅仅解析成功一段字符串，不能代替设备身份验证。

## 七种规则图模板

每个模板都要求安装端口、源端口和目的端口属于同一个已知的 DPDK switch domain，
源端口和目的端口必须不同，也不能只是同一个 switch port 的两个别名。

| 模板 | 请求中的源 → 目的 | 角色要求 |
| --- | --- | --- |
| `pair` | 任意明确端点 → 明确端点 | 允许角色未知，但交换域必须已知且一致 |
| `uplink-vf` | uplink → VF | 双方角色来源可信，代表标志一致 |
| `uplink-sf` | uplink → SF | 双方角色来源可信，代表标志一致 |
| `pf-vf` | PF → VF | 可信角色及可证明的 PF 父子关系 |
| `pf-sf` | PF → SF | 可信角色及可证明的 PF 父子关系 |
| `vf-vf` | VF → VF | 双方角色来源可信，代表标志一致 |
| `sf-sf` | SF → SF | 双方角色来源可信，代表标志一致 |

PF 父子模板要求双方都有相同的 PF 编号。此外，双方必须都有相同的显式 controller 编号；
或者双方都没有 controller 前缀，但有相同、非空且经过身份核对的 PCI `device_name`，
以证明编号来自同一个局部命名域。只有一方知道 controller、controller 不同，或者无法证明
省略 controller 时的共同命名域，都会拒绝，不能因为两边都写着 `pf0` 就认定是同一 PF。

`oneway` 生成一条源到目的的规则；`bidir` 生成两条，第二条反向。
规则 ID 为 `ID` 和 `ID+1`，必须事先都不存在。图固定使用 group 0，可选 `count` 和
`priority:N`，不隐式添加 JUMP 或通配入口。相同交换域、group、优先级下与已有入口相交的
transfer 规则会在驱动调用前拒绝，避免规则含义依赖不明确的匹配顺序。

## plan、apply 与版本约束

```text
transfer-plan ID INSTALL SRC DST TEMPLATE oneway|bidir [count] [priority:N]
transfer-apply ID INSTALL SRC DST TEMPLATE oneway|bidir EXPECTED [count] [priority:N]
```

`INSTALL` 是规则安装所在的 ethdev，`SRC/DST` 是明确的源、目的端点，三者不能混用。
`transfer-plan` 只检查拓扑、角色、规则 ID、重叠和版本空间，返回当前
`repository-generation` 及完整图，不访问 PMD，不安装规则，也不修改 COUNT 或快照。

`transfer-apply` 必须传入刚才 plan 返回的精确全局账本版本，并重新检查完整图。
该版本不是某条规则的 generation；即使另一条无关规则改变了账本，也必须重新 plan。
安装不接受 `any` 或无条件覆盖；重复安装不能直接当作幂等成功，应重新查询实际状态。
这些模板只新建规则，不包含修改既有图的专用接口。

所有边都强制 `require hardware`，使用 represented-port 匹配和 represented-port 输出动作。
底层 flow 同时设置 ingress 与 transfer，不设置 egress。没有相同交换域或 PMD 不支持时，
请求明确失败，不转为软件规则。三层路由、连接跟踪或动态 ND 模式也会拒绝这种绕过软件阶段的规则。

成功的 plan 输出 `hardware-acceptance=not-run`，表示仍需 PMD 校验和硬件报文验证。
成功的 apply 也保留这个标记：创建了驱动 handle 不能证明报文命中，更不能单靠它证明绕过 CPU worker。

## 命令示例

下面只是已经准备好硬件和端点时的操作示例，不能直接用于当前 VMware 环境。
假设 `port-show` 已确认端口 10 为 uplink、端口 12 为 VF，安装端口 10 与两端属于同一已知交换域。
示例中的端口和版本号不是本次环境盘点发现的设备或实机验收结果。

```bash
./build/dppctl port-show 10
./build/dppctl port-show 12
./build/dppctl transfer-plan 40000 10 10 12 uplink-vf bidir count priority:10

# 仅当刚才 plan 确实返回 repository-generation=42 时才使用 42
./build/dppctl transfer-apply 40000 10 10 12 uplink-vf bidir 42 count priority:10
./build/dppctl rule-status 40000 any
./build/dppctl rule-status 40001 any
./build/dppctl count 40000 any
./build/dppctl count 40001 any
```

若 apply 返回 `ESTALE`，重新 plan，再使用新版本。查询允许 `any`，不会放宽 apply 的版本条件。
双向图删除可使用已有 `delete-batch`，分别传入两个 ID 和实际读取到的精确规则 generation；
不要把全局账本版本当作每条规则的版本。
COUNT 查询要求驱动同时提供有效包数和字节数，否则返回 `ENODATA`，不能把缺失值解释为零。

## 整批执行和失败恢复

apply 复用正式的整批创建事务：先校验全批，再创建对象，全部创建完成后发布 desired 账本并保存快照。
第二条校验失败时不创建第一条；创建中途失败会逆序删除已经创建的对象。
这是控制事务与失败补偿语义，不承诺网卡对双向报文做瞬时、原子的整图切换。

若删除补偿也失败，返回 `EUCLEAN`，保留残留 handle，并进入恢复隔离，普通写入被封锁。
应查询 `reconcile-status`、失败历史和持久化状态，再沿既有恢复流程处理；不能清空账本后假装已恢复。
快照保存失败同样可能返回 `EUCLEAN`，此时实际对象和 desired 账本可能已经创建，
需要核对 `persistence-status` 和实际规则，修复存储后显式保存，不能盲目重试整个图。
发布阶段的异常补偿也不承诺把已经推进的历史版本号倒退。

## 现在可以复现什么

现有平台已具备可用的模拟驱动入口、普通用户 net_ring 及独占临时 TAP 测试条件。
可以安装需要的软件或补充仿真，但软件不能生成真实 NIC ASIC 或证明硬件交换旁路。
各层证据的含义如下：

| 验证方式 | 能证明 | 不能证明 |
| --- | --- | --- |
| 角色与图单测 | 名称解析、拓扑拒绝、模板、严格版本、重叠检查 | 机器上存在相应端点 |
| 正式控制事务配模拟驱动 | 全批校验、创建、COUNT、失败补偿和恢复隔离 | 真实 PMD 接受规则，或真实计数有效 |
| 驱动调用契约测试 | 产品适配器传给 DPDK 的方向、匹配和动作符合约定 | ASIC 执行了这些动作 |
| 正式进程 net_ring 负向验收 | 未知角色、缺少交换域时明确拒绝，服务仍正常，软件规则持久化不受影响 | transfer 硬件数据路径成功 |
| TAP 收发回归 | 已声明软件与 TAP 能力范围内的真实报文行为 | 物理 NIC transfer、硬件 COUNT 或 CPU 旁路 |

复现入口：

```bash
python3 scripts/inspect_m3_environment.py
python3 tests/unit/test_m3_inventory.py
python3 tests/integration/m3_transfer.py --build-dir build
```

net_ring 集成脚本启动正式 daemon 与 CLI，检查重复 plan/apply、全部模板拒绝、旧版本拒绝，
以及拒绝前后的 desired 规则、COUNT、快照和 PMD 校验次数不变。
它还验证已有软件规则的创建、查询、删除、重启恢复和进程清理，输出明确标记
`virtual-negative-acceptance` 与 `hardware-acceptance=not-run`。

## 尚未完成或尚未验收

AGE 和 indirect/shared action 尚未形成规则 IR、正式管理与完整资源生命周期，不能登记为已实现。
现有通用失败恢复和模拟补偿不等于端点移除、真实 device reset 后已经完成硬件 reconciliation。
这些变化还需要实际设备、端点身份重新发现、handle 失效和规则重建的端到端验证。

当前未完成 mlx5/BlueField 与物理非 mlx5 PMD 的可复现实机 transfer 报告，
未证明卸载报文不经过 CPU worker，也未完成吞吐、时延或规则规模的硬件性能验收。
VMware 模拟 e1000、vmxnet3、net_ring 和 TAP 均不能替代这些证据。
其他厂商的角色发现目前没有可信解码器；在交换域已知时只能考虑通用 `pair`，
具体 represented-port 能力仍必须由该 PMD 校验和实机报文测试确认。

主要实现对应 [端点身份](../include/dppd/topology.h)、[规则图接口](../include/dppd/transfer.h)、
[图与控制事务](../lib/control/transfer.c)、[正式 CLI](../app/dppctl/transfer_cli.c) 和
[虚拟负向验收](../tests/integration/m3_transfer.py)。
