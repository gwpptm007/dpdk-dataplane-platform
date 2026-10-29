# 实验入口与结果记录

所有下列命令在 **Linux 项目根目录**执行，`build` 指已经配置的 Meson 构建目录。
Windows 当前工作区用于阅读与文档检查，不能直接执行 Linux DPDK 实验。
本次连接情况和实际运行记录见 [验收记录](../validation.md)。

## 先确认环境

```bash
pkg-config --modversion libdpdk
meson --version
python3 -c 'import os; print(sorted(os.sched_getaffinity(0)))'
meson introspect build --buildoptions
meson test -C build --list
```

首次构建可以在独立目录使用：

```bash
meson setup build-dpdk-learning -Dtests=true -Dwerror=true -Db_ndebug=false
meson compile -C build-dpdk-learning
```

后续命令中的 `build` 相应替换为 `build-dpdk-learning`。`b_ndebug=false` 保留测试断言。
依赖包括编译器、Meson/Ninja、Python3、DPDK 开发包；net_ring 集成测试还要求该 PMD 库被构建系统发现。
CPU 的数量以 affinity 允许集合为准，不能只看整机核心数。

## 建议先做的三个实验

| 实验 | 命令 | 预测与观察 |
| --- | --- | --- |
| 报文改写失败边界 | `meson test -C build 'IPv4 routed packet rewrite' --print-errorlogs` | 先检查人工 mbuf 与复制失败包装，再理解原包为何不应被改坏 |
| 软件收发与短发送 | `python3 tests/integration/software_traffic.py --build-dir build --queues 2` | 至少 3 个可用 CPU；观察内容/短发送/资源归还断言，单队列与双队列都覆盖 |
| 多段、引用与路由 | `python3 tests/integration/ipv4_traffic.py --build-dir build` | 至少 3 个可用 CPU；对照 clone、分段、共享引用和原包断言 |

这两个 Python 入口负责选择 CPU、构造唯一 file-prefix，并使用 `--no-pci --no-huge`。
不用为了这些实验绑定管理网卡、修改物理路由或宣称测出了真实硬件吞吐。
其他章节提供相关测试名和脚本；优先运行当前问题对应的最小实验，再根据观察决定是否扩展。

## 结果应怎样记录

每次保留源码提交、DPDK 版本、完整命令、环境/CPU、退出码和原始日志。
在执行前写下自己的预测，执行后区分“观察到的值”和“对这个值的解释”。

```text
问题：TX ring 满时，未提交包由谁释放
预测：短发送尾部由 worker 释放，停止并排空后对象数恢复
执行：填写命令、时间、源码版本、DPDK 版本和日志路径
观察：填写真实退出码、断言和计数，不复制示例作为结果
结论：仅覆盖 net_ring 软件路径，不推导真实 NIC 完成回收时序
```

## 无硬件时可以学习的边界

结构推演和源码跟踪不要求设备；net_ring/TAP 能验证不少正式软件路径。
描述符 DMA、真实 RSS、跨 NUMA 性能需要相应物理环境；DPU transfer 旁路与收益需要目标 DPU。
暂时不能执行的实验标记 `not_run`，保留复现入口，不用模拟结果替代硬件结论。
