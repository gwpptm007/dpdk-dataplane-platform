# M3 的 DPDK 版本兼容验收

本轮在 135 的独立 home 目录安装 DPDK 24.11.8，并保留系统与既有私有 DPDK 21.11.9。
没有替换系统库、修改驱动绑定或操作实体网卡，严格构建仍开启 `tests=true` 和 `werror=true`。

## 修复的实际问题

DPDK 22.11 起，公共 flow 校验直接拒绝 transfer 与 ingress/egress 同时设置。
项目此前为兼容 21.11 保留 ingress，不能把同一组属性原样用于新版本。
现在按版本选择方向，普通 ingress/egress 规则不变，绑定共享动作的规则也执行相同检查。
共享对象自身的 indirect 配置没有套用普通规则属性的限制。

24.11.8 还对旧 telemetry 无符号字典与数组接口产生弃用告警，导致 `-Werror` 构建失败。
兼容层在 23.03 起使用新名称，旧版本继续使用原接口，数值仍完整保留为 `uint64_t`。
实际 API 边界、官方来源与驱动契约见 [flow 调用契约](m3_flow_contract.md)。

真实容器检查还发现旧失败字段键含有短横线，新版 DPDK 会返回 `EINVAL`。
现在仅将两个 Telemetry 键规范为 `failed_invalid_input`、`failed_not_found`，CLI 分类名和数值保持原样。
测试夹具同步采用对应版本的真实符号和整数宽度，继续覆盖分页、故障注入、并发与完整六十四位值。

## 独立环境

| 项目 | 本轮路径或配置 |
| --- | --- |
| 工作目录 | `/home/wq7/dppd-dpdk24-compat` |
| 安装前缀 | `/home/wq7/.local/dppd-dpdk-24.11.8` |
| DPDK | 官方 24.11.8 发布包，核对官方 MD5，另保存 SHA256 |
| 目标 | generic、共享库、mlx5/ring/null/tap 及必要 bus/common 依赖 |
| mlx5 依赖 | 使用已有 rdma-core/ibverbs，安装库不视为发现真实网卡 |

构建和测试保存实际动态库路径，避免以系统 21.11 的运行结果冒充 24.11。
测试仅使用软件 PMD，mlx5 库成功编译不证明硬件初始化或 flow 安装成功。

## 最终验收结果

2026-10-18，四套严格构建使用同一份最终源码，单测均无失败、跳过或超时。

| 构建 | 单测 | 软件回归 | M3 联合验收 |
| --- | --- | --- | --- |
| 135 系统 DPDK 21.11.9 / GCC 11 | 70/70，内含 76 项硬件工具门禁 | 四步核心回归通过 | 六步全部通过 |
| 135 ASan/UBSan / DPDK 21.11.9 | 同上 | 四步核心回归通过 | 六步全部通过 |
| 134 私有 DPDK 21.11.9 / GCC 13 | 同上 | 四步核心回归通过 | 六步全部通过 |
| 135 私有 DPDK 24.11.8 / GCC 11 | 同上 | 四步核心回归通过 | 六步全部通过 |

每套均覆盖五组共享恢复、正常/RESET/RMV/DESTROY 四组正式设备事件及 31 组功能模拟。
255 项源码逐项相同，摘要表按键排序、紧凑 JSON 编码后的 SHA256 为：

```text
004eec9e62b4c1ea25c9f4df98c06451d7c59b7aa0ff3625d53337ddecdb4371
```

现代构建另外核对了 `pkg-config` 前缀、全部 `librte_*` 实际动态库路径及已安装 mlx5 PMD 的文件摘要和依赖。
独立真实 API 演练确认：混用方向返回 `EINVAL`；transfer-only 可以进入 net_ring 驱动，随后因无 flow 实现返回 `ENOSYS`。
真实 Telemetry 容器确认旧键返回 `EINVAL`，规范键可保存 `UINT64_MAX`。
这两项证明的是公共 API 行为，不是硬件规则支持。

前期严格构建和首次单测的失败日志另行保留；表内结果均来自修复后的最终同源重跑。
原始报告、动态库记录、公共 API 演练源码与输出、发行包摘要及核对汇总保存在
`build/native-check/m3-compat-artifacts/`，提交前后均逐项核对源码摘要。
本轮没有重复完整 TAP，完整 TAP 记录继续按 [通用身份轮次](m3_generic_identity.md#本轮整体验收) 登记。
ASan/UBSan 未报告错误；本轮沿用关闭 LeakSanitizer 的配置。

135/134 实际只读硬件预检仍为 blocked，报告通过 schema 验证，未执行物理报文或复位。
所有联合报告保留 `hardware_acceptance=not_run`、`complete_m3_validation=false`。

## 实机范围仍需单独核对

21.11.9 mlx5 缺少当前 `pair` 使用的 `REPRESENTED_PORT` 源匹配路径，详见
[21.11.9 能力矩阵](m3_pmd_capabilities_21_11.md)。24.11.8 已有对应匹配和动作实现，
这次兼容修复消除的是软件调用与构建障碍，具体硬件、固件和规则组合仍须真实 PMD 校验。

当前项目使用同步 `rte_flow` 接口，不能把 mlx5 的 HWS 模板/队列模式直接当作已支持。
进入真实 DV 验收前应核对实际 `dv_flow_en` 与 DevX、ASO、AGE/共享 COUNT 组合限制，
不能仅凭新版本的功能清单推断组合可用。

24.11.8 mlx5 仍没有 `dev_reset` 回调，升级版本不会自动提供项目主动 `device-reset` 的硬件成功证据。
真实端点移除与设备复位后的清理、身份核对和 reconciliation 仍须专用设备与操作记录。
本文没有将模拟恢复、库安装或版本兼容记为完整 M3 实机验收。
