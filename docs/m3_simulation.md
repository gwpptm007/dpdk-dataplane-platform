# 不用实体网卡的 M3 功能演练

现在可以用真实 `dppctl` 完成正向演练：规划和安装 transfer 图、输入报文字节、查看
转发端点与 COUNT，再制造失败，观察回滚、恢复隔离和快照重放。演练不需要 root、
hugepage 或 Mellanox 设备。开发工具与 134 的私有 DPDK 环境见
[环境准备](m3_simulation_environment.md)。

模拟器是 `tests=true` 才构建的独立程序 `tests/integration/dppd_transfer_sim`，
它复用正式管理协议、控制事务、规则账本和持久化，只替换驱动入口与端点夹具。
正式 `dppd` 不链接模拟代码，管理协议仍为 v24。

## 一条命令运行完整演练

134 已准备好以下目录和环境：

```sh
source /home/wq/.local/dppd-m3-20261018/environment.sh
cd /home/wq/dppd-m3-simulation
python3 scripts/simulate_m3.py demo --build-dir build
```

在其他 Linux 开发机先按 [构建说明](build_and_run.md) 准备真实 DPDK，再运行：

```sh
meson setup build -Dtests=true -Dwerror=true
meson compile -C build
python3 scripts/simulate_m3.py demo --build-dir build
```

脚本逐项检查七种模板的单向和双向转发、逐字节帧内容、方向独立计数、优先级切换、
14/2048 字节帧、错误输入无副作用、查询失败不清计数、批量创建与补偿失败、隔离后
禁止转发及快照重启恢复。任何一项失败都返回非零，不会继续报告全部通过。

结果与模拟器日志保存在 `build/m3-simulation/`。报告明确保留
`simulation=true`、`hardware_acceptance=not_run`、`complete_m3_validation=false`，
并核对构建目录来源和演练前后的源码摘要。

## 手动使用真实管理命令

第一个终端启动独立服务，目录必须属于当前用户、权限为 0700，并专供模拟使用：

```sh
python3 scripts/simulate_m3.py serve --build-dir build --directory /tmp/dppd-my-m3-demo
```

第二个终端操作同一构建生成的客户端：

```sh
./build/dppctl --socket /tmp/dppd-my-m3-demo/ctl.sock port-show 0
./build/dppctl --socket /tmp/dppd-my-m3-demo/ctl.sock port-show 2
./build/dppctl --socket /tmp/dppd-my-m3-demo/ctl.sock transfer-plan 100 0 0 2 uplink-vf bidir count
```

从 plan 结果读取 `repository-generation`。以下以新目录首次运行返回 0 为例；已有
规则时必须使用实际版本，不能照抄 0：

```sh
./build/dppctl --socket /tmp/dppd-my-m3-demo/ctl.sock transfer-apply 100 0 0 2 uplink-vf bidir 0 count
python3 scripts/simulate_m3.py inject --io-socket /tmp/dppd-my-m3-demo/io.sock \
  --source 0 --frame 02000000000202000000000188b5
./build/dppctl --socket /tmp/dppd-my-m3-demo/ctl.sock count 100 any
python3 scripts/simulate_m3.py status --io-socket /tmp/dppd-my-m3-demo/io.sock
```

这条 14 字节帧应输出目的端点 2、规则 100、原始帧和 `simulation=yes`，COUNT 为
`hits=1 bytes=14`。它没有发到内核接口或真实网卡。

可以通过 `fault` 设置某类操作第 N 次后续调用失败一次，`clear-faults` 仅清除故障
计划，不清空规则和计数。例如第二次创建失败可用于演练双向图的中途失败：

```sh
python3 scripts/simulate_m3.py fault --io-socket /tmp/dppd-my-m3-demo/io.sock create 2
python3 scripts/simulate_m3.py clear-faults --io-socket /tmp/dppd-my-m3-demo/io.sock
```

`validate/create/remove/query` 分别注入 `ENOTSUP/ENOSPC/EBUSY/EIO`。要观察失败，
在设置后执行相应操作；上面最后一条是独立的故障清除示例。
补偿失败时 `status` 会区分 desired 账本与 installed 模拟句柄，不能把残留对象当作
正常转发规则。`reconcile-retry` 后仍需重启，符合正式控制面的隔离约定。

第一个终端按 Ctrl+C 正常退出会清理自己的两个 socket，保留模拟规则文件，
再次使用相同目录启动会从快照恢复规则，COUNT 从零重新累计。

## 固定端点夹具

| 端点 | 角色 | 名称夹具 | 交换域 |
| --- | --- | --- | --- |
| 0 | uplink | p0 | 77 |
| 1 | PF representor | c0pf0 | 77 |
| 2、3 | VF representor | c0pf0vf1、c0pf0vf2 | 77 |
| 4、5 | SF representor | c0pf0sf1、c0pf0sf2 | 77 |
| 6 | unknown | 无名称 | 77 |
| 7 | uplink | p1 | 88 |

`pair` 允许在同一已知交换域中使用角色未知的端点；其他模板要求对应角色。
端点 7 可用于验证跨交换域拒绝。所有端口的 driver 都显示 `simulation`，名称和
PCI 值只是合成夹具，没有执行真实设备发现。

## 支持范围与状态隔离

报文演练只解释 group 0 的明确源端点、通配 Ethernet、可选 COUNT 和明确目的端点。
输入是 14 到 2048 字节的完整十六进制帧，按最低 priority 选择唯一规则，保留原始
字节；无匹配时丢弃，同优先级歧义时拒绝。模拟器不会执行真实软件 worker、协议栈、
ACL/NAT/ND 或其他硬件动作；出现软件规则、持久化 dirty 或恢复隔离时拒绝注入。
既有软件数据面仍通过独立的真实 ring/TAP 流量验收。

持久化仅允许专用 `simulation-state.bin`，启动会核对目录所有者、权限、独占锁和
模拟标记，拒绝接管无标记的已有快照，不覆盖已有 socket。不要将模拟快照或目录
用于正式 `dppd`，两者不提供双向状态导入承诺。

该工具证明已实现控制逻辑在可控端点与报文字节上的行为。AGE、indirect/shared action
和设备 reset 后 reconciliation 尚未实现，不能由模拟器自动补齐。真实 PMD 支持、
ASIC 卸载、CPU 旁路及硬件性能继续按 [M3 验收](m3_acceptance.md) 单独登记。

## 本轮验证结果

2026-10-18，三套构建全部通过 `tests=true`、`werror=true` 的统一核心验收：

| 环境 | 单测 | 正式软件 worker 与 M3 拒绝路径 | 正向功能模拟 |
| --- | --- | --- | --- |
| 135 系统 DPDK 21.11.9 / GCC 11 | 55/55，无跳过或失败 | 全部通过 | 十组检查全部通过，含七种模板各自单向/双向 |
| 135 ASan/UBSan | 55/55，无跳过或失败 | 全部通过 | 同上，未发现 sanitizer 错误 |
| 134 私有 DPDK 21.11.9 / GCC 13.2 | 55/55，无跳过或失败 | 全部通过 | 同上，全部通过 |

三份统一报告、各自的软件回归与模拟报告的 224 文件源码摘要完全一致，并与本地源码
逐项核对。摘要表排序并紧凑 JSON 编码后的 SHA-256 为：

```text
3202b4b8d92dab36d6ebfd1f3e6367e98c2969dc53c2aef4ab5ec0d0c10ed60f
```

另外通过真实手动前端验证 `serve/inject/status/fault/clear-faults`、双向独立计数、
同目录恢复、Ctrl+C 和 SIGTERM 清理；边界夹具验证已有 socket 或无模拟标记的状态
文件不会被接管，路径被替换时保留新对象。未遗留模拟进程或 socket。

本轮核心回归包含正式 worker 单双队列，不把八组完整 TAP 阶段验收重复计入。
既有 TAP 结果继续对应 [M3 基础验收](m3_acceptance.md) 中记录的实现版本；134 新安装
的 TAP PMD 另经私有网络命名空间启停、端口和内存池回收验证。
ASan/UBSan 关闭 LeakSanitizer，mbuf 归还由 worker 记录另行核对。

原始报告保存在两机各自的构建目录，复制件及核对摘要位于本地
`build/native-check/m3-functional-artifacts/`。176 个 C 源码和头文件通过格式检查，
新增中文注释结尾、Python 语法与文档链接检查通过。
