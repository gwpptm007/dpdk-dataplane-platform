# 跨规则原子更新验证记录

日期：2026-09-26。验证对象为当前未提交工作区代码（基于 `c9979f0`），包含 management v7、
跨规则更新、在线隔离和新增测试。本文不代表物理 NIC/SmartNIC 验收。

## 环境与构建

- SSH 测试机：`wq7`，Ubuntu 22.04.5 LTS，kernel `6.8.0-138-generic`。
- GCC 11.4.0、Meson 0.61.2、Ninja 1.10.1、DPDK 21.11.9。
- 独立目录：`/tmp/dppd-batch-validation.3tWENj`，未覆盖测试机原项目。
- 普通用户运行，未调整物理网卡绑定、地址、路由或大页设置。

```bash
meson setup build -Dtests=true -Dwerror=true
meson compile -C build
meson test -C build --print-errorlogs
python3 tests/integration/batch_update.py --build-dir build
python3 tests/integration/recovery_isolation.py --build-dir build
python3 tests/integration/software_traffic.py --build-dir build
```

`-Werror` 全量构建和链接通过，**15/15 单元测试通过**。新增的 `atomic batch update`
覆盖新建失败、旧版本删除失败、撤新/恢复失败后的隔离、原 generation 保留、混合
backend 补偿、无变化内容更新、输入与容量边界，以及 snapshot 故障。管理协议和 CLI
测试验证 v7 请求、精确 generation、保留位及参数拒绝。

首次运行发现新增测试在持续磁盘故障下仍要求 `dppd_control_fini()` 成功。
已修正为断言 `/dev/null/rules.bin` 保存返回 `-ENOTDIR`，并确认退出后对象和路径已释放；
没有改变生产代码的故障返回行为。当时的完整单测结果为 14 成功、0 失败；加入纯软件整批发布测试后为 15 成功、0 失败。

## 纯软件整批发布

`software batch snapshot publication` 对两条和四条规则分别检查：

- 在控制服务入口注入快照副本、每个新计数器、退役记录分配失败，全部返回 ENOMEM，
  active 指针、仓库版本和旧计数保持不变；后续重试成功。
- 新版本 COUNT 归零，未更新规则的 COUNT 保留；实际匹配后新胜出规则正确计数。
- 注册读者尚未报告安全点时旧表留在退役链表；注销后管理查询触发安全回收。
- 一个或两个并发读者持续分类解析后的 UDP 测试报文，两条、四条规则各更新 2000 次。
  每对规则交换优先级和 MARK，完整新旧表均应命中 MARK=7，部分替换会出现 MARK=9；
  测试始终命中、DROP 且 MARK=7，每个读者至少完成 2000 次分类。

`atomic batch update` 同时覆盖纯软件保存成功、保存失败后的 dirty 状态，以及 PREFER
最终成为混合计划时临时空间不足，确认没有创建或删除实际规则。

新增测试还在独立 `build-software-asan` 中使用 `-Db_sanitize=address,undefined` 构建，
以 `ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1` 执行通过，未报告内存或
未定义行为错误。测试专用 `--wrap=calloc` 不链接进生产 daemon。

## net_ring 进程间结果

脚本使用两个 `net_ring` vdev、lcore 0/1、`--no-huge --no-pci -m 64`、每端口一个队列，
每个实例使用独立 file-prefix、socket 和 snapshot 路径。

| 场景 | 结果 |
|---|---|
| 2 条规则，rule-capacity=4 | 创建版本 1/2，更新为 3/4，共享事务；旧请求 ESTALE；相同内容再更新为 5/6；删除后全局版本 8、规则数 0 |
| 4 条规则，rule-capacity=8 | 创建版本 1–4，更新为 5–8，共享事务；旧请求 ESTALE；相同内容再更新为 9–12；删除后全局版本 16、规则数 0 |
| 2 条规则，rule-capacity=2 | 满表更新为 3/4；相同内容再更新为 5/6；删除后全局版本 8、规则数 0 |
| 4 条规则，rule-capacity=4 | 满表更新为 5–8；相同内容再更新为 9–12；删除后全局版本 16、规则数 0 |

所有成功更新均确认 backend=software、priority=20；各实例最终持久化状态均为
`enabled=yes dirty=no`，daemon 正常退出且 socket 清理成功。集成脚本的临时目录已清理；
独立构建目录保留以便复查，单测日志位于其 `build/meson-logs/testlog.txt`。

## 纯软件实际收发验证

`software_traffic.py` 在独立进程中建立两对 net_ring RX/TX 队列，启动正式 runtime 和
EAL worker，通过真实 mbuf 解析与收发路径验证批量更新。工作线程处理报文时，主线程
交换两条或四条规则匹配的 UDP 端口；无论使用完整旧表或新表，8000/9000 都应丢弃，
10000 应转发。任何目标报文漏到出口均直接失败。

| 规则数 | 成功更新次数 | 实际接收 | 正常转发 | 策略丢弃 | 分配失败注入点 |
|---|---:|---:|---:|---:|---:|
| 2 | 1001 | 24120 | 8040 | 16080 | 4 |
| 4 | 1001 | 48336 | 16112 | 32224 | 6 |

1000 次更新与队列收包交错进行，最后一次成功更新用于验证故障解除后的重试和新计数。
所有分配失败均保留旧版本并继续准确累计 COUNT；重试成功后各新规则 COUNT 从零开始，
固定一轮报文后每条命中 8 次、512 字节。接收、转发、丢弃和字节统计一致，无畸形报文
或发送丢弃。工作线程退出后全部队列清空，mbuf 池空闲对象数恢复到开始时的数量。

普通 `-Werror` 构建与 AddressSanitizer/UBSan 构建各通过这两组测试，未报告内存或
未定义行为错误；15/15 单测回归通过。测试没有配置物理网卡，也不测量性能上限。
日志为独立构建目录下的 `build/software-traffic.log` 和
`build-software-asan/software-traffic.log`。

## 在线恢复隔离：进程级验证

新增仅供测试的共享库 `libdppd_flow_faults.so`，在 `-Dtests=true` 时构建，不链接到
dppd、不安装。`recovery_isolation.py` 只为它启动的 daemon 设置 `LD_PRELOAD`，在
DPDK flow API 边界模拟 handle 和 create/destroy 故障；真实 daemon、管理 socket、
worker 和持久化逻辑照常运行。测试库同样通过 `-Werror` 编译。

| 故障路径 | 隔离时硬件 backend 残留对象数 | 结果 |
|---|---|---|
| 第二条新版本创建失败，撤销第一条新版本时删除失败 | 3（两个旧版本、一个新版本） | 通过 |
| 第二条旧版本删除失败，撤销新版本后恢复第一条旧版本失败 | 1（第二条旧版本） | 通过 |

两组测试均检查了以下完整过程：

- 等待主循环在 `dppd_runtime_wait()` 完成后输出 worker 已停止标记；
- `ping/list/get/persistence-flush/update-drop-batch` 全部返回 EUCLEAN；
- 首次清理重试再次注入删除失败，保留一个对象，状态仍为 reconciliation-required；
- 第二次清理重试成功，返回 restart-required 和零残留；
- daemon 自动以退出码 1 结束，socket 被删除，测试库报告零存活 handle；
- 隔离与重试期间 snapshot 字节完全不变；重启后恢复规则 700/701、版本 1/2、
  priority=0，恢复状态为 ready，随后正常退出。

脚本的全部临时进程和目录均已清理。该验证补齐了在线隔离分支的进程级证据，
不宣称 PMD 或物理设备具备相同故障行为。

## 证据边界

- net_ring 验证真实 daemon/CLI、软件规则生命周期和 snapshot 写路径，没有注入报文，
  不证明逐包切换、吞吐或硬件 offload。
- 独立 `software_traffic` 测试补充了虚拟 PMD 的实际收发、DROP、COUNT 和正常转发验证，
  控制更新通过 C API 调用，不通过管理 socket；不代表物理网卡或多进程数据面验收。
- 在线补偿与重试故障由独立 flow API 测试库注入；真实 PMD 故障、设备复位及残留
  硬件规则的行为仍需具体设备验收。
- 本次没有验证非空 software snapshot 的重启重放，不能据此扩大既有恢复能力承诺。
