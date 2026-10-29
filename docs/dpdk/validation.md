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

## 深入样章

新增入口：[一个包怎样从网卡进入程序](reader/index.html)，验收日期：2026-10-29。
正文固定项目源码 `d9b6305ac95cff5bf2eadbfa7f2fae3049679f66`，与上面的业务源码基线一致。
驱动案例固定 DPDK `v24.11` 的 `ixgbe_recv_pkts` 标量、单段路径；官方 24.11 文档站会跟随维护版本更新。
本轮只验收教学内容和浏览器交互，没有重新执行 Linux DPDK 或真实 NIC 实验，也没有重新探测两台主机。

内容审查覆盖描述符、DMA 地址、内存可见性、mbuf 所有权、替补分配失败、RDT 发布边界、
批量成本、多段读取、共享改写、池容量和本项目调用链。
修正了“写 RDT 就必然让最新替补立即可用”的过度简化；图示明确假定同批后续槽位已处理，发布范围覆盖 B。
资源公式区分 RX 流程占用与设备当前可接收容量，所有计算均标注假设，不作为实测吞吐或丢包时刻预测。

| 项目 | 结果 |
| --- | --- |
| 离线阅读 | file 协议直接打开，拦截远程请求后仍正常，无外部资源请求或脚本异常 |
| 七步推演 | 逐步跳转、前后边界、键盘操作、全部展开和切回均通过 |
| 资源练习 | 正常、零余量、不可能占用、空值、非整数和越界输入均检查 |
| 阅读与布局 | 桌面、390px 和 320px 手机、横向图示滚动、本地链接和目录锚点通过 |
| 打印和无脚本 | 打印自动显示全部步骤；禁用脚本时正文和简化因果链仍可阅读 |
| 手工视觉复查 | 查看桌面开篇、各步骤和手机截图，修正一条穿过缓冲区说明文字的连线 |
| 独立时序图 | Archify showcase 四项门禁通过；亮暗主题及说明卡片已查看，无裁切 |
| 原有材料 | 保留十章 Markdown、旧学习材料和个人掌握记录；不修改业务代码 |
| 运行实验 | 本轮 `not_run`；页面状态与计算仅为教学演示 |

复查页面交互：

```bash
node docs/dpdk/tools/reader-check.cjs
```

需要 Node.js 能解析 `playwright`，并有可用 Chromium；可以用 `DPDK_DOC_BROWSER` 指定浏览器。
报告与截图写入 `build/dpdk-learning/reader/`，随教材保留 [页面检查报告](reports/reader-check.json)。
它检查行为与几何边界，不代替内容审查或人工看图。
独立时序图的 [候选](reader/sequence/candidate.json) 和 [检查摘要](reader/sequence/rx-cycle.finalize-summary.json) 用于复现。

RX 正文直接维护 `reader/index.html`，共享样式在 `reader/lesson.css`，RX 七步状态和资源练习在 `reader/lesson.js`。
通用翻页、展开和目录高亮操作在 `reader/reader-ui.js`，由 RX 与 TX 页面共用。
这些文件无需构建；新增内容后运行静态检查与页面检查，并实际查看截图。
不要手改生成的 `reader/sequence/rx-cycle.html`；应修改其候选并重新 finalize，保持产物与回执一致。

## TX 深度章节

新增入口：[一个包交给网卡以后](reader/tx.html)，验收日期：2026-10-29。
项目源码核查固定 `c793dbdb70f12f3dcda0085b263e6984479479c8`，业务代码与 RX 章基线一致。
本章只增加教学内容与页面交互，原业务代码、Linux 测试和个人掌握记录未修改。

主例固定 DPDK `v24.11` 的 ixgbe simple 标量路径：128 个 TX 描述符、两个阈值均为 32，
用一组 32 个普通直接、单段、独占 mbuf 追踪提交、DD 完成与清理。
源码审查确认初始可用描述符为 127，提交后为 95，成功回收一组后恢复为 127。
明确区分该示例、完整/向量路径和项目的 MULTI_SEGS 配置，不声称项目实际必走 simple。

五包交互练习独立解释 ethdev 的已接受前缀契约，允许选择返回值 0～5。
页面按项目逻辑释放未接受尾部，随后把完成和回收分开，始终核对 8 个对象的去向。
该模型不复现真实 NIC 的 RS 分组、硬件时间或吞吐。

| 检查 | 结果 |
| --- | --- |
| TX 页面交互 | 11 组检查通过，覆盖 7 步、展开与切回、键盘、自检、0～5 全部返回值、非法输入和重复操作限制 |
| 资源守恒 | 应用持有 + 发送路径持有 + 池中空闲恒为 8；已完成数只作为发送路径持有的子集 |
| 页面与离线 | 桌面、390px/320px 手机、打印、无脚本和本地锚点通过；无远程资源请求与脚本异常 |
| RX 回归 | 提取共用阅读控件后，原 RX 页面 10 组检查再次通过 |
| 内容与图示 | 核对提交顺序、RS/DD、低流量尾部、cleanup/prepare、mempool 计数范围、项目发送与关闭清理；人工查看七步图和手机练习 |
| 原有 TX 时序图 | 复用原产物，未改变候选、HTML 或回执字节 |
| Linux / 物理 NIC | 本轮 `not_run`；软件实验保留可复现命令和预测，不把教学状态当成运行证据 |

复查命令：

```bash
python docs/dpdk/tools/check.py
node docs/dpdk/tools/reader-check.cjs
node docs/dpdk/tools/tx-reader-check.cjs
```

运行环境要求与 RX 页面检查相同。TX 检查报告与截图写入 `build/dpdk-learning/tx-reader/`；
提交的 [TX 页面检查报告](reports/tx-reader-check.json) 记录正文、脚本与样式的 SHA256，
[RX 页面检查报告](reports/reader-check.json) 更新为共享控件调整后的回归结果。
浏览器报告只记录自动检查；内容语义与截图可读性另行审查。

TX 正文维护 `reader/tx.html`，图示状态与练习逻辑维护 `reader/tx-lesson.js`。
继续阅读时可从 RX 页的“下一章”进入，也可从 TX 页返回 RX。
