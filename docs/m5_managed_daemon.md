# M5：正式 daemon 的配置恢复与管理协议

本项将 [M4 完整配置持久化](m5_flow_state.md) 接入正式 `dppd`、本机 Unix 管理 socket
和 `dppctl`，提供可直接启动、管理和重启恢复的软件模式。没有指定新参数时仍运行原控制面。
完整 M5、北向网络 API、鉴权和硬件恢复不在本次完成范围内。

## 启动与互斥

```sh
# 初始化端口 0 的配置，逻辑规则容量 64，两个控制队列，每队列深度 64
build-perf/dppd-flow-state init-software /absolute/path/m4.state 0 64 2 64

# EAL 参数仍由部署环境决定，示例只使用两个虚拟端口
build-perf/dppd -l 0-1 --no-huge --no-pci --no-shconf \
  --vdev=net_ring0 --vdev=net_ring1 -- \
  --ports 0,1 --queues 1 --flow-state /absolute/path/m4.state \
  --control-socket /absolute/path/dppd.sock
```

`--queues` 是转发 worker 的 RX/TX 队列数；配置文件中的控制队列数是异步规则任务队列，
二者独立。新模式的规则容量、模板、配额和控制队列以完整配置文件为准，旧 `--rule-capacity`
仅用于原控制面。配置安装端口必须属于本次显式选择的转发端口集合。

新模式在配置和端点检查、取得写锁、完整重放之后才初始化项目网卡和启动 worker。
EAL 的设备探测仍发生在应用参数解析之前，不能把它描述成启动失败完全没有设备访问。
加锁后再次核对实际恢复的配置，不能复用加锁前可能被替换的文件副本。
同一文件只有一个写入者，管理 socket 不得占用配置或其 `.lock` 路径，包括父目录别名。

新模式与 `--state-path`、旧恢复保护、TAP owner 模式和独立路由控制面互斥。
进程不创建旧 control，也不把另一个 classifier 或同步规则写入器接到新实例。
原生配置明确拒绝：正式入口目前没有独立端点身份及旧硬件对象清空证明，不能把重启当作清空。

## 规则管理与持久回执

管理协议提升到 **v28**，客户端与 daemon 必须使用同一构建的本机 C ABI。
版本或固定报文长度不符会拒绝，不支持跨架构、跨版本或网络序列化协商。

```sh
build-perf/dppctl --socket /absolute/path/dppd.sock flow-status
build-perf/dppctl --socket /absolute/path/dppd.sock ready

# 模板键 1，控制队列 0，规则 ID 10，安装端口 0，新建期望版本 0
build-perf/dppctl --socket /absolute/path/dppd.sock \
  flow-create 1 0 apply-drop 10 0 0
# accepted session=S owner=O serial=N slot=K durable=no

# 按上一条返回的四个数字领取结果，EAGAIN 表示仍需使用同一凭据重试
build-perf/dppctl --socket /absolute/path/dppd.sock flow-collect S O N K
# completed status=0 applied=yes durable=yes count=1
# id=10 generation=G

build-perf/dppctl --socket /absolute/path/dppd.sock get 10
build-perf/dppctl --socket /absolute/path/dppd.sock list

# 精确旧版本 G 更新到控制队列 1，仍然单独领取最终回执
build-perf/dppctl --socket /absolute/path/dppd.sock \
  flow-update 1 1 apply-drop 10 0 G

# 删除也返回受理凭据，使用更新后的精确版本
build-perf/dppctl --socket /absolute/path/dppd.sock flow-remove 0 10 NEW_GENERATION
```

`accepted` 只说明完整请求已复制入队，不能当作策略生效或持久化成功。
`flow-collect` 只有实际整批提交并成功落盘后才能输出 `durable=yes`；任务本身失败时 CLI 返回非零。
每次启动生成随机 session，防止旧进程任务号与新任务碰撞；领取后重复领取返回 `ENOENT`，
跨重启凭据返回 `ESTALE`。任务和回执不写入配置文件，重启后通过规则和修订号核对结果。

丢失受理响应不能假定未执行；先查询 ID、版本和 `flow-status`，避免盲目重复提交。
丢失最终领取响应时也应核对期望状态，当前协议没有持久回执重放或跨连接幂等请求键。
未领取任务受已有深度限制，客户端应及时领取；关闭入口后 daemon 会取消未执行任务，
推进并领取已登记任务的终态，不把退出清理写成策略删除。

协议一次支持 1–4 条完整规则的原子创建、更新或精确删除，所有规则使用同一控制队列。
CLI 当前提供单条构造，复用 `apply-drop`、`apply-filter`、`apply-filter-drop`、`acl-set`、`acl6-set`，
模板键和队列必须显式给出，规则必须符合配置中模板的形状和掩码。
初始化工具只提供 Ethernet DROP 模板，其他模板和共享定义仍通过配置 C API 构造，不自动生成。
旧同步写命令在此模式返回 `ENOTSUP`，不会静默修改另一个空账本。

支持原有只读 `ping`、`health/ready`、`stats`、`get/list`、软件 `count` 及 `persistence-status`。
列表使用可靠保存的配置及精确修订号分页；COUNT 只有规则包含计数动作时才可查询。
其他旧控制面专属诊断、路由、硬件复位仍不支持；后续已增加
[在线模板和共享 COUNT 配置事务](m5_online_configuration.md)，协议升级到 v29。
新模式目前使用管理 socket 查询统计及退出汇总，没有接入原控制面的规则 telemetry 和周期日志。
连接跟踪、NAT、ND 依赖独立路由模式，在这个受管理端口对模式下也不启用。

## 故障与退出

实际发布后写盘失败会封闭新写入和成功回执，保留 dirty。主线程停止并等待 worker，
注销所有 QSBR reader 后才允许恢复操作，`health/ready` 同时反映线程与持久化状态。
这不承诺从实际发布到发现磁盘失败之间完全没有报文使用新策略，成功回执始终要求落盘。

```sh
build-perf/dppctl --socket /absolute/path/dppd.sock flow-status
# 修复存储后显式重试保存，成功后继续领取原任务
build-perf/dppctl --socket /absolute/path/dppd.sock persistence-flush
# 仅重试本进程持有凭据的已知补偿，不清除未知实际状态
build-perf/dppctl --socket /absolute/path/dppd.sock flow-retry
```

恢复到 ready 后复用原 classifier 地址重新启动 worker，不热换 backend。
写盘错误没有修复就退出会返回失败，不能把尚未可靠保存的策略报告成成功。
损坏文件、模式冲突、路径冲突或不支持的原生配置不开放管理入口。
启动恢复未完成或清理失败保留组件的失败语义，入口重试关闭并失败退出，不提供硬件自动清理。

设备移除、复位或销毁事件触发收发门禁和失败退出，不在此模式接受主动设备复位。
SIGKILL 后由部署层确认旧进程已经退出，再清除其残留 socket；daemon 不擅自删除已存在入口。
正常退出先关闭管理入口、停止 worker、处理任务，再回收配置实例和设备。

## 验收入口

```sh
# 严格构建、全部单测、既有 M2/M3/M4 和新进程恢复，再验收真实 daemon/CLI
python3 scripts/verify_m5.py --build-dir build-perf

# 无需 root，双 net_ring 正式 daemon 验收
python3 tests/integration/m5_daemon.py --build-dir build-perf --report-dir build-perf/daemon

# 由 root 在独立网络命名空间运行，双 TAP 逐字节验证实际收发
unshare --net python3 tests/integration/m5_daemon.py \
  --build-dir build-perf --report-dir build-perf/daemon-tap --tap
```

TAP 测试在命名空间内关闭 IPv6 自动配置可避免背景报文。固定 48 个报文中 16 个逐字节转发、
32 个规则丢弃，覆盖安装前、安装后、SIGKILL 恢复后、删除后、存储恢复后和失败退出后的恢复。
丢弃场景逐包确认已经进入 worker，避免 TAP 内核发送队列丢包被误计为策略命中。
报文经正式 `dppd` 和实际 `dppctl` 管理；磁盘故障使用另行链接 fsync 包装的测试入口，正式程序无故障开关。
此外覆盖第二写入者、错误消息长度、旧 session、损坏文件、模式/路径冲突、未领取退出及失败退出码。

2026-10-18 同源功能验收：

| 构建 | 单测 | M4 worker | 原 M5 两进程 worker | 正式 daemon 双 TAP |
| --- | --- | --- | --- | --- |
| 系统 DPDK 21.11.9 优化 | 87/87 | 272 包 | 96 包 | 48 包，16 转发、32 丢弃 |
| 私有 DPDK 24.11.8 优化 | 87/87 | 272 包 | 96 包 | 48 包，16 转发、32 丢弃 |
| 21.11.9 ASan/UBSan | 87/87 | 272 包 | 96 包 | 48 包，16 转发、32 丢弃 |
| 私有 24.11.8 ASan/UBSan | 87/87 | 272 包 | 96 包 | 48 包，16 转发、32 丢弃 |

四套无跳过或 sanitizer 错误，各含原有核心四步、M3 六步、共享恢复五组、设备事件四组、
模拟 31 组及 76 项硬件验收工具门禁。ASan/UBSan 对 DPDK 线程禁用 leak 检查，不能宣称通过通用 LSAN。
四套完整 16384 条保存、恢复、逐条精确查询、退出及文件复查分别耗时
4.038、2.664、7.756、7.749 秒，是完整功能测试时间，不是单独恢复 RTO 或硬件指标。

额外的本地 TAP 兼容构建单测 87/87、原有十二步完整回归通过，各程序的实际动态库已核对，
私有 24.11 构建未混用系统 21.11 库。四组性能各保留全部五轮，在固定参考 CPU 0–3 上复测，
八项 M4 revision 1 门禁全部通过，原契约与阈值不变。
16384 条内存驱动单条安装为 1154957.856 条/秒、API P99 为 3.877 微秒；
1024 条正式软件安装为 5152.092 条/秒、API P99 为 524.122 微秒；
4096 条队列完成 P99 为 4.834805 毫秒，原期望快照重放为 58.158797 毫秒。
这些是原 M4 门禁的测量，不代替新配置落盘、完整启动恢复或实际硬件性能。

源码目录共 314 项，摘要 `628d37787a3931a5124fc8742dcfe917bdd305578b035a1ce9f2aeb6bd94b0a8`。
原始日志、源码映射、二进制与动态库检查、性能和最终推送记录保存于
`build/native-check/m5-daemon-artifacts/summary.json` 及同目录证据包。
本项不将模拟或 TAP 结果计为 ASIC 验收，完整 M5 继续标记为未整体验收。
