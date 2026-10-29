# 教材检查与实验状态

源码核查基线：`380d7da738eb363ae1424228e332249326ae1b54`；记录日期：2026-10-29。
这份记录验收教材准确性与可读性，不代表读者已经掌握教程，也不替代项目的软件/硬件阶段验收。

## 核查范围

十章覆盖系统模型、EAL/设备、mbuf、队列、多核、QSBR、rte_flow、DPU、异步模板及排障。
章节引用真实调用点与官方版本资料，交叉审查过资源生命周期、测试名称、命令参数和模拟边界。
历史学习进度文件保持不变，业务代码和原有测试未修改。

## 本次实验环境

| 检查 | 实际结果 | 能说明什么 |
| --- | --- | --- |
| SSH 到 192.168.65.135 | 端口 22 连接超时 | 当前不能在该主机重跑实验，不能推断源码测试失败 |
| SSH 到 192.168.65.134 | 端口 22 连接超时 | 同上，不是密码验证失败 |
| 本地 WSL | 未提供可用 Linux 运行环境 | 本地 Windows 不作为 Linux DPDK 实验结果 |
| 本轮 Linux DPDK 实验 | `not_run` | 章节中的实验命令已按源码核对，未将预期写成实际通过 |
| 真实 NIC/DPU、DMA、RSS、NUMA 收益 | `not_run` | net_ring/TAP 或包装测试均不能替代这些证据 |

网络恢复后，先按 [实验入口](labs/README.md) 检查源码与构建版本，再执行章节对应命令。
新结果应记录日期、版本、环境、完整命令和日志，不覆盖此前报告，也不自动改变个人掌握状态。

## 文档和图示复查方法

在项目根目录执行：

```bash
python docs/dpdk/tools/check.py --output build/dpdk-learning/content-check.json
node docs/dpdk/tools/render-check.cjs build/dpdk-learning/mermaid-input.json /path/to/mermaid.min.js
```

第二步需要 Node.js 可解析 `playwright`、可用 Chromium，以及调用者提供的 Mermaid 浏览器包。
`DPDK_DOC_BROWSER` 可指定已安装浏览器可执行文件；依赖和截图留在构建/临时目录，不随教材复制。
本次使用 Mermaid 11.12.0。检查只能证明所用版本能渲染，GitHub 将来升级渲染器时仍可能需要复查。

静态工具检查本地链接、围栏、章节覆盖与尾空白，并输出图示清单。
浏览器工具逐图解析和渲染，检查可见尺寸与横向溢出，保存 SVG、PNG 和报告。
图片的内容准确性仍需人工与代码对照，语法通过不等于讲解准确。

Archify 生成的 HTML 保留生成器空行缩进，避免改变已绑定回执的文件摘要；
仅该产物在本目录 `.gitattributes` 中排除空行尾部空白检查，手写正文仍检查尾空白。

交互图由 Archify 3.0.1 的 `finalize sequence` 生成，
输入为 [candidate.json](diagrams/tx-ownership/candidate.json)，源码引用固定到上述 Git 基线。
[自动检查摘要](diagrams/tx-ownership/tx-ownership.finalize-summary.json) 记录 schema、产物完整性、源码引用及浏览器检查结果。

## 本次最终结果

| 项目 | 结果 |
| --- | --- |
| 本地文档静态检查 | 18 个 Markdown、10 章，本地链接、围栏、尾空白及章节结构全部通过 |
| Mermaid 浏览器渲染 | 34/34，通过解析、可见尺寸与横向溢出检查 |
| 内存 SVG 与交互时序图 | 已在浏览器打开并查看截图，文字与关系可读 |
| 图示人工复查 | 检视全部 Mermaid 的截图总览，修正 NUMA 图的边标签遮挡并重新渲染复查 |
| 源码交叉审查 | 各组章节交叉核查，无未解决的实质问题；修正 telemetry 现场原子读取与发布副本的区分 |
| 实验入口核查 | 命令参数、Meson 测试名、CPU 前提及模拟/包装边界与源码对齐 |
| 代码与旧学习记录 | `app/`、`lib/`、`include/`、`tests/`、`docs/learning/` 相对源码基线无改动 |
| Linux 运行及硬件实验 | 本轮 `not_run`，环境限制如上 |

随教材保留 [静态检查报告](reports/content-check.json) 与 [图示渲染报告](reports/render-check.json)。
原始逐图 SVG、截图和汇总预览留在本地 `build/dpdk-learning/`，不重复提交到仓库。
检查工具可复现上述文档检查；人工阅读、源码语义核查与硬件验证不能由它们自动替代。
