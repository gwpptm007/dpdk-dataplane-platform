# 跨进程硬件安装保护与离线恢复确认

2026-10-09 完成跨进程恢复的第一阶段：硬件安装前持久化端口线索，异常退出后阻止
未经核对的快照重放，外部清理完成后按精确版本确认，再重新启动。管理协议仍为 v15，
规则快照仍为 v2；新增独立恢复记录格式 v1 和离线工具 `dppd-recovery`。

这一步解决“旧进程已失去句柄，新进程却直接按空 backend 继续安装”的问题。
它不枚举实际残留规则，也不执行设备复位、全端口 flush 或 PMD 专用删除。
自动识别和清理残留对象仍需后续驱动适配。

DPDK 的 flow 句柄由应用在当前进程中保管，flush 操作作用于端口关联的所有规则。
本项目不会把磁盘中的旧指针恢复为有效句柄，也不会把整端口清空当作按所有权清理。
接口边界参考 [DPDK 21.11 Generic flow API](https://doc.dpdk.org/guides-21.11/prog_guide/rte_flow.html)。

## 启用

```bash
sudo ./build/dppd -l 0-2 -n 4 -- \
  --ports 0,1 --queues 2 --promisc \
  --state-path /var/lib/dppd/rules.snapshot \
  --recovery-path /var/lib/dppd/hardware.recovery
```

两个文件的父目录须已存在。恢复保护默认禁用，须显式设置 `--recovery-path`，并同时
设置不同的 `--state-path`。未启用时保留原有行为，不能声称具有崩溃后重放保护。
首次启用前应确认设备已经处于可重放的干净基线；没有记录不证明此前部署没有残留。

恢复文件与规范化后的快照绝对路径绑定，同一部署后续必须继续使用同一恢复文件。
更换快照路径会返回 `EXDEV`，即使恢复记录是 clean 也不会静默改绑。
运行中不要移动、替换、截断或删除恢复文件，也不要把它放在重启后自动清空的目录。
生产目录应由部署账户控制，底层存储须支持可靠文件锁、文件同步和目录同步。

同一文件由 daemon 在整个生命周期持有排他锁，另一实例或离线工具返回 `EBUSY`。
这是使用同一恢复文件的协作约束，不是设备独占声明；它无法阻止其他软件、不同文件路径
或主动禁用保护的实例操作同一设备。文件名和 inode 不可被替换来规避此约束。

## 正常运行与异常退出

| 情况 | 行为 |
|---|---|
| 首次启用 | 创建权限为 0600 的 clean 记录，同步文件和目录后继续启动 |
| 软件安装、画像或 validate 探测 | 不增加硬件待核对端口 |
| 某端口本轮第一次调用驱动 create | 先写入端口和设备身份并 fsync，成功后才允许 create |
| 同端口后续 create | 核对文件仍是持锁的同一对象、设备身份一致，复用已有标记 |
| 记录写入或同步失败 | 本次不调用 create，后续硬件安装保持阻断；已有转发规则不因此自动删除 |
| create 失败或后续补偿成功 | 保守保留本轮标记，不按单次请求推断设备全局干净 |
| 退出时所有本进程 flow 成功删除 | 清除端口记录并同步，规则快照继续保留供重放 |
| 退出删除失败 | 保留句柄和磁盘线索，退出不得宣称已清理 |
| SIGKILL、崩溃或断电，之前尝试过硬件安装 | 后续启动发现待核对端口，拒绝项目的运行时初始化和快照重放 |
| 只执行过软件安装的进程被强制结束 | 恢复记录保持 clean，可以按原规则快照启动 |

启动检查位于 EAL 初始化之后、项目的队列配置、worker 启动和规则重放之前。
因此不承诺在 EAL 设备探测之前阻断。被旧记录拦截时没有管理 socket，也没有可查询的
`health/ready` 或 telemetry 服务，应使用离线工具查看磁盘记录。

同一端口上的标记覆盖整轮进程活动，包括批量事务、补偿重建和启动重放。
标记不会因最后一条业务规则被删除而立即清除；这保证后续失败或无法完整追踪的尝试
仍要求核对。逐包路径没有文件操作，安装耗时仍只计驱动 create，不包含记录同步耗时。

本轮同时修复 backend 退出清理：创建报错但留下非空 handle 的对象，即使未标记
installed，也必须尝试删除；删除失败不能释放仓库并把恢复记录清为 clean。

## 查看线索并恢复

先确认旧进程已结束，然后用 daemon 所属账户或 root 查询。上面的启动示例使用 root，
因此相应查询和确认也使用 sudo：

```bash
sudo ./build/dppd-recovery show /var/lib/dppd/hardware.recovery
```

示例输出：

```text
recovery state=external-reconciliation-required revision=5 ports=2
snapshot=/var/lib/dppd/rules.snapshot
port=0 device=0000:01:00.0 driver=example first-rule=901 first-generation=2
port=1 device=0000:02:00.0 driver=example first-rule=902 first-generation=3
```

`ports` 是本轮可能涉及硬件安装的端口数，**不是实际残留规则数**。
`first-rule/first-generation` 仅为各端口本轮首次尝试的线索，不是完整安装日志。
端口编号可能在重启后变化，应结合设备名称、驱动及部署配置核对实际对象；记录的安装
端口也不能代表 transfer 规则影响的全部端点。工具不会据此自行选择设备并删除规则。

按目标 PMD 和设备支持的方法完成外部核对、清理或复位，确认允许重新安装后，使用
刚刚查看的版本显式确认。下面的命令仅登记这个事实，本身不执行清理或验证硬件状态：

```bash
sudo ./build/dppd-recovery acknowledge-clean \
  /var/lib/dppd/hardware.recovery 5 --external-cleanup-complete
```

成功后版本推进，状态变为 clean；期望规则快照不变。然后用原配置重新启动 daemon，
重新执行完整规则重放。恢复工具不会启动进程，也不会删除异常退出遗留的管理 socket；
部署方应在确认旧进程已结束后处理该 socket。

确认要求文件锁和精确版本同时满足。缺少显式标志、版本带符号或空白、溢出以及多余
参数均被拒绝；版本不符返回 `ESTALE`，当前已是 clean 返回 `EALREADY`。
不要复用旧确认版本，也不要通过删除记录、换路径或去掉启动参数跳过核对。
版本仅属于保留的这个文件，不提供跨文件复制或删除重建后的身份保证。

进程内隔离的 `dppctl reconcile-retry` 仍只删除本进程持有句柄的对象。它无法替代
上述跨进程流程。受保护的进程若全部清理成功并退出，会自动记录 clean，无须离线确认。

## 文件与错误边界

恢复文件固定 7712 字节，显式小端编码，不依赖 C 结构体布局：

| 区域 | 字节数 | 内容 |
|---|---|---|
| 头部 | 32 | magic、版本 1、总长度、uint64 修订号、端口数、CRC32 |
| 快照路径 | 4096 | 规范化绝对路径，NUL 终止，未使用部分为零 |
| 16 个端口槽位 | 每个 224 | 端口、首次规则和版本、128 字节设备名、64 字节驱动名及保留位 |

记录新增端口和清除时推进修订号，重复安装同一端口不推进。修订号耗尽后拒绝写入，
不会回绕。CRC32、完整长度、字符串终止、重复端口、保留位和布局都必须通过校验。
符号链接、多重硬链接、非普通文件以及组或其他账户可访问的权限模式被拒绝。

为保持文件锁始终绑定同一个 inode，更新使用完整原位写入和 fsync，不替换文件名。
中途断电或写入失败可能留下损坏记录；既有空文件也被视为损坏，不能自动初始化。
坏文件返回 `EBADMSG` 并阻止启动，离线确认工具不会强制覆盖它。此时需保留故障证据，
按部署配置核对所有可能涉及的设备，再通过受控离线流程重建文件。
CRC32 不防恶意篡改，文件和目录权限属于必要部署条件。

运行中首次记录错误通过原有规则失败指标返回；后续安装返回 `EUCLEAN`，需要结束
本进程处理故障。该保护不为健康接口新增状态位，`ready=yes` 不能证明所有后续硬件
控制操作都可成功，也不能证明物理设备中不存在其他所有者的规则。

## 验证记录

2026-10-09，Ubuntu 22.04.5 / DPDK 21.11.9 / GCC 11.4 / Meson 0.61.2：

- `-Werror` 完整构建和 26/26 单测通过
- 文件锁、fork 后持锁竞争、16 端口容量、崩溃保留、快照绑定、精确确认版本通过
- fsync 故障阻止驱动 create，文件替换、权限、硬链接、符号链接、截断及校验损坏拒绝通过
- 部分创建失败的 handle 在回滚和退出删除失败后保留，重试成功才允许标记 clean
- ring flow 夹具与真实 TAP 进程完成软件崩溃重放、硬件正常清理、SIGKILL 阻止重放、离线确认后恢复及路径故障验证
- 旧有三组恢复隔离、ring/null 健康与线程故障、四组正式 worker 收发回归通过
- 恢复保护、硬件 backend、事务、控制服务四项单测通过 AddressSanitizer/UBSan

```bash
meson setup build -Dwerror=true
meson compile -C build
meson test -C build --print-errorlogs
python3 tests/integration/recovery_guard.py --build-dir build
sudo python3 tests/integration/recovery_guard.py --build-dir build --tap
python3 tests/integration/recovery_isolation.py --build-dir build
python3 tests/integration/health_readiness.py --build-dir build
python3 tests/integration/software_traffic.py --build-dir build
```

TAP 验证只使用测试自建的临时接口，并在确认进程和接口消失后执行测试记录的离线确认。
ring 夹具的 flow 只存在于独立进程堆内存，不代表物理 PMD 行为。测试检查正常退出时
handle、socket、虚拟接口和 mbuf 回收；没有真实断电、物理设备复位或跨进程实际流表枚举验收。
