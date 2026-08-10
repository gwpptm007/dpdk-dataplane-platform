# 通用 capability model

不要以厂商名选择代码路径。每个端点在启动和设备恢复后生成 capability profile：

- ethdev：queue 上限、RSS types、RX/TX offload、descriptor 限制、NUMA socket；
- topology：representor 标记、driver、switch domain/port；
- flow：domain、item/action 组合、counter/age、indirect action、template/async、资源限制；
- operational：规则能否跨 stop/start 保留、设备 reset 行为、错误码与恢复方式。

静态 capability bit 只能用于快速过滤；具体规则仍需 `rte_flow_validate()`。profile 必须带 PMD、设备、固件与 DPDK 版本，不能永久缓存。
