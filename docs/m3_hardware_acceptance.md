# M3 实机验收入口与能力矩阵

`scripts/verify_m3_hardware.py` 独立执行实机门禁，不改变原有虚拟/软件验收入口。
默认只盘点和保存报告，缺少硬件、身份或接线信息时明确返回 **blocked**。
安装了 mlx5 库、发现一个 PCI 候选、`rte_flow_create` 成功或者模拟测试通过，都不能单独产生硬件通过结论。

目前自带基础 runner：在已有专用硬件 daemon 上创建一条单向 transfer + COUNT 规则，
暂停该 daemon 的全部线程，由另一台机器的两个独立真实接口收发本轮报文，恢复 daemon 后核对
外部接收字节、硬件 COUNT 和 CPU worker 统计，最后用精确版本删除本轮规则并检查健康状态。
对端 helper 随项目提供，使用 Python 标准库 `AF_PACKET`，不需要使用者自己编写 adapter。

同一入口还提供 AGE 和共享 COUNT 生命周期 runner，每轮报文也使用真实暂停窗口与外部抓包。
AGE 核对驱动查询、再次命中续期、闲置自动删除和旧 COUNT 消失；共享 COUNT 核对双向汇总、
在用删除被拒绝、删除一条后继续累计、引用归零及精确销毁。
另有显式选择的 `planned_reset` runner，调用正式 v27 `device-reset`，可以分别核对 COUNT、
共享 COUNT 和 AGE 的物理复位恢复。当前虚拟环境没有执行这些 runner 的硬件正向验收。
**真实端点移除仍需人工操作，没有自动接线 runner**。
因此已实现阶段成功只得到 `partial`，该版本入口不会输出完整 M3 `passed`，不会返回成功退出码零。
只有选择 `planned_reset` 且完整门禁通过时才允许正式 PMD 端口组 stop/reset/reconfigure。
固件升级、驱动重绑、SR-IOV 切换和网络配置均不在这个脚本的操作范围内。

## 当前环境的实际结果

2026-10-18 重新只读检查：135 和 134 仍都是 VMware，DPDK 均为 21.11.9。
135 有两块 `8086:100f / 15ad:0750` 模拟 82545EM 和一块 `15ad:07b0` vmxnet3；
134 有一块同类模拟 82545EM 和一块 vmxnet3。两机管理口都是 `ens33`，devlink 没有设备端口，
没有可见的 15b3 设备、SR-IOV 或 IOMMU group，firmware 字段为空，hugepage 数为零。
没有取得独立真实硬件及物理线缆的证据，不能执行本入口的硬件正向阶段。

非 mlx5 还存在当前软件适配缺口：生产角色发现路径尚未为这些 PMD 采集完整的
`interface_name` 和 `phys_port_name`，因此严格接口身份核对会保守 blocked。
`pair` 允许 UNKNOWN 角色，不会放宽接口与 PCI 的独立核对；该类别需要先完成目标 PMD 的身份/角色适配，
再补真实设备与报文验收，不能把剩余工作只登记为“缺硬件证据”。

这里的实机缺口不会因软件工具安装而消失。既有软件环境和功能模拟仍可使用，详见
[环境盘点](m3_environment_20261018.md)、[软件模拟环境](m3_simulation_environment.md)、
[transfer 规则图](m3_transfer.md)、[底层 flow 契约](m3_flow_contract.md) 和
[复位模拟边界](m3_reset_simulation.md)。

## 一键只读预检

在项目源码根目录执行：

```bash
python3 scripts/verify_m3_hardware.py \
  --build-dir build --report-dir build/m3-hardware-preflight
```

134 先加载已安装的私有环境：

```bash
. /home/wq/.local/dppd-m3-20261018/environment.sh
cd /home/wq/dppd-m3-simulation
python3 scripts/verify_m3_hardware.py --build-dir build --report-dir build/m3-hardware-preflight
```

当前虚拟环境预期退出码是 `2`，报告 `status=blocked`、`success=false`、
`hardware_acceptance=not_run`、`complete_m3_validation=false`。
这表示门禁正确识别了缺少实机条件，不是实机验收通过。

报告文件格式见 [报告 schema](m3_hardware_report.schema.json)。所有阶段先保留未知或 blocked，
只有本轮运行保存了对应原始证据后才更新单个能力行。

## 接线和参数

复制 [可填配置示例](m3_hardware_config.example.json)，逐项替换 `FILL`。
示例中的设备、端口、版本、接线和对端均为占位，不是本次环境的观察结果。

| 配置项 | 必须证明的事实 |
| --- | --- |
| `ports.install` | 规则实际安装 ethdev，不能把它当成源或目的端点的别名 |
| `ports.source`、`ports.destination` | 不同的源/目的 ethdev，各自有已核对接口和 PCI 设备 |
| `port_id / pci_address / interface` | DPDK 端口、Linux 接口和 sysfs PCI 独立一致 |
| `pmd / device_name / firmware` | 与当前 daemon 的 `port-show`、`capability-show` 原始结果完全一致 |
| `role / switch_domain` | 角色保留来源；安装、源和目的都在同一个已知交换域 |
| `eal_devargs` | 必须是当前测试 daemon 命令行中真实存在的完整参数，不接受虚拟设备或 `--no-pci` |
| `daemon_pid / socket` | 已启动的专用测试 daemon，Unix socket 的 `SO_PEERCRED` 必须指向这个 PID |
| `peer.ssh_argv` | SSH 的参数数组，例如 `ssh -o BatchMode=yes tester@peer` 分成独立元素 |
| `peer.tx_interface / rx_interface` | 对端上两个不同的真实 PCI 网络接口，均不能承载管理连接 |
| `physical_links` | 源/目的到真实物理口、VF/SF 数据端点和对端接口的逐段连接说明 |
| `rule_id / priority / template` | 专用且尚不存在的规则 ID、优先级和本轮七模板之一 |
| `stages` | 默认 `basic, age, shared_count`，可另加 `planned_reset`，不会把未执行阶段登记通过 |
| `planned_reset.device_ports` | daemon 配置的完整端口组，每个端口提供与图端点同样完整的身份，额外端口也不能省略 |
| `planned_reset.snapshot_path` | 当前 daemon 实际 `--state-path` 的绝对路径，不能填另一份复制文件 |
| `planned_reset.allow_full_group_reset` | 默认 false，只有确认完整组专用且独立管理后显式改成 true |
| `planned_reset.modes` | 必含 `count`，可增加 `shared_count`、`age`，每种模式单独执行一轮物理组复位 |

默认配置执行 `basic`、`age` 和 `shared_count`；只需采集基础阶段时可将 `stages` 改为 `["basic"]`。
AGE 阶段的专用规则在基础规则删除后复用 ID；共享阶段再使用该 ID 和 ID+1，
共享对象使用独立命名空间中的同一 ID。所有这些规则和共享对象都必须事先不存在。

连接说明来自使用者登记，不能把文字本身当成已经观察到物理线缆。
本轮外部实际接收、真实硬件 COUNT 和 daemon 全线程暂停期间的转发共同补充路径证据。
复杂 VF/SF 场景仍需用户准确描述数据端点所在机器和实际接线；默认 runner 不自动创建 VF、SF 或 representor。
辅助总线 SF 或其他无法独立映射到完整 PCI parent 的身份会保守 blocked，不能用相似名称绕过门禁。

管理保护同时检查默认路由和当前 SSH 本地地址，保护整个管理 PCI 设备。
即使一个 representor 没有 IP，只要与管理接口共用被保护的 PCI 设备，也拒绝测试。
基础、AGE、共享 COUNT 和复位阶段都沿 VF `physfn` 归到父 PF，DUT 和 peer 使用相同规则。
对端必须取得有效的 `SSH_CONNECTION` 并匹配真实本地地址；缺失、格式错误或未匹配时，
即使存在默认路由也不继续，无法识别任何管理接口时同样拒绝。
路由、地址、PCI 映射或管理身份未知时不会继续发包、创建规则或暂停进程。
脚本不会用“没有默认 IP”推断某接口可随意使用。

## 实机基础阶段

只有已经准备好独立管理链路、专用测试 daemon 和真实对端时执行：

```bash
python3 scripts/verify_m3_hardware.py \
  --build-dir build --config /absolute/path/hardware-config.json \
  --report-dir build/m3-hardware-run --execute
```

`--execute` 明确授权该配置对应的专用测试动作：严格重新构建、创建一条新规则、
短暂 `SIGSTOP/SIGCONT` 指定测试 daemon、SSH 对端原始帧收发、精确删除规则。
它不启动或接管另一套 daemon，也不安装驱动或改网络。
只有另外显式配置 `planned_reset` 才包含本节后述的正式物理端口组复位动作。
运行中的 daemon 二进制与本次构建不一致、协议不匹配、固件未知或身份不一致时停止。
不要把业务 daemon 的 PID 和 socket 填入配置。

对端账号必须已经能够 SSH 登录，并具有打开 `AF_PACKET` 原始套接字的权限。
脚本不安装包、不申请 sudo、不保存密码；权限不足会保存原始错误并报告 blocked。
SSH 远端执行的命令固定为 Python，接口和报文参数通过标准输入传递，不拼接进 shell。
`ssh_argv` 只接受明确的最后一个主机名、`-p/-i/-J` 及受限的 BatchMode、ConnectTimeout、
StrictHostKeyChecking 选项，不接受额外远端命令或 ProxyCommand。
对端不需要预先复制 helper 文件，入口把项目自带 helper 送入该次 SSH 会话执行。

基础阶段包含两个带本轮随机 nonce 的 96/128 字节 Ethernet 帧，每种提交 8 个。
保存的 TX PCAP 是 `AF_PACKET` 完整提交记录，**不是物理线缆上的抓包**；
RX PCAP 来自对端另一真实接口的入站捕获，排除本机 outgoing 副本。
验证器逐字节检查帧、缺包/重包/改写，并要求 COUNT 包数和字节增量完全相符。
这里按 DPDK 帧长度核对，不包含以太网 FCS；若具体 PMD 的计数语义不同会失败，不能自动放宽。

CPU 旁路证据由入口自己采集：核对 Unix socket PID、进程开始时间和正在运行的二进制内容，
连接类型与正式管理服务一致，使用 `SOCK_SEQPACKET`；错误的 STREAM 服务不能充当管理身份。
打开 Linux pidfd 并在前后再次核对身份，只有全部一致才允许发送暂停信号。
没有 pidfd 的 Python/内核组合保留 blocked，不退回裸 PID 发信号。
入口先保存 worker 统计，再实际通过固定 pidfd 暂停 daemon，
读取 `/proc/PID/task/*/status` 确认全部线程停止，在该窗口执行外部收发，
再次核对线程仍停止，恢复后检查 COUNT 增量与 worker RX/TX 包数/字节数不变。
配置里的 `success`、`worker_paused` 或任何离线 adapter 声明都不能替代这些观察。
这是本次暂停窗口内的路径证据，不是吞吐率或 CPU 利用率性能结论。

## 可选的正式物理复位阶段

该阶段要求配套 v27 daemon 和 CLI，使用正式 `device-status` 与 `device-reset EXPECTED_SEQUENCE`。
`device-reset` 的作用范围是 **daemon 配置的完整端口组**，不只是一张 transfer 图的三个端点。
不能把承载 SSH、默认路由或其他业务的额外配置端口遗漏出清单。

先在专用 daemon 的应用参数中显式提供 `--ports 10,12 --queues 1 --state-path /absolute/test-rules.bin`，
参数位于 EAL 的 `--` 之后。相应配置保留 `ports` 中的三个图端点，再填写示例中的
`planned_reset.device_ports`，并将 `stages` 增加 `planned_reset`。
全量端口身份、实际 `--ports`、`device-status` 的 `device-ports count=N ids=...` 必须一致。
没有快照、使用相对快照路径、缺少端口组证据或参数重复时均不执行复位。

```json
{
  "stages": ["basic", "planned_reset"],
  "planned_reset": {
    "allow_full_group_reset": true,
    "snapshot_path": "/absolute/test-rules.bin",
    "modes": ["count", "shared_count", "age"],
    "device_ports": ["此处必须填写配置示例中完整的端口对象，不能使用字符串"]
  }
}
```

上段只说明需要调整的字段，不是一份可执行的配置；完整对象格式见配置示例。
确认填写后仍使用同一条 `--config ... --execute` 命令。
只采集 COUNT 复位恢复时，将 `modes` 显式设为 `["count"]`。
未选择共享或 AGE 模式的物理复位报告不包含这两项恢复能力的通过结论。

入口在第一次创建规则前检查专用 daemon 的 desired/actual rule/shared 数量全部为零，
并核对完整组的实际 PMD、device、固件、角色、交换域及 Linux PCI 身份。
每次复位前重新盘点管理接口、沿 VF `physfn` 核对 PCI parent，保护与管理 PF 共用的 VF 和 representor。
路由/连接跟踪/邻居模式不纳入这一专用纯硬件阶段。
正式 C 复位入口还会独立执行管理保护，Python 的配置和预检不能替代它。

每种模式执行以下证据链：

1. 创建本轮专属硬件规则，先取得暂停全部 CPU 线程期间的外部 nonce 帧、逐字节 RX PCAP 和硬件计数增量
2. 保存规则完整内容和精确 generation、共享对象 generation/revision、持久化状态及原始快照文件
3. 读取当前精确设备 sequence，零是合法值；单次限时 120 秒调用正式 `device-reset`，不使用 any，不自动重试
4. 核对完整组 stop/reset/configure/start 位图、attempts/resets/recoveries 各增加一次、最终 phase=READY 和真实 worker health
5. 重新核对端口角色/交换域/PMD/固件等身份，规则与共享版本、共享 revision、快照原始字节保持不变，硬件 COUNT 从零开始
6. 再次暂停 CPU，取得复位后的本轮外部报文与真实计数证据，共享模式还验证反向计数汇总
7. AGE 模式验证实际空闲秒数从复位前至少两秒重新开始，再观察本轮命中后的完整闲置期限与自动删除

`snapshot-before.bin` 与 `snapshot-after.bin` 保存在本轮证据目录，报告记录其 SHA256。
COUNT、共享 COUNT 和 AGE 使用不同证据目录，不把一次基础复位推断成所有动作均能恢复。
只有 CLI 成功、计数复位、快照保持和复位后的外部硬件流量全部一致，才更新物理复位能力行。
这些证明依赖当前正式实现实际增加的 reset/recovery 计数，不能只靠 `device-reset` 返回零。

明确的 `ENOTSUP` 或 `ENODATA` 记录为能力 `rejected`，不表示实机通过。
只有返回前置拒绝，且完整设备状态、规则/共享版本、计数和快照均未改变，才允许精确清理本轮已知对象。
复位请求发出后超时、隔离、部分 PMD 失败或任何恢复证据冲突时保留现场，
设置 `planned_reset.preserve_scene=true`，不删除未知旧句柄，不执行 acknowledge-clean，也不自动再次复位。
后续应先检查结构化诊断、原始命令输出与隔离原因，再明确安排外部清理或重启。

## 真实移除的人工证据流程

当前脚本不控制物理插拔、电源或交换机，`physical_removal` 始终保留 blocked。
不会把软件事件注入、删除虚拟接口、解绑驱动或离线日志登记成真实硬件移除通过。
配置中的 `manual_physical_removal` 只是一张人工准备单，不会触发任何操作，也不能导入成功结论。

实机具备独立带外管理后，先由操作者明确记录：完整设备组与待移除的精确 BDF/端口、
线缆或带外控制器、操作类型与 UTC 时间、DUT 和 peer 的本轮报告目录及独立管理链路。
操作前保存 `device-status`、端口/固件身份、规则/共享版本、快照摘要和外部真实流量。
人工移除后保存连续原始设备事件、对应 removal/destroy 位图与 sequence、worker 停止、
health 隔离以及外部报文停止的证据；禁止针对未知旧对象发起清理。
重新插入后仍需明确外部清理或新的 daemon 生命周期，再重验设备身份和真实恢复流量。
在自动验证这一流程的正式 runner 落地以前，这些人工文件只供审查，不能把本工具的 blocked 改为 passed。

## 能力矩阵和退出码

| 行 | 当前 runner 的处理 |
| --- | --- |
| discovery | 独立 PCI、PMD/device/firmware、交换域、外部 peer 和管理保护 |
| 七种 transfer 模板 | 本轮显式选择的单向模板验证，其他模板仍 blocked |
| COUNT / CPU bypass | 与本轮外部真实帧和暂停窗口一起核对 |
| AGE | 三秒配置的真实 AGE 查询、两轮命中续期、闲置账本删除与旧 COUNT 消失 |
| shared COUNT | 双向规则共用真实对象，逐轮计数汇总、在用 EBUSY、删一边继续累计和精确销毁 |
| physical reset recovery | 显式选择 planned_reset 后核对全组正式 PMD reset、版本/快照保持、计数归零和恢复后的真实流量 |
| physical removal | 保留 blocked，人工准备单不能替代受验证的真实移除与恢复证据 |
| mlx5 / non-mlx5 | 只更新本轮实际验证的类别，非 mlx5 还需补齐生产接口身份采集适配，再补独立实机报告 |
| performance | `out_of_scope`，属于 M4，不作为 M3 门槛 |

| 退出码 | 报告状态 | 含义 |
| --- | --- | --- |
| `1` | failed | 构建、证据、执行或清理出现冲突/失败，需检查原始日志 |
| `2` | blocked | 缺硬件、参数、权限、可信身份，或 PMD 明确拒绝，未完成本轮基础验收 |
| `3` | partial | 本轮已列出的硬件阶段成功，仍缺其余 M3 实机阶段或另一类设备报告 |

PMD 明确拒绝会记录为该能力行 `rejected`，其 `capability=rejected`，不会改成 supported。
缺证据使用 blocked/unknown，不把“看不到”解释成硬件永久不支持。
当前 schema 的总体 `success` 和 `complete_m3_validation` 固定为 false。
完整 M3 仍需 mlx5/BlueField 与非 mlx5 两类可复现实机报告，以及各项 runner 的真实结果、真实移除和设备复位恢复证据。

## 报告与清理

报告保留源码逐文件摘要、构建来源路径、严格选项、二进制 SHA256、run ID、配置摘要、
独立盘点、真实命令 argv/stdout/stderr/返回码、对端盘点与 PCAP、暂停线程原始状态和清理结果。
`ctl-*.json` 按执行顺序保留，不以汇总的 supported 元素掩码推断任意组合能力。
每轮原始证据保存到随机 run ID 子目录，顶层 `report.json` 指向本轮目录并登记文件 SHA256，
旧报文文件和命令日志不会混入新一轮结论。缺少物理设备时在主机构建或运行绑定前停止，
报告仍保留本次工具摘要和独立盘点，不把不存在的硬件执行包装成已验证的构建结果。
开始时立即写入 `in_progress=true` 且未完成的报告，撤销上轮结果；正常结束才设置完成时间。

每条本地阶段命令使用独立进程组和限时，超时 TERM 后再 KILL 并保存原始输出。
对端 helper 本身也有有界采集窗口，关闭套接字时不改变现有链路。
正常失败、SIGINT 或 SIGTERM 会走 finally，先通过固定 pidfd 恢复暂停的测试 daemon，
再分别尝试删除本轮精确版本规则并确认健康；恢复、删除、健康检查的错误独立记录，不因某一步失败跳过后续清理。
物理复位发起后的未知状态适用前述保留现场规则，不使用这一常规删除路径尝试处理旧硬件句柄。
SIGKILL、整机掉电或外部强制断连无法由 Python finally 保证清理，报告与原始日志不能被当成成功。

完成报告后发现源码变化会失败。报告绑定的是这次执行的源码和二进制，
不证明任意离线复制文件的真实性；本工具没有接收外部 `success=true` 文件的“导入通过”入口。

门禁测试可直接运行：

```bash
python3 tests/unit/test_m3_hardware.py
```

这些反例夹具只验证门禁自身，不能登记为真实 ASIC、报文线缆或 CPU 旁路验收。

## 首轮工具验收结果

2026-10-18，基于设备恢复交付 `23a6a52`，135 系统 GCC 11、135 ASan/UBSan 与
134 私有 GCC 13 三套严格构建各 70/70 单测通过，其中各含 58 项硬件门禁测试且无跳过。
每套六步 M3、四步核心软件回归、五组共享恢复、四组正式事件和 31 组模拟全部通过。
本轮只增加硬件工具与测试，完整 TAP 回归沿用设备恢复交付的两套十二步记录，不把旧结果
改成新源码摘要的完整 TAP 验收。

三套报告的 255 项源码逐项相同，摘要表按键排序、紧凑 JSON 编码后的 SHA256 为：

```text
bccfe6b265d62cf5be1b6e41b84bfabf076dd7b9069a222c3eb1d1fc8a34526d
```

58 项反例包括正式 SEQPACKET 身份与错误 STREAM 协议、空管理证据、SSH 本地地址错配、
VF 与管理口同 PF、未知 parent、旧报告、PCAP/COUNT/CPU 不一致及复位部分失败保留现场。
135/134 的实际只读预检均返回 2、blocked/false，两份原始报告通过 JSON Schema
2020-12 验证，工具摘要与本轮已测源码相同。没有运行真实报文或物理 reset 正向阶段。
原始报告与日志保存在 `build/native-check/m3-hardware-artifacts/`，M3 始终保持未完成。
