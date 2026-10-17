# M3 当前可落地范围与验收

本轮按 [路线图](roadmap_v3.md) 推进 SmartNIC/DPU transfer，先核查硬件，再完成当前
虚拟环境可验证的控制基础。整个 M3 尚未完成，不能用模拟结果代替实机卸载验收。

## 已交付范围

| 路线图项目 | 本轮实现与证据 | 尚缺证据或功能 |
| --- | --- | --- |
| 环境与设备 | 标准库只读盘点、权限/缺失保留、管理口识别 | 当前环境没有已验证的 mlx5/BlueField 实机 |
| representor 角色 | mlx5 严格名称解析、netlink/sysfs/PCI 交叉核对，未知保留来源与错误 | 实际 PF/VF/SF 与跨控制器设备验证 |
| 同域与场景图 | 七种单向/双向模板、同域/别名/父子证据检查 | PMD 对具体匹配和动作组合的支持 |
| 正式控制入口 | v24 的 plan/apply、精确全局版本、整批创建/补偿、隔离和持久化复用 | 无硬件逐包原子承诺；运行中拓扑重发现待实现 |
| counter | transfer 方向契约、COUNT 64 位值及有效位检查 | 实机线缆报文与计数、CPU worker 对照 |
| age/indirect | 明确登记为未实现 | 规则 IR、管理、共享对象生命周期与完整验收 |
| 移除/复位 | 沿用原有设备异常停止和失败退出机制 | 真实硬件 reset/端点移除后的 reconciliation |

角色与图的使用方法见 [M3 控制闭环](m3_transfer.md)，适配器修复见
[驱动契约](m3_flow_contract.md)，实际设备观察见 [环境记录](m3_environment_20261018.md)。
纯名称解析只证明语法；正式控制面使用经过发现核对的拓扑，测试夹具仅构造模拟证据。

## 验收分层

- 单测用可控驱动接口覆盖七种模板的正向安装、COUNT、验证/创建失败、补偿失败及隔离
- 拓扑单测覆盖严格名称、权限/缺失、命名空间不一致、接口变化和文件描述符回收
- 正式 daemon/CLI 在 net_ring 上覆盖未知角色和交换域的明确拒绝、无副作用、版本与重启
- M2 回归覆盖现有软件 worker、双栈 ACL/NAT/ND/隧道及真实内核 TAP 的收发
- 硬件层必须另有设备、固件、拓扑、线缆流量、PMD 计数和 CPU 旁路记录，当前未执行

这里的 TAP 是实际内核虚拟接口，模拟驱动入口是单测夹具，两者均不是 mlx5 硬件。
不安装假的 SmartNIC backend 到生产路径，也不把 transfer 规则静默降级到软件执行。

## 重复运行

先按 [构建说明](build_and_run.md) 准备 Linux DPDK，开启 `tests=true`、`werror=true`。

```sh
python3 scripts/verify_m3.py --build-dir build
sudo python3 scripts/verify_m3.py --build-dir build --tap
sudo env LD_LIBRARY_PATH=/path/to/native/lib:/path/to/native/pmds \
  python3 scripts/verify_m3.py --build-dir build-native --tap
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 scripts/verify_m3.py --build-dir build-asan
```

统一入口只读盘点、调用 M2 回归入口执行全量单测与软件验收，最后运行 M3 正式进程
拒绝场景。`--tap` 只在脚本新建的私有网络命名空间使用自己的 TAP，不修改实体设备。
没有 `--tap` 只执行核心软件回归，报告不会标记完整 M2 TAP 验收通过。

构建目录的 `m3-acceptance/` 保存日志和 JSON；`success=true` 只表示所列虚拟范围通过。
`hardware_acceptance=not_run` 和 `complete_m3_validation=false` 始终保留，M3 实机验收
不会由这条命令自动补成成功。失败、超时、单测跳过、旧子报告或源码中途变化均阻断成功。
软件回归由子入口按每一步限时并清理同组进程，M3 外层不再叠加总期限，避免提前
杀掉负责清理的父进程而留下内层测试或 daemon。其他 M3 步骤仍有各自的超时保护。
ASan/UBSan 本轮关闭 LeakSanitizer，实际 mbuf 归还由 worker 的内存池记录另行核对。

## 本轮结果

2026-10-18，三套最终构建完成同版本验收，报告的 222 个源码文件逐项一致，并与
实现提交 `03dc2e9` 的源码相同。两套完整验收分别限制在 CPU 0–3 和 4–7 并行执行。

| 构建 | 全量单测 | 软件回归 | M3 正式进程 |
| --- | --- | --- | --- |
| 系统 DPDK 21.11.9 | 55/55，无失败、跳过或超时 | 十二步通过，含八组 TAP | v24、全部模板拒绝、无副作用及重启通过 |
| 本地 TAP 适配 DPDK 21.11.9 | 55/55，无失败、跳过或超时 | 十二步通过，含八组 TAP | 同上，全部通过 |
| ASan/UBSan DPDK 21.11.9 | 55/55，无失败、跳过或超时 | 四步核心回归通过，含单双队列 worker | 同上，全部通过 |

三份 M3 报告及各自的软件回归报告均成功，源码摘要表按键排序、紧凑 JSON 编码后的
SHA-256 为：

```text
e2c2278f95768aa7e0bcb396c72b6d6bbbd08ff33fccedbba23488d5548d66e7
```

每套报告都保留 `hardware_acceptance=not_run`、`complete_m3_validation=false`。
sanitizer 日志没有报告错误；本轮关闭 LeakSanitizer，worker 记录的 mbuf 全部归还。
测试后没有遗留 dppd、监护或受控作用域进程及命名网络空间，宿主接口恢复至原有
lo、ens192、ens33、ens34。175 个 C 源码和头文件通过格式检查，新增中文注释结尾、
Python 语法和文档链接检查通过。

Linux 测试目录的三套 `m3-acceptance/` 保留原始报告与逐步日志，本地
`build/native-check/m3-final-artifacts/` 保存复制件及核对摘要，均为构建产物，不提交到源码。

验收入口另以 14 组临时夹具检查旧报告、其他源码目录、失败、超时、源码变化、
TAP 标志和完整性不符等门禁，均能正确拒绝。真实多级进程夹具复现了外层总期限
先杀父进程而留下子测试的问题；取消重复总期限后，内层逐步超时能够清理测试和
daemon，并把失败传回 M3，夹具没有残留进程。

## 后续验收条件

需要至少一套可使用的 NVIDIA mlx5/BlueField 和一套物理非 mlx5 PMD 环境，明确哪些
端口是管理口、哪些属于同一 embedded switch，再由实际 PMD 验证规则能力。
之后补齐 AGE/indirect 生命周期、设备事件恢复，进行线缆流量和 CPU 旁路验收。
驱动、rdma-core、DPDK mlx5 PMD 可以安装，但不会凭软件产生 PCI 网卡或交换芯片；
可继续使用当前虚拟环境开发和回归，实机报告保持单独待验收。
