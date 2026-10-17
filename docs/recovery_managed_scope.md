# 受控本地 TAP 恢复闭环

`dppd-scope` 已把业务异常退出后的残留核验、精确删除、逐条结果落盘、失败续接和
快照重启恢复连通。支持范围是启动器在自己的私有网络命名空间内新建的两个本地 TAP，
硬件规则使用原生标识补丁的 DROP/QUEUE；普通离线命令和其他设备仍不获得自动删除权限。

## 信任边界

宿主机管理员、root 启动器、服务专用账户及同一次构建的程序和动态库属于信任边界。
管理员可以进入网络命名空间并改变 TC，这种管理行为不在隔离承诺内。服务账户不能
同时运行不受信任的程序，也不能把恢复存储目录开放给其他用户。

正式启动器只在每次新建的、未发布名称的网络命名空间内创建自己的两个持久 TAP。
它不迁移实体网卡，不接管已有网络命名空间，不通过扫描 `/proc` 猜测全体写入者。
普通外部进程不能凭自己的 `CAP_NET_ADMIN` 进入这个由初始用户命名空间拥有的网络空间；
宿主机 `CAP_SYS_ADMIN` 属于上述管理员信任范围。

子进程切换为非 root 的固定 UID/GID，清空附加组并设置 `NoNewPrivs`。监护的全部
capability 集为空；业务和恢复仅保留 `CAP_NET_ADMIN`，其他能力从 bounding set 中删除。
授权通道固定继承为 fd 3，exec 前关闭其他继承描述符并拒绝 socket 类型的标准输入输出，
避免旧命名空间的网络 socket 绕过目标空间隔离；该入口要求内核支持 `close_range`。
在 exec 前安装第一层 seccomp 过滤器，禁止派生独立进程、切换命名空间、ptrace 和
io_uring；正式入口核验身份后再禁止后续 exec 和修改 dumpable、父进程死亡信号等设置。
允许的 clone 只用于同一线程组的工作线程，因此业务 pidfd 退出不能遗留独立写入子进程。
未知架构拒绝受控模式，目前过滤器支持 x86-64 和小端 AArch64。

seccomp 是组合边界的一部分，不能单独替代身份、capability 和网络命名空间控制。
具体系统调用语义见[内核 seccomp 文档](https://docs.kernel.org/userspace-api/seccomp_filter.html)、
[capability 手册](https://man7.org/linux/man-pages/man7/capabilities.7.html)和
[父进程死亡信号手册](https://man7.org/linux/man-pages/man2/PR_SET_PDEATHSIG.2const.html)。

## 实时授权

启动器以匿名 Unix `SOCK_SEQPACKET` socketpair 给每个角色单独发放通道，入口只读取
继承的 `DPPD_SCOPE_FD`。环境字符串不是授权：入口必须确认内核对端为 root 父进程，
核对自身 UID/GID、全部 capability 集及当前网络命名空间，然后执行新随机数握手。
回复绑定随机作用域编号、命名空间设备号/inode、父子 PID 和用户身份。每次往返共用
1000 ms 单调时钟预算，错误在客户端锁存；关闭后不能回到未受控状态。

父端通过 `SO_PASSCRED` 核对真实发送 PID、UID 和 GID，拒绝错误角色、多余描述符和
消息截断。子进程不能用数据字段冒充另一个角色。作用域编号只保存在活进程中，不从
旧文件补造。启动器死亡将使子进程收到 SIGKILL；旧监护文件只表示会话中断。

未指定 `DPPD_SCOPE_FD` 的普通入口保持既有行为，仍不能执行受控清理。当前只读计划
仍保留 `writer-exclusion=no executable=no`，不会根据一次握手改写为可执行计划。

## 启动和状态管理

准备一个真实的绝对路径目录，所有者为服务账户 UID/GID，权限 0700。目录路径最多
60 字节，以保留 DPDK Unix socket 的路径空间；服务账户必须能够读取同一构建的程序、
动态库和 DPDK PMD。示例中的 1000 是服务 UID/GID，部署时替换为实际专用账户。

```sh
install -d -m 0700 -o 1000 -g 1000 /var/lib/dppd-tap
sudo ./build-native/dppd-scope run /var/lib/dppd-tap 1000 1000 --native
./build-native/dppctl --socket /var/lib/dppd-tap/control health
```

使用私有 DPDK 安装前缀时，在可信 root 启动环境中指定 `LD_LIBRARY_PATH`，并确保服务
账户可访问该路径。启动器清除预加载和其他继承环境，只保留这项可信库配置，另外设置
私有 `XDG_RUNTIME_DIR`，避免不同用户争用 `/tmp/dpdk`。不支持原生标识的系统构建可以
省略 `--native` 使用软件规则；普通硬件规则崩溃后会因缺少标识明确拒绝清理。

启动器固定使用 `--no-pci`、两个自建 TAP、一个工作线程和单队列，不接受外部设备参数。
目录内 `scope.lock` 为 root 所有的 0600 文件；启动器保持排他锁并持续核对目录和锁文件
身份。`snapshot` 保存业务快照，`recovery` 保存恢复保护，`control` 是管理 socket，
每一轮监护写入带随机作用域编号和轮次的新 `session-*` 文件。

1. 启动器锁定私有恢复目录并拒绝已有待核对记录，再创建专用命名空间与 TAP
2. 先启动无能力的监护，确认会话就绪，再启动唯一有网络写权限的业务
3. 用 pidfd 确认业务整个线程组退出；监护必须持续有效，暂停或中断都不能启动删除
4. 同一作用域仅授权一个恢复进程，保持恢复文件锁和监护租约，按原生标识、内容、范围
   与接口连续性核验后精确删除；证据不齐、重复标识、共享动作、接口变化等都拒绝
5. 每个删除结果持久落盘，处理删除后、结果落盘前恢复进程崩溃的窗口；持续持有作用域
   证明与有效监护时允许续接，丢失这些证据则保持待人工核对
6. 全部记录完成且核对后才能清除恢复保护；关闭旧监护，以新会话启动业务重放原快照

事件循环同时处理授权请求与进程退出，不在等待业务或恢复时阻塞监护授权。先关闭通道
不算退出，必须等内核进程退出证据；业务与恢复的网络写权限不会同时有效。监护请求
超过 1000 ms 不更新时，启动器拒绝其他写入角色，执行器还必须自己得到监护新鲜回复。
失败清理最多启动三次，只重试可恢复 I/O 错误或清理进程被 SIGKILL 的情况。一次启动器
运行最多自动重启业务三次，避免持久启动故障形成无限重启。SIGINT/SIGTERM 先给业务
正常清理时间；失败退出会终止自己的角色并保留恢复保护，不用清空文件掩盖失败。

原生标识依赖独立 TAP 补丁。系统驱动缺少标识时必须验证为明确拒绝，不能用同坐标或
内容相似替代原生标识；软件规则不产生硬件残留时应走无残留重启路径。

## 精确删除和独立日志

受控入口为 `dppd-recovery clean-managed PATH REVISION SESSION_PATH`，只能由活着的
root 启动器交付匿名授权通道。普通 shell 运行此命令会在打开恢复文件前拒绝。现有
`plan`、`plan-monitored` 与 `handoff-check` 保持只读及 `executable=no`。

执行器取得原 revision 的文件锁和监护租约，把监护登记的全部端口纳入查询，包括尚未
安装规则的端口。任何缺失证据、不完整观察、重复标识、内容或范围变化、共享动作及
未关联规则都拒绝。查询窗口仍拒绝 TC 修改和接口身份变化，但允许最后一个业务 TAP
描述符关闭产生的普通载波通知；监护持续覆盖删除、改名和身份冲突，接收丢包仍会失效。

删除请求只使用精确 ifindex、parent、chain、priority、protocol、handle 和 flower kind
发送 `RTM_DELTFILTER`，发送与 ACK 共用 2000 ms 绝对预算，不 flush qdisc。删除前再次
核对作用域、监护、原记录及日志，删除后重新列举全部对象确认标识消失。

每次事故使用 `recovery.cleanup.REVISION` 独立日志。原恢复记录在全部删除期间保持
原 inode 和原 revision，现有 IPC v2 精确版本租约不用放宽或增加进度提交消息。日志
固定 4224 字节，以小端编码、CRC32 及全零保留区域检查损坏，绑定恢复文件设备号、
inode、revision、原 CRC、作用域编号、监护会话和尝试 ID。CRC 不是权限证明。

日志先完整写入 0600 临时文件并同步，再原子替换并同步目录；首次创建不覆盖已有文件。
文件必须保持单链接和正确所有者、权限、inode、长度及完整内容。状态依次记录待处理、
删除中、已删除或删除失败，保存具体错误；已删除不能回退，写盘失败在本进程锁存。
每个结果持久化完成后才继续下一条，全部为已删除且最终查询没有残留后，按原 revision
确认恢复保护为干净状态，随后才允许新监护与快照重放。

删除成功但 ACK 不确定、结果未保存，或创建成功但结果仍是 intent 时，重新查询原生
标识与预期内容。在同一活作用域和原监护仍有效的前提下，标识已消失可以保存已删除；
标识仍在必须继续完整核验再删除。原业务 `guard_removed` 不接受 intent 的规则不变。
启动器或监护丢失后不能靠日志重新造出授权，新启动器直接拒绝任何待核对的旧保护文件。
旧事故日志和旧会话保留供核对，不在启动时覆盖或删除。

## 验收

2026-10-17，在 Ubuntu 22.04.5、Linux 6.8.0-138、GCC 11.4、Meson 0.61.2 上验证系统
DPDK 21.11.9 与原生标识补丁 DPDK 21.11.9 两套构建：

| 检查 | 结果 |
| --- | --- |
| 两套 `-Werror` 构建及全量单测 | 各 37/37 |
| 正式启动器完整恢复场景 | 原生构建 20/20，系统构建 2/2 |
| 降权、角色身份和启动器死亡回归 | 两套各 13/13 |
| 原有只读租约和交接回归 | 两套各 10/10 |
| 日志与查询窗口 AddressSanitizer/UBSan | 2/2 |
| 本地与 Linux 验收源码哈希、格式、中文注释结尾和文档链接 | 一致且通过 |

| 完成条件 | 代码及验证入口 |
| --- | --- |
| 写入权限和接口连续性 | [作用域限制](../lib/control/recovery_scope.c)、[正式启动器](../app/dppd_scope/main.c)、[权限集成测试](../tests/integration/recovery_scope.py) |
| 完整残留核验和精确删除 | [执行器](../lib/control/recovery_cleanup.c)、[单条 netlink 删除](../lib/control/recovery_delete.c) |
| 逐条结果持久化与损坏拒绝 | [独立日志](../lib/control/cleanup_journal.c)、[I/O 故障单测](../tests/unit/test_cleanup_journal.c) |
| 失败续接、intent 窗口和重启重放 | [真实进程验收](../tests/integration/managed_recovery.py)、[清理暂停点](../tests/integration/cleanup_window.c)、[创建结果暂停点](../tests/integration/scope_business_window.c) |
| 条件不满足拒绝且不误删 | 同一真实进程验收核对内核残留、保护文件、快照和角色 pidfd |

两套 DPDK 21.11.9 构建使用 `-Werror`。进程限制单测检查线程允许而独立
进程禁止、clone3 回退、禁止再次 exec 和命名空间切换，以及通道身份和失败锁存。
日志单测覆盖短写、文件/目录同步失败、替换失败、结果不可回退、跨事故绑定、链接与
内容篡改，查询窗口和日志单测通过 AddressSanitizer/UBSan。基础权限和原有交接回归
继续分别运行，完整主链使用以下正式启动器集成测试。

```sh
sudo python3 tests/integration/recovery_scope.py --build-dir build
sudo python3 tests/integration/recovery_scope.py --build-dir build-native
sudo python3 tests/integration/managed_recovery.py --build-dir build
sudo python3 tests/integration/managed_recovery.py --build-dir build-native --native
```

测试专用启动器在新的网络命名空间中运行三个正式入口，分别检查正常授权、明确拒绝、
命名空间错配和角色错配；另以 pidfd 验证启动器被杀后监护退出且旧会话报告中断。
业务在仅保留 `CAP_NET_ADMIN` 时启动真实 TAP 和 DPDK 工作线程，正常退出后回收管理
socket。完整恢复测试核对快照字节、规则 generation、实际内核过滤器和每条持久结果，
并保留测试命名空间描述符，在启动器退出后继续核对没有误删。测试关闭该描述符后
回收自己的命名空间，宿主机接口集合必须保持不变。

验收覆盖：

- 正式启动器必须用 pidfd 管理角色，不在等待子进程退出时阻塞授权事件循环；旧写入者
  退出前不能启动恢复写入者，恢复退出前不能启动新业务
- 固定 revision 的原记录与独立日志组合，跨进程续接不放宽普通租约的检查
- 原有 `dppd_recovery_guard_removed` 不接受尚处于 intent 的记录。受控恢复必须另行处理
  “内核已创建而创建结果未落盘”的窗口，凭原生标识、预期内容和持续作用域核验后才能
  持久保存删除结果，不能改变普通业务无 handle 时的保守规则
- 删除只发送精确接口、parent、chain、priority、protocol、handle 和 kind 的
  `RTM_DELTFILTER`，不 flush qdisc、不批量清空接口，也不把未关联对象当作自己的规则
- 删除后、结果落盘前恢复进程死亡时，在同一作用域且原监护持续有效的前提下，可以
  重新核对原生标识缺失并保存已删除；旧会话中断、身份变化或观察不完整时仍必须拒绝
- 业务 SIGKILL 后 DROP 与带匹配条件的 QUEUE 清理及快照重放，软件无残留重启
- 清理进程在删除前、删除后、逐条结果提交后、最终保护解除后被杀，删除或保存失败
- 真正创建成功而结果未落盘的 intent 残留，仅重放已提交快照
- 跨端口重复标识、无安装记录端口上的副本、共享动作、未知对象、内容及范围变化
- 接口改名、监护死亡或暂停、启动器死亡、日志损坏和系统驱动缺少标识时明确拒绝

该能力不承诺实体设备复位、远端镜像、其他 PMD、整机重启后的原接口连续性、任意旧
命名空间领养或跨主机恢复。这些场景需要自己的所有权与生命周期证明，仍走外部核对。
