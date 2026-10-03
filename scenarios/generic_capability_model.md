# 通用 capability model

不要以厂商名选择代码路径。每个端点在启动和设备恢复后生成 capability profile：

- ethdev：queue 上限、RSS types、RX/TX offload、descriptor 限制、NUMA socket；
- topology：representor 标记、driver、switch domain/port；
- flow：domain、item/action 组合、counter/age、indirect action、template/async、资源限制；
- operational：规则能否跨 stop/start 保留、设备 reset 行为、错误码与恢复方式。

静态 capability bit 只能用于快速过滤；具体规则仍需 `rte_flow_validate()`。profile 必须带 PMD、设备、固件与 DPDK 版本，不能永久缓存。

2026-10-03 已落地本机管理 v12 的 `capability-show` 和 `probe-drop/probe-filter`：
启动身份、queue/RSS/offload、描述符限制和累计驱动校验记录；完整规则答复最多缓存
五秒，共享 64 个槽位。正式安装始终重新校验，观察过的元素集合不能推出任意组合
支持。固件未知明确显示，重启建立新缓存。高级 flow、容量和 reset 保留语义仍待
对应设备验收，详见 [实现与验证](../docs/capability_probe.md)。
