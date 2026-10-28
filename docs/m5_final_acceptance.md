# M5 软件生产化联合验收

2026-10-28，M5 当前环境软件生产化联合验收完成
四套各 94/94 单测、22 份同源证据、完整 TAP、双目录复现和原八项性能门禁全部通过
物理卸载、CPU 旁路和真实设备复位仍为 not_run，功能限制见下文

## 当前证据与最终条件

| 独立交付 | 已有证据 | 对本轮整体结论的作用 |
| --- | --- | --- |
| M4 配置持久化、正式 daemon 恢复与在线定义事务 | 已完成独立验收，见配置、daemon 和在线配置文档 | 作为必须重新执行的基线 |
| 持久请求回执与幂等重试 | 四套各 89/89 单测及各 56 包正式请求 TAP，324 项同源记录 | 证明该阶段已验收，仍须最终同源回归 |
| 北向 HTTP/JSON、权限与 TLS | 四套各 90/90 单测及正式 HTTP/TLS 四阶段闭环，329 项同源记录 | 证明该阶段已验收，仍须最终同源回归 |
| 运行观测、心跳与 NUMA 布局 | 四套最终同源验收通过 | 94/94 单测及正式进程验证通过 |
| 可复现发布、升级、回滚与中断恢复 | 四套最终同源验收通过 | 94/94 单测及正式进程验证通过 |
| 最终软件联合验收 | 22 份输入同源聚合通过 | 当前环境软件完成，硬件另列 |

历史证据分别保存在 `build/native-check/m5-config-artifacts`、`m5-requests-artifacts` 和
`m5-api-artifacts`。这些目录内有原始报告、测试记录与摘要；它们不是当前源码的最终证据

最终验收由 `scripts/verify_m5_production.py` 组织。四套构建必须来自同一份完整源码：

| 构建目录 | DPDK | Meson buildtype | sanitizer |
| --- | --- | --- | --- |
| `build-perf` | 21.11.9 | debugoptimized | none |
| `build-asan` | 21.11.9 | debug | address,undefined |
| `build-modern-private` | 24.11.8 | debugoptimized | none |
| `build-modern-asan` | 24.11.8 | debug | address,undefined |

全部开启 `tests=true`、`werror=true`、`b_ndebug=false`。每套执行 Meson 注册的全部测试，任何
失败、跳过、Python 内层跳过或 sanitizer 错误都会拒绝通过。仅有几条功能成功报告不满足整体条件

DPDK 所在线程环境目前使用 `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1` 和
`UBSAN_OPTIONS=halt_on_error=1`。因此这里验证 ASan/UBSan 错误，不能称为完整 LSAN 内存泄漏验收
软件包与 mbuf 的资源回收还由各自故障测试和计数断言验证

## 单套正式程序验收

先为目标目录配置正确的编译 SDK 和运行时库，尤其核对 `PKG_CONFIG_PATH`、`LD_LIBRARY_PATH`
与实际加载的 DPDK/PMD 版本。不能用构建目录名称代替实际依赖版本检查

```sh
python3 scripts/verify_m5_production.py \
  --build-dir build-perf --report-dir build-perf/m5-production-acceptance
```

对另外三套目录重复执行，使用各自的 SDK 环境。入口依次执行编译、既有 `verify_m5.py` 基线、
`m5_northbound.py`、`m5_observability.py` 和 `m5_release.py`，其中基线包含 M2 核心、M3 模拟、
M4 worker、全新进程恢复、正式 daemon 与持久请求闭环

每套结果位于 `BUILD/m5-production-acceptance/report.json`，同时保留子目录原始报告和日志。
报告记录完整源码文件摘要、正式及相关测试二进制前后摘要、Meson 注册清单与每条测试结果、日志摘要。
不会把 Meson 原始记录中的主机环境变量复制到联合报告

单套通过时 `success=true`，但 `complete_m5_software_validation=false`。该结果只说明本环境的
功能回归通过，尚未证明四套环境、完整 TAP、性能与独立构建复现都满足要求

## 真实 TAP 与完整数据面回归

完整 M2 回归使用独立的 TAP 验收构建，至少保留一份完整十二步报告：

```sh
sudo env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" python3 scripts/verify_m2.py \
  --build-dir build-tap --tap --report-dir build-tap/m5-full-tap
```

该入口的网络测试自行创建私有命名空间。TAP 需要 root 权限和对应 PMD，不能跳过后仍写成通过。
若环境使用特定 TAP 修复版本，应保留对应构建、运行库和 PMD 身份，不能当作实体网卡测试

持久请求、正式 daemon 与发布升级还需要分别在四套生产构建上执行 TAP：

```sh
sudo env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" unshare --net python3 tests/integration/m5_requests.py \
  --build-dir build-perf --report-dir build-perf/m5-requests-tap --tap
sudo env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" unshare --net python3 tests/integration/m5_daemon.py \
  --build-dir build-perf --report-dir build-perf/m5-daemon-tap --tap
sudo env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" unshare --net python3 tests/integration/m5_release.py \
  --build-dir build-perf --report-dir build-perf/m5-release-tap --tap
```

另三套使用各自构建目录、SDK 环境及 sanitizer 配置。每次都使用独立网络命名空间，不能直接在业务
网络命名空间运行。这些脚本的预期报文分别为请求 56 包、daemon 96 包、发布 72 包；预期数量是
校验条件，本页不据此宣称本轮已实际收发完成。最终报告还核对转发和丢弃数量，不能只看总包数

北向脚本不提供 TAP 模式，证据为每套正式 daemon、CLI 和 HTTP/TLS 网关的四阶段闭环。
规则生效的真实报文证据由对应正式 daemon 和持久请求 TAP 补充

## 可复现构建与冻结性能

在参考 DPDK 21.11.9 环境中，用两个尚不存在的独立目录重新构建正式程序：

```sh
python3 scripts/m5_release.py reproduce --source . \
  --work-dir /tmp/dppd-m5-final-reproduce --report build-perf/m5-reproduce.json
```

`code_source_sha256` 必须与最终源码文件清单一致，两个构建的三个正式程序逐字节相同。
包含文档的 `source.tar` 摘要还须与各套正式发布验收一致。相同二进制重复打包只能证明归档稳定，
不能代替这次独立构建检查。因此发布说明和安装脚本也应在本轮源码与文档冻结后再生成最终证据

四份性能报告继续使用 [原冻结契约](m4_acceptance_contract.json)，参考环境为 DPDK 21.11.9、
GCC 11、CPU 0～3、严格优化构建，每份完整五轮取中位数：

```sh
taskset -c 0-3 python3 scripts/benchmark_m4.py --build-dir build-perf \
  --rules 4096 --repetitions 5 --enforce-contract --report-dir build-perf/m5-final-control4096
taskset -c 0-3 python3 scripts/benchmark_m4.py --build-dir build-perf \
  --rules 16384 --repetitions 5 --enforce-contract --report-dir build-perf/m5-final-control16384
taskset -c 0-3 python3 scripts/benchmark_m4_queue.py --build-dir build-perf \
  --rules 4096 --report-dir build-perf/m5-final-queue4096
taskset -c 0-3 python3 scripts/benchmark_m4_queue.py --build-dir build-perf \
  --rules 16384 --report-dir build-perf/m5-final-queue16384
```

联合验收从五轮原始样本重算中位数，并调用原 `verify_m4.complete_software` 核对全部八项门槛。
不得降低阈值、删失败样本、换小规模、删掉未通过构建，或用旧源码的性能数字拼接新功能报告

## 汇总全部证据

以下 Bash 示例收集四套 profile、十二份生产功能 TAP、一份完整 M2 TAP、一份双目录复现和四份性能报告，
共 22 份输入。路径与上文一致，实际目录可通过相同选项显式替换

```bash
arguments=(--complete-software --report-dir build-perf/m5-final-acceptance
  --tap-report build-tap/m5-full-tap/report.json
  --reproducibility-report build-perf/m5-reproduce.json)
for build in build-perf build-asan build-modern-private build-modern-asan; do
  arguments+=(--profile-report "$build/m5-production-acceptance/report.json"
    --requests-tap-report "$build/m5-requests-tap/report.json"
    --daemon-tap-report "$build/m5-daemon-tap/report.json"
    --release-tap-report "$build/m5-release-tap/report.json")
done
for kind in control4096 control16384 queue4096 queue16384; do
  arguments+=(--benchmark-report "build-perf/m5-final-$kind/report.json")
done
python3 scripts/verify_m5_production.py "${arguments[@]}"
```

最终输出 `build-perf/m5-final-acceptance/report.json`。只有全部证据同源、完整且通过，才设置
`success=true` 和 `complete_m5_software_validation=true`。缺项、重复输入、重复 JSON 键、旧源码、
二进制变动、不同发布包、失败或跳过都拒绝通过，失败时会撤销先前遗留的成功标记

即使软件完成，`complete_m5_validation=false`、`hardware_acceptance=not_run` 仍保留，明确没有
将软件结果扩大为全部物理硬件验证。最终归档还应登记 Git 提交和 push 状态、主机、SDK、命令、
原始报告目录及报告包摘要，便于从结论追溯到实际测量

## 实际运行模式与剩余限制

| 使用方式 | 已有能力 | 不能自动推断的能力 |
| --- | --- | --- |
| M2 独立三层模式，使用 `--ip-routing` 等入口 | IPv4/IPv6 路由、ACL、连接跟踪、NAT、ND 及相关数据面回归 | 没有自动变成 M5 managed 模式中的持久事务或 HTTP 配置入口 |
| M4/M5 managed 模式，使用 `--flow-state` | 端口对软件转发、规则与模板/共享 COUNT、持久请求、恢复、北向 API、运行观测 | 与旧 `--state-path`、路由和硬件恢复配置不能混合启动 |
| 原生硬件与 M3 模拟入口 | 能检查 API 兼容、拒绝不支持能力、模拟 AGE/共享动作/设备故障及恢复边界 | 不能证明 ASIC 安装速率、CPU 旁路、物理热拔插、真实 DPU/网卡复位 |

正式 managed daemon 拒绝原生硬件配置。缺少可见 Mellanox/BlueField 等目标设备时，物理卸载和
真实设备复位仍保持未验收，不能通过安装工具或功能模拟补出硬件证据

持久请求最多保存 **32 个客户端身份、128 条回执**。客户端应长期复用身份并协调递增序号；已回收的
旧键由持久高水位拒绝，不会重新执行。客户端身份满额后新身份返回容量错误，没有管理员清空防重放
水位接口。重启遇到 pending 会保存取消结果，不自动重新执行；COUNT 累计和连接跟踪会话不随配置保存

在线定义事务目前只支持模板与共享 COUNT，队列/配额在线扩容、共享 RSS/METER、定义与规则混合事务
不在本轮范围。HTTP v1 提供经过类型校验的 drop、IPv4/IPv6 ACL 等入口，不提供任意 CLI 字符串执行

北向 API 使用 reader/operator/admin 角色，所有角色能查看全局策略，并不提供多租户数据隔离。
本机管理 socket 属于可信服务用户边界；认证文件和 TLS 证书更新需要重启网关，daemon 升级也不会
自动替换正在运行的 Python 网关。API 审计日志为有界 best-effort，不替代持久请求账本或可靠审计系统

规则 telemetry 是带采样时间的观测值，不是逐包一致性快照。共享 COUNT 返回对象总值，不能把多个
引用规则的同一总值重复相加。worker 心跳能检测转发线程停止前进，主线程或整机卡死仍需要外部探针和
进程监护；显式设置 `--worker-timeout-ms 0` 会关闭超时保护

NUMA 布局只在用户允许的 EAL 核集合内选核，保持现有 worker 数量与队列对应关系。VM 和模拟拓扑
验证的是选择算法与约束，不是实际双路网卡的吞吐收益。跨 socket 端口对仍可能产生远端访问

部署升级保留当前策略和持久请求，禁止用旧快照覆盖新策略。单实例交接会暂停转发；多实例滚动只保证
逐个执行并检查其他实例 ready，不提供跨主机会话复制、流量调度或零丢包保证。当前滚动命令管理同一
主机上的独立实例，跨主机编排另由外部系统完成。格式不兼容时拒绝回滚并保留当前文件

发布包按外部可信 SHA256 校验完整性，**没有发布者数字签名**，摘要本身须经可信渠道获取。
包不捆绑 DPDK、PMD 和系统动态库，需要匹配的运行环境。可复现证明限定于记录的 SDK、编译器、
链接器、架构和构建选项，不承诺跨工具链得到相同二进制

## 最终结果登记

2026-10-28 实际完成记录：

- 功能提交 `801f5a8` 已 push，DPDK 21.11.9 和 24.11.8 优化及 ASan/UBSan 四套各 94/94 单测，无跳过
- 发布内层 25 项、验收器内层 18 项，正式 API 四阶段、观测三阶段和发布八阶段全部通过
- 每套 daemon 96 包、持久请求 56 包、发布 72 包，四套合计 896 包，转发及丢弃分别核对
- 独立 TAP 构建完整十二步 M2 回归通过，含 IPv4/IPv6、ACL、CT/NAT、DNAT、NAT66、ND、隧道
- 两个新目录真实构建，三个正式程序逐字节相同；包含最终文档的源码归档与发布包、Git 提交树核对
- 343 项源码摘要 `834714bb552eb326a988975d6e48620d58516573e5627c45bb5ebbab7bd4968e`
- 证据位于 `build/native-check/m5-production-artifacts/summary.json`、`host-135-core.tar.gz`、`host-135-final.tar.gz`
- 主机 wq7-virtual-machine，Ubuntu 22.04、GCC 11.4.0、Meson 0.61.2，参考性能绑核 0-3，全部五轮样本保留

| 原冻结门禁 | 五轮中位数 | 原门槛 |
| --- | --- | --- |
| 4096 单条内存驱动安装 | 1,199,761.805 条/秒 | ≥70,000 条/秒 |
| 16384 单条内存驱动安装 | 1,103,709.559 条/秒 | ≥100,000 条/秒 |
| 16384 单条控制面 P99 | 4.048 微秒 | ≤500 微秒 |
| 软件 1024 单条安装 | 取较低值 5,160.161 条/秒 | ≥4,000 条/秒 |
| 软件 1024 单条 P99 | 取较高值 0.514770 毫秒 | ≤1 毫秒 |
| 已知进程内残留清理 | 取较高值 471 纳秒 | ≤50 毫秒 |
| 16384 快照重放 | 56.648268 毫秒 | ≤5 秒 |
| 4096 队列批 64 完成 P99 | 5.019815 毫秒 | ≤100 毫秒 |

以上内存驱动、软件路径和旧快照重放按原契约测量，不扩大为 ASIC 或 managed 完整启动性能
最终文档冻结后重新验证发布 core/TAP 和双目录构建，完整 source.tar 摘要同样对齐
实际报告目录为 `BUILD/m5-production-{daemon,requests,release}-tap` 与 `build-perf/m5-production-{control,queue}规模`
最终聚合报告为 `build-perf/m5-final-acceptance/report.json`
