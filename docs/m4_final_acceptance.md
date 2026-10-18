# M4 软件整体验收

M4 软件交付对应 [路线图](roadmap_v3.md) 的 template table、async flow queue、资源配额、
批量操作、aged flow 回收、shared action、背压及 admission control。
这份结论针对当前环境可执行的软件和 API 契约，物理卸载、CPU 旁路和真实设备复位仍须实机单列。

## 交付范围

| 项目 | 正式实现与验收 |
| --- | --- |
| 配额与准入 | 全局/每端口九类资源，整批驱动前预留，准备及残留继续计费，精确凭据和线程竞争 |
| 模板表 | 最多 64 表，固定掩码及动态值/动作分离，实际流引用与逻辑保护，部分销毁失败保留身份 |
| 异步队列 | 最多八队列、深度 256、每批 64，push/pull、精确乱序完成、超时及未知完成隔离 |
| 创建/删除/更新 | 64 条整批期望状态发布，软件 classifier 一次切换，精确旧版本和原队列补偿 |
| 背压与取消 | 受理和完成区分，按规则数计费，完成未领取继续占深度，只撤销尚未激活任务 |
| AGE 回收 | 严格队列、独立 AGE 配额、实例通知防误删、公平预算、有界自动维护及失败退避 |
| 共享动作 | 最多 64 个 typed shared COUNT，模板/逻辑/实际引用分开保护，动态改绑及 AGE 后累计保留 |
| 规模与恢复 | 稳定槽位索引，16384 规则完整安装/删除，既有期望状态重放和已知残留精确清理 |

API 入口见 [flow_service.h](../include/dppd/flow_service.h)、[flow_template.h](../include/dppd/flow_template.h)、
[async_flow.h](../include/dppd/async_flow.h) 和 [flow_shared_count.h](../include/dppd/flow_shared_count.h)。
新增功能为可组合 C API，调用方须独占相应 classifier、配置和队列，不与旧同步控制面的写入资源混用。
既有 daemon 管理协议、快照和业务转发路径保持兼容；本阶段未扩展 M5 北向接口。

## 可重复的联合入口

每套构建先运行以下入口，它会重新编译和运行全部单测、核心/M3 回归及 M4 正式 worker 报文测试：

```sh
python3 scripts/verify_m4.py --build-dir build-modern-private
python3 scripts/verify_m4.py --build-dir build-asan
```

完整标记还要求同源的参考优化构建、现代原生 API 构建、ASan/UBSan 构建、十二步完整 TAP，
以及控制面和异步队列各自的 4096/16384 五轮测量。普通功能报告不会提升为整体完成。
参考门禁使用 CPU 0–3、GCC 11、DPDK 21.11.9、优化严格构建及开启断言：

```sh
taskset -c 0-3 python3 scripts/benchmark_m4.py --build-dir build-perf --rules 4096 \
  --enforce-contract --report-dir build-perf/m4-final-control4096
taskset -c 0-3 python3 scripts/benchmark_m4.py --build-dir build-perf --rules 16384 \
  --enforce-contract --report-dir build-perf/m4-final-control16384
taskset -c 0-3 python3 scripts/benchmark_m4_queue.py --build-dir build-perf --rules 4096 \
  --report-dir build-perf/m4-final-queue4096
taskset -c 0-3 python3 scripts/benchmark_m4_queue.py --build-dir build-perf --rules 16384 \
  --report-dir build-perf/m4-final-queue16384
```

TAP 使用单独配置的本地 DPDK TAP 兼容构建，在私有网络命名空间内验收，不改变宿主管理网卡：

```sh
sudo python3 scripts/verify_m2.py --build-dir build-tap --tap --report-dir build-tap/m4-final-tap
taskset -c 0-3 python3 scripts/verify_m4.py --build-dir build-perf --complete-software \
  --peer-report build-modern-private/m4-acceptance/report.json \
  --peer-report build-asan/m4-acceptance/report.json \
  --tap-report build-tap/m4-final-tap/report.json \
  --benchmark-report build-perf/m4-final-control4096/report.json \
  --benchmark-report build-perf/m4-final-control16384/report.json \
  --benchmark-report build-perf/m4-final-queue4096/report.json \
  --benchmark-report build-perf/m4-final-queue16384/report.json
```

现代或本地私有 DPDK 运行时须保持与构建相同的 SDK、PMD 和动态库搜索路径。
联合报告保存输入文件摘要及完整源码映射，拒绝旧源码、缺少覆盖、跳过测试、缺少完整 TAP、
修改后的契约、少于五轮样本、减少队列规模及未达标数值。

## 本轮同源结果

2026-10-18 / 135 / GCC 11，294 项源码摘要为
`8871ef40683797428b02383fd6f3dc3392ff2d8110599c136735320193dfdbf5`。
系统 21.11.9 优化、私有 24.11.8 优化、21.11.9 ASan/UBSan 三套各 82/82 单测通过，
包含联合报告缺项、旧源码、假成功和最终输入异常的拒绝测试。
三套各 272 包 M4 新路径通过，另有核心四步、M3 六步、共享恢复五组、设备事件四组及模拟 31 组。
本地 TAP 兼容构建另有 82/82 单测及十二步完整回归，通过 IPv4/IPv6、ACL、连接跟踪/NAT、DNAT、
NAT66、动态 ND 和隧道，命名空间及业务进程按各自夹具完成清理。

五轮样本全部保留后取中位数，契约 revision 1 及 SHA256 保持不变。

| 固定门禁 | 本轮结果 | 原门槛 |
| --- | --- | --- |
| 4096 条内存驱动单条安装 | 1225476.630 条/秒 | 至少 70000 条/秒 |
| 16384 条内存驱动单条安装 | 1104808.899 条/秒 | 至少 100000 条/秒 |
| 16384 条单次 API P99 | 4.008 微秒 | 不超过 500 微秒 |
| 1024 条正式软件安装 | 两次规模测量分别 5109.807、5160.208 条/秒 | 至少 4000 条/秒 |
| 正式软件单次 API P99 | 分别 514.543、509.539 微秒 | 不超过 1000 微秒 |
| 4096 条、每批 64 条队列完成 P99 | 4.936462 毫秒 | 不超过 100 毫秒 |
| 进程内已知残留清理 | 分别 371、471 纳秒 | 不超过 50 毫秒 |
| 16384 条期望状态重放 | 56.648569 毫秒 | 不超过 5000 毫秒 |

额外的 16384 条异步批量完整安装、查询及删除通过，完成 P99 中位数 4.879679 毫秒。
原始命令、全部轮次、二进制摘要、源码映射和实际动态库核对在
`build/native-check/m4-final-artifacts/`，汇总为 `summary.json`。
正式联合入口只在三套功能证据、完整 TAP、四组测量和八项门禁全部匹配后输出
`complete_m4_software_validation=true`；`complete_m4_validation=false` 及 `hardware_acceptance=not_run` 保持。

## 明确保留的边界

DPDK 24.11 原生适配实际调用模板及异步公共 API；正向生命周期由包装 PMD 入口的契约测试验证。
21.11 原生模板/异步接口明确返回不支持，实际 net_ring 也明确拒绝原生卸载配置。
真实 worker 报文测试使用正式软件 classifier，不把包装 PMD 命中或软件转发写成 ASIC 命中。

软件路径不模拟语义不同的 AGE、共享 COUNT、硬件 RX QUEUE 分流或 transfer。
共享类型当前只声明 COUNT，RSS/METER 等未实现动作不静默降级。
硬件创建/删除存在逐条可见的过渡，整批账本发布不代表硬件逐包原子切换。

M4 独立交付时没有模板及队列磁盘格式；后续 [M5 首项](m5_flow_state.md) 已增加完整配置与进程重启恢复。
自动端口复位和未知硬件对象的通用清理仍不包含，原生重启须有独立清空证明，不能盲目重复安装。
快照门禁测量已有正式期望状态重放，残留清理门禁测量进程内已知对象，不冒充操作系统或设备重启 RTO。
硬件安装速率、ASIC 容量、实际报文旁路、固件兼容和真实复位仍未验收。
