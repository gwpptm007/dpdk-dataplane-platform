# M3 驱动调用契约

本项说明适配器调用与 DPDK 版本兼容契约，不代表已完成硬件卸载验收。

## transfer 的方向

方向属性按 DPDK 版本分别编译，不能把旧版驱动的要求直接用于新版本公共入口。

| DPDK 版本 | transfer 规则方向 |
| --- | --- |
| 21.11 | `ingress=1`、`transfer=1`、`egress=0` |
| 22.11 及以后 | 只设置 `transfer=1`，`ingress=egress=0` |

源端点由 `REPRESENTED_PORT` 匹配项指定，目的端点由同名动作指定，不以安装端口代替源端口。
21.11.9 mlx5 的旧方向检查需要入方向；但这不证明它支持当前整套新端口匹配，
具体版本和 PMD 能力还须单独核对。

官方在 [21.11 API 变更](https://doc.dpdk.org/guides-21.11/rel_notes/release_21_11.html#api-changes)
弃用方向混用，并在 [22.11 API 变更](https://doc.dpdk.org/guides-22.11/rel_notes/release_22_11.html#api-changes)
明确禁止 transfer 与 ingress/egress 同时出现。
`RTE_VERSION_NUM(22, 11, 0, 0)` 是这次兼容分支的边界。

已核对的 24.11.8 [公共校验入口](https://github.com/DPDK/dpdk-stable/blob/v24.11.8/lib/ethdev/rte_flow.c)
会在调用 PMD 前返回 `EINVAL`，不会替调用方清除多余方向。
相应 [属性定义](https://github.com/DPDK/dpdk-stable/blob/v24.11.8/lib/ethdev/rte_flow.h)
也明确 ingress/egress 只用于 non-transfer 规则。
本次只调整普通规则属性，不把这条限制未经核对地套到共享对象配置。

## Telemetry 的无符号数值接口

23.03 起字典使用 `rte_tel_data_add_dict_uint()`，数组使用 `rte_tel_data_add_array_uint()`。
旧版本分别继续使用 `rte_tel_data_add_dict_u64()` 和 `rte_tel_data_add_array_u64()`。
新名称的参数仍是 `uint64_t`，不截断计数，也不改变返回错误。
兼容层只集中选择函数名称，严格构建继续保留 `-Werror`，不屏蔽弃用警告。

引入边界已对照官方 [22.11 头文件](https://github.com/DPDK/dpdk/blob/v22.11/lib/telemetry/rte_telemetry.h)
与 [23.03 头文件](https://github.com/DPDK/dpdk/blob/v23.03/lib/telemetry/rte_telemetry.h)。
24.11.8 的旧名称已带弃用标记，因此不能因为它仍能链接就忽略严格编译失败。

失败分类的两个 Telemetry 计数字段统一使用 `failed_invalid_input` 与 `failed_not_found`，
替换带短横线的旧键 `failed_invalid-input` 与 `failed_not-found`。
这只修正构造字典时的字段键，分类名称、CLI 显示、字段值和管理协议保持原样。
23.03 及以后的真实容器会拒绝带短横线的键；相关实现见
[官方 Telemetry 数据容器](https://github.com/DPDK/dpdk/blob/v23.03/lib/telemetry/telemetry_data.c)。
契约测试按版本包装实际调用的符号，保留错误注入、分页、并发与完整六十四位数值验证，
同时检查两个规范键均已输出以及新版拒绝非法键。

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
后续 DPDK 21.11.9/24.11.8 四套同源整体验收、真实公共 API 演练和动态库记录见
[版本兼容验收](m3_dpdk_compatibility.md#最终验收结果)。

这是模拟驱动入口的契约测试，未证明 mlx5、BlueField 或非 mlx5 实机支持这些规则。
后续已接通 [AGE 控制与模拟闭环](m3_age.md)，契约测试也覆盖 AGE 参数与独立查询。
首期 [共享 COUNT](m3_shared_count.md) 已接通真实 indirect create/query/destroy 与规则
INDIRECT 动作绑定。绑定下标对应规则动作下标，COUNT 私有查询不返回共享汇总，
共享查询同样要求包数和字节数都有效。prepare 到最终释放持有引用，销毁失败保留
handle；native `before_create` 恢复日志模式明确拒绝 indirect，不绕过日志或降级。
共享 RSS/METER/AGE 和更新仍未实现；真实 PMD 的 AGE 和共享 COUNT 支持仍需单独验收，
不会仅因 DPDK 头文件中存在 API，就登记为硬件已验证能力。
