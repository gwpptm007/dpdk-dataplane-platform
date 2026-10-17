# M3 驱动调用契约

本项修复进入 M3 前可在真实适配器代码中证明的两处问题，不代表已完成硬件卸载验收。

## transfer 的方向

transfer 规则同时设置 `ingress=1`、`transfer=1`、`egress=0`。源端点由
`REPRESENTED_PORT` 匹配项指定，目的端点由同名动作指定，不以安装端口代替源端口。
原实现只设置 transfer，在 DPDK 21.11.9 mlx5 的方向检查中会被拒绝。

语义参考 [DPDK 21.11 switch representation](https://doc.dpdk.org/guides-21.11/prog_guide/switch_representation.html)，
并核对测试机 DPDK 21.11.9 的 `mlx5_flow_dv.c` 中 transfer 禁止 egress、要求唯一方向的校验。

## 硬件计数的有效性

`dppd_flow_query_count()` 只有在驱动同时提供有效包数和字节数时才写出结果。缺少
`hits_set` 或 `bytes_set` 时返回 `-ENODATA`，不将未提供的字段伪装成零，也不覆盖调用方
原有输出。控制服务和 CLI 保留错误返回，不能用这类结果证明没有报文经过 CPU。

有效位定义见 [DPDK COUNT 查询结果](https://doc.dpdk.org/api-21.11/structrte__flow__query__count.html)。

## 验证和限制

`test_flow_contract` 直接捕获生产适配器传入 `rte_flow_validate/create` 的参数，核对
transfer 方向、源/目的端点、COUNT 和所有配置对象的存活时间；覆盖验证失败不创建、
创建失败不留下 handle、删除失败保留 handle，以及计数缺失和驱动错误保持原输出。
系统 DPDK 21.11.9 严格编译、普通及 ASan/UBSan 契约测试通过。

这是模拟驱动入口的契约测试，未证明 mlx5、BlueField 或非 mlx5 实机支持这些规则。
后续已接通 [AGE 控制与模拟闭环](m3_age.md)，契约测试也覆盖 AGE 参数与独立查询。
indirect/shared action 仍待实现；真实 PMD 的 AGE 和共享动作支持仍需单独验收，
不会仅因 DPDK 头文件中存在 API，就登记为硬件已验证能力。
