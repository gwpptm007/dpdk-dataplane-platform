# M3：DPDK 21.11.9 驱动能力与目标适配边界

本文记录对 DPDK 官方 `v21.11.9` 源码、21.11.9 文档和测试机已安装头文件的只读核查。
结论只适用于这个版本，不能直接外推到其他 DPDK、厂商补丁、网卡型号或固件。
这里没有新增实机验收结果，也不是设备购买建议。

需要分开看三件事：驱动源码实现了某个接口、项目能够规划和编译某种规则、真实设备按预期转发报文。
前两项成立，仍不能代替最后一项。

## 能力矩阵

表中的“有实现”指官方源码存在对应处理路径，具体设备、方向、权限和动作组合仍须真实 PMD 校验。
“无实现”指这个版本的对应驱动没有该解析或回调路径，不表示其他版本永远不支持。
COUNT 指规则私有计数；共享 COUNT 指通过 indirect action 句柄创建、绑定、查询和销毁的对象。

| 驱动与路径 | PMD 宿主 `if_index` | `REPRESENTED_PORT` 匹配项 | `REPRESENTED_PORT` 动作 | 私有 COUNT | AGE | indirect/shared COUNT |
| --- | --- | --- | --- | --- | --- | --- |
| mlx4 | 从已关联 Linux 接口取得 | 无实现 | 无实现 | 无实现 | 无实现 | 无实现 |
| sfc MAE | 未填写 | 有实现 | 有实现 | 有实现，有服务核等前提 | 无实现 | 无实现 |
| bnxt TruFlow/ULP | 未填写 | 有实现 | 有实现 | 有实现及查询路径 | 无实现 | 无实现 |
| mlx5 DV | 从关联接口取得 | 无实现，源端口仍使用旧 `PORT_ID` 等匹配 | 有实现 | 有实现 | 有实现，须核对设备能力 | 有实现，须核对设备能力及域 |

mlx4 的接口编号赋值见
[mlx4_ethdev.c](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/drivers/net/mlx4/mlx4_ethdev.c)，
流能力范围见其 [feature 表](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/doc/guides/nics/features/mlx4.ini)。
sfc、bnxt 的匹配和动作入口分别见
[sfc_mae.c](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/drivers/net/sfc/sfc_mae.c)
和 [ulp_rte_parser.c](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/drivers/net/bnxt/tf_ulp/ulp_rte_parser.c)。

mlx5 的匹配项与动作必须分别核查，不能因为动作支持便推断匹配项也支持。
这个版本的 [feature 表](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/doc/guides/nics/features/mlx5.ini)
只在动作部分声明 `represented_port`；
[mlx5_flow_dv.c](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/drivers/net/mlx5/mlx5_flow_dv.c)
实现了该动作，而源端口匹配仍走 `PORT_ID`。
对整个 `drivers/net` 的核查中，AGE 动作及 indirect 生命周期回调的实现只出现在 mlx5；
其公共回调分派见
[mlx5_flow.c](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/drivers/net/mlx5/mlx5_flow.c)。

项目当前的 `pair` 固定编译为 `REPRESENTED_PORT + ETH` 匹配，再执行可选计数、可选 AGE 和端口转发。
所以 mlx5 21.11.9 单独具备 AGE、共享 COUNT 和目标端口动作，仍不能证明当前整套 `pair` 规则可用。
本文不建议静默替换成旧 `PORT_ID`；这种版本适配需要另外核对语义和测试。
DPDK 对旧端口表达和新端口表达的说明见
[Flow API](https://doc.dpdk.org/guides-21.11/prog_guide/rte_flow.html)。

## 接口身份不是 transfer 能力

DPDK 明确允许 `rte_eth_dev_info.if_index=0`，意思是没有绑定的宿主接口。
公共层先清空 `dev_info`，驱动没有填写时不会自动获得 Linux 接口编号。
依据见 [字段定义](https://doc.dpdk.org/api-21.11/structrte__eth__dev__info.html)
和 [rte_eth_dev_info_get](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/lib/ethdev/rte_ethdev.c)。

通用身份采集只在 PMD 给出非零编号时，核对当前网络命名空间的接口、sysfs、PCI BDF 和第二次观察。
它不为 VFIO 端口制造 netdev，也不会遍历同 BDF 接口后挑一个填空，详细契约见
[通用 PCI 与接口身份](m3_generic_identity.md)。
mlx4 是已有宿主接口身份却不具备本项目 transfer 规则形态的例子；
sfc、bnxt 则是具有相关 flow 路径，却不能直接走这条非零 ifindex 身份链的例子。

项目 `pair` 规划器允许 UNKNOWN 角色，也不全局要求存在 Linux 接口。
它仍核对源、目的、安装口、交换域、交换端口别名和版本，并要求真实 PMD 校验。
因此不能把“没有可信 Linux 接口”写成“生产规划器必然拒绝 pair”。
需要 Linux netdev 的硬件 runner 也不能据此宣称已经覆盖无 netdev 的 PCI 目标；
PCI-only runner 必须另有明确配置、实际端点核对和报文证据，不能仅跳过原来的检查。

## 可进入目标能力核查的两个候选

以下是源码层面的候选范围，不是已经通过项目实机测试的设备清单。
任何一个候选都应先验收普通 `pair` 与私有 COUNT，不能默认追加 AGE 或共享 COUNT。

### sfc：具有 MAE 管理权限的端点

可核查范围包括实际具备 MAE 的 sfc 设备，例如官方支持列表中的 Alveo SN1022。
不能把所有受 sfc 驱动支持的旧型号都当成 MAE 设备。
transfer 要求安装口处于 MAE ADMIN 状态，且这个版本只接受 group 0；
是否开放具体能力还依赖固件，见
[官方驱动说明](https://doc.dpdk.org/guides-21.11/nics/sfc_efx.html)
及 [属性校验](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/drivers/net/sfc/sfc_flow.c)。

COUNT 要求 counter queue 已初始化并且有可用 service lcore；representor 代理也需要 service lcore。
部署和验收必须记录实际 EAL 服务核配置，不能仅记录 worker 核数。
相关条件见
[MAE 计数动作](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/drivers/net/sfc/sfc_mae.c)、
[representor 支持检查](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/drivers/net/sfc/sfc.c)
和 [服务核查找](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/drivers/net/sfc/sfc_service.c)。

安装口公开实现 `rte_eth_representor_info_get`，可以取得 controller、PF 和 representor 范围。
代表端口提供交换域和交换端口，但自己的 flow ops 只负责寻找 transfer proxy，没有普通规则安装回调；
首期目标应明确安装到经核对的 MAE 管理口。
代表端口自己的 ops 没有固件查询，不能把查询不支持改写为它自己的“固件已知”。
来源见 [sfc_ethdev.c](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/drivers/net/sfc/sfc_ethdev.c)
和 [sfc_repr.c](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/drivers/net/sfc/sfc_repr.c)。

### bnxt：启用 TruFlow 的目标

官方说明将 host-based flow table 管理限定在 Whitney+、Stingray 和 Thor 设备。
BCM57508、BCM57504、BCM57502 等可作为具体目标进一步核查，但型号名字本身不能证明
TruFlow 已启用、固件满足要求或某组端点能够互通。
依据见 [官方驱动说明](https://doc.dpdk.org/guides-21.11/nics/bnxt.html)
和 [PCI 设备表及 flow ops 选择](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/drivers/net/bnxt/bnxt_ethdev.c)。

ULP 解析器将 DPDK 端口转换到自己的内部端口数据库，支持 represented-port 匹配和转发。
这里的内部 `ifindex` 不是 Linux 宿主接口编号。
COUNT 查询有独立实现；普通 `rte_flow` 回调表没有 indirect 创建、查询和销毁回调。
依据见 [端口解析](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/drivers/net/bnxt/tf_ulp/ulp_rte_parser.c)、
[动作表](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/drivers/net/bnxt/tf_ulp/ulp_rte_handler_tbl.c)
和 [flow 回调表](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/drivers/net/bnxt/tf_ulp/bnxt_ulp_flow.c)。

代表端口由 parent PCI device 创建，提供 representor 标志和交换域；其交换端口取 VF 编号的 `0x7FFF` 掩码。
这个版本没有实现公开的 representor-info 回调，代表端口自己的 ops 也没有固件查询。
不能把生成的 `net_BDF_representor_N` 名称单独作为完整身份，更不能把 N 解释成所有设备通用的角色编号。
依据见 [bnxt_reps.c](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/drivers/net/bnxt/bnxt_reps.c)、
[端口掩码](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/drivers/net/bnxt/bnxt_reps.h)
和 [代表端口创建](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/drivers/net/bnxt/bnxt_ethdev.c)。

## 后续边界应分三类记录

1. **目标 PCI 端点身份与验收适配**：需要真实型号、固件、绑定方式、EAL allowlist/devargs、
   完整 ethdev 名称、安装口、同域端点和外部流量端点的对应关系
2. **六个命名模板的角色证据**：通用身份仍保留 UNKNOWN，当前模板只接受既有 mlx5 来源；
   若增加其他来源，需要先定义可信证据及明确的规划器支持，不能仅解析相似名称
3. **驱动缺少 AGE/indirect**：sfc、bnxt 在本版本没有这些路径，需要另行扩展驱动或验证其他版本；
   不能通过补接口编号、共享私有计数值或用 COUNT 轮询来伪造支持

PCI-only pair 验收是否需要增加生产字段，应由目标契约决定，不能先假定必须升级 ABI。
可以在独立工具中核对既有 v27 的 BDF、驱动、完整 ethdev 字段、交换域和端口，并保留明确的配置来源；
如果记录 parent/backer 的固件，报告必须写清查询端点和关联范围，不能冒充代表端口的独立查询结果。
任何由用户配置提供的关系都应与运行中的 PMD、PCI 和报文观察交叉核对。

最终实机验收仍需要正反向原始帧、PMD COUNT、CPU worker 对照、删除后行为、故障清理和恢复记录。
flow 创建成功不能独立证明实际转发或 CPU 旁路；停止设备也不能通用于证明所有 indirect 对象已经清空。
本文没有执行网卡绑定、复位、端口变更或硬件发包，所有候选的项目实机验收状态均为未运行。
