# M3 复位与恢复模拟

这项演练验证的是：驱动对象全部失效后，控制面能保留原配置，暂停转发，重新核对端点，
再完整恢复共享对象和规则。它使用独立模拟程序，不会复位真实网卡，也不证明真实
mlx5、BlueField 或其他 PMD 已具备在线恢复能力。整个 M3 的实机验收仍未完成。

## 支持范围

正式后端增加可选的 `simulate_reset` 回调，默认真实适配器将它保持为 `NULL`，调用时
返回 `ENOTSUP`。只有独立模拟程序提供实现。模拟代码只在 `tests=true` 时构建，不链接
进正式 `dppd`；正式管理协议仍为 v26，没有新增 daemon 复位命令。

模拟回调必须遵守“全有或全无”：成功使本实例的全部旧规则与共享动作句柄失效；失败
必须保留全部实际对象、计数和引用。后端只有在回调成功后才清空本地句柄与引用，并使
旧能力缓存失效。此时不能再对旧句柄调用删除，也不会释放后端的仓库、配置或历史指标。
真实硬件复位可能部分成功，不属于这个模拟接口的承诺。

控制面会在任何失效操作前拒绝以下组合：

- 后端没有模拟回调，或启用了 native `before_create` 恢复日志保护
- 已进入恢复隔离，或规则快照为 dirty，尚未完整保存
- 接入了三层路由、连接跟踪或动态 ND，或存在软件后端规则
- 期望规则与实际规则数不一致，或无法取得精确安装记录
- 有规则待恢复但事务编号已耗尽，或重建所需的校验、创建、删除、共享绑定接口缺失

这些限制避免把没有实现的 worker 停止、软件快照重建和三层状态协调当作已经支持。
复位和恢复都由同一个串行控制线程执行，不允许夹在尚未完成的规则事务中间。

## 配置保留与恢复顺序

复位前一次分配并保存完整的内存恢复计划，包含期望规则、共享对象定义、规则账本
版本、共享表修订号，以及当时的端点身份。分配或检查失败就不调用模拟复位回调。

复位成功后，期望配置仍在，实际对象归零；普通修改、计数查询和报文注入保持隔离。
AGE 轮询也不会在此期间删除期望规则。`reset-status` 分别报告 `desired_rules`、
`desired_shared`、`actual_rules`、`actual_shared`，避免把“配置还在”误解为“正在转发”。

显式 `recover` 按以下顺序恢复：

1. 核对内存计划的版本和涉及端点的身份，完成全部规则的本地规划
2. 如果上一次补偿仍有残留，先删除残留规则，再删除失去引用的共享对象
3. 先创建共享 COUNT 对象，再通过正式事务校验、准备和创建全部规则
4. 全部成功才撤销隔离，释放内存恢复计划，允许继续注入报文

恢复保留规则和共享对象原来的精确版本，不把重建误当成用户修改。规则、共享对象与
共享表修订号一起保留，驱动 opaque 重新分配且不复用旧值。私有和共享 COUNT 从零
开始，AGE 从规则重建时重新计时，复位前的闲置时长不带入新对象。

复位和恢复不读取新快照来替换内存计划，也不改写已有快照。启用持久化的演练逐字节
检查文件保持不变；没有配置持久化路径时也能恢复，且不会因此生成快照。恢复后正常
业务修改或 AGE 到期删除仍按原有规则保存，这不属于复位本身的文件操作。

## 端点身份与失败重试

身份核对只针对计划涉及的安装端点、源端点和目的端点，不要求无关端点全部不变。
同一个 ethdev 编号不代表同一个设备；交换域和交换端口、角色及其来源、接口索引、
设备名，以及有效的 controller/PF/function/物理端口信息都参与核对。
可选数值只在相应的 `has_*` 标志有效时比较，未提供的数值和结构体填充字节不充当
身份，发现数组的排列顺序也不影响结果。

端点缺失返回 `ENOENT`，身份或交换域改变返回 `EXDEV`；这些检查在任何新对象创建前
完成。不会因为旧编号又出现，就把另一块设备当作原来的恢复目标。

共享对象或规则重建失败时，内存计划保留，普通转发继续隔离。规则事务逆序补偿已创建
规则，只有引用释放后才清理共享对象；补偿失败还会保留有效句柄和引用。下一次
`recover` 先重试清理，再重新构建，不能叠加出第二套对象。

`last_error` 保存原始恢复失败，`cleanup_error` 单独保存补偿失败；后者非零时返回
`EUCLEAN`。不能以“一部分规则已经创建”作为恢复成功依据。

存在复位计划时，普通 `reconcile-retry` 返回 `EBUSY`。该入口原本用于清理残留后要求
进程重启，不适合替代保留期望配置的专用 `recover`。

## 手动演练

按 [构建说明](build_and_run.md) 开启 `tests=true`、`werror=true`。第一个终端启动
独立模拟服务，使用一个新的专用目录：

```sh
python3 scripts/simulate_m3.py serve --build-dir build --directory /tmp/dppd-reset-demo
```

第二个终端先规划，再使用本次返回的 `repository-generation` 安装双向规则。以下
仅以新目录的空账本版本为 0 为例；复用目录时必须读取实际版本：

```sh
./build/dppctl --socket /tmp/dppd-reset-demo/ctl.sock transfer-plan 9200 0 0 2 uplink-vf bidir count
./build/dppctl --socket /tmp/dppd-reset-demo/ctl.sock transfer-apply 9200 0 0 2 uplink-vf bidir 0 count
python3 scripts/simulate_m3.py inject --io-socket /tmp/dppd-reset-demo/io.sock \
  --source 0 --frame 02000000000202000000000188b5
./build/dppctl --socket /tmp/dppd-reset-demo/ctl.sock count 9200 any
python3 scripts/simulate_m3.py reset --io-socket /tmp/dppd-reset-demo/io.sock
python3 scripts/simulate_m3.py reset-status --io-socket /tmp/dppd-reset-demo/io.sock
python3 scripts/simulate_m3.py recover --io-socket /tmp/dppd-reset-demo/io.sock
./build/dppctl --socket /tmp/dppd-reset-demo/ctl.sock count 9200 any
```

注入后规则 9200 应为 `hits=1 bytes=14`。复位隔离时保留两条 desired 规则，actual 为零，
再次注入应明确失败。恢复成功后规则版本不变，COUNT 为零，之后可以继续注入。
共享对象的创建和查询命令见 [共享 COUNT](m3_shared_count.md)。

可在下一轮复位后调整模拟端点，验证身份不符时拒绝恢复，再还原证据后重试：

```sh
python3 scripts/simulate_m3.py reset --io-socket /tmp/dppd-reset-demo/io.sock
python3 scripts/simulate_m3.py topology --io-socket /tmp/dppd-reset-demo/io.sock 2 missing
python3 scripts/simulate_m3.py recover --io-socket /tmp/dppd-reset-demo/io.sock
python3 scripts/simulate_m3.py topology --io-socket /tmp/dppd-reset-demo/io.sock 2 restore
python3 scripts/simulate_m3.py recover --io-socket /tmp/dppd-reset-demo/io.sock
```

第一次 `recover` 预期返回错误和非零退出码。`topology` 只在存在复位计划时允许执行，
支持 `present`、`missing`、`domain:N`、`identity:N` 和 `restore`，只改变模拟夹具。
多余参数、非法编号、溢出和内嵌 NUL 都会被拒绝，失败输入不改变对象或故障计划。

复位故障使用 `fault reset 1`，表示下一次模拟复位失败一次；也可以继续使用
`shared-create`、`create`、`remove` 和 `shared-destroy` 故障，观察恢复和补偿失败。
每次检查 `reset-status`，清除故障后仍通过 `recover` 重试。

## 验收范围与限制

新增后端契约单测验证默认真实回调为空、native guard、回调失败保留状态、成功不调用
旧 remove/destroy、引用归零、缓存失效，以及同 ID/版本重建后旧句柄仍失效。
该单测已在隔离目录通过严格编译与 ASan/UBSan。

自动演练入口为：

```sh
python3 scripts/simulate_m3.py demo --build-dir build
```

本轮增加七组复位恢复场景，完整演练为 31 组。
报告仍标记 `simulation=true`、`hardware_acceptance=not_run`、
`complete_m3_validation=false`。

## 本轮验收结果

2026-10-18，三套最终构建均通过 `tests=true`、`werror=true` 的统一核心验收：

| 环境 | 全量单测 | 核心软件回归 | 功能模拟 |
| --- | --- | --- | --- |
| 135 系统 DPDK 21.11.9 / GCC 11 | 61/61 | 四步通过 | 31 组通过 |
| 135 ASan/UBSan | 61/61 | 四步通过 | 31 组通过，无 sanitizer 错误 |
| 134 私有 DPDK 21.11.9 / GCC 13.2 | 61/61 | 四步通过 | 31 组通过 |

没有失败、跳过或超时。三份统一报告、各自的软件回归和模拟报告中的 237 项源码摘要
逐项一致，并与本地源码核对。摘要表排序、紧凑 JSON 编码后的 SHA-256 为：

```text
ccaed3dc485480205a13c7b2b3554ddb84a01f6afac29f22182190f1372e0d33
```

额外用真实 Python 命令前端验证 `serve/reset/recover/reset-status/topology/fault/inject/status`，
包括共享 CLI 引用、身份拒绝、COUNT 归零、AGE 新起点、复位故障原状和 SIGTERM 清理。
237 项源码、189 个 C 源码和头文件通过语法、格式、行宽、中文注释结尾与文档链接检查。
报告复制件在 `build/native-check/m3-reset-artifacts/`，构建产物不提交到源码。
两机验收后没有测试进程或模拟 socket 残留。

本轮没有重复登记历史八组完整 TAP，也没有将模拟记录扩大为真实硬件或完整 M3 验收。
ASan/UBSan 关闭 LeakSanitizer；正式 worker 的 mbuf 归还另由内存池记录核对。

尚未实现真实设备 reset、运行中硬件重枚举、端点移除后的在线重建、软件或三层状态
联合恢复，也不保证中断期间无丢包。真实 PMD 的句柄失效范围、资源清理和固件行为
需要独立实机验证，不能直接套用模拟回调的原子契约。

实现入口见 [控制恢复](../lib/control/reset.c)、[后端门禁与失效](../lib/offload/rte_flow_backend.c)、
[模拟驱动](../tests/integration/transfer_sim.c) 和 [演练脚本](../scripts/simulate_m3.py)。
