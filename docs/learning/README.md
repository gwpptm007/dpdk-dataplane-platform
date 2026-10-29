# 源码学习材料

入口：[学习总目录](index.html)。在本地浏览器中打开，按章节查看示例、代码、解释和状态变化。

本目录保存长期维护的学习材料，与 `.archify/` 中的架构图、流程图产物分开管理。学习页面最初随图表整理，现已迁到这里，内容只在本目录维护。旧学习目录和跳转页已全部移除，请使用本目录的入口；1.1 页面仍链接到 `.archify/` 中保留的完整流程图。

## 目录与维护方式

| 路径 | 用途 |
| --- | --- |
| `index.html` | 学习总目录，由 `build.py` 生成 |
| `progress.json` | 经对话确认的各节进度、当前停点、下一步与记录日期 |
| `packet-path/` | 1.1 收发与归属，页面本身也是后续章节共用样式的来源 |
| `packet-parse/` | 1.2 报文解析，内容在 `lesson.json`，布局在 `lesson-template.html` |
| `software-rules/` | 1.3 软件规则，内容在 `section.json`，布局在 `section-template.html` |
| `chapters.json` | 1.4～2.4 的章节内容与源码引用；`principles` 保存原理说明和 plain text 示例 |
| `chapter-template.html`、`install-template.html` | 章节页面模板 |
| `section-*.html` | 生成的章节页面 |
| `sources/`、`manifest.json` | 构建时生成的源码展示副本、行号与引用清单 |

修改章节数据、模板或源码注释后，从项目根目录运行：

```powershell
python docs/learning/build.py
```

构建会刷新源码副本、行号和导航，并将 `progress.json` 的进度显示在首页和章节状态中。每次确认新的学习停点后，更新该文件再构建；页面点击不会自动标记学会，也不会自动重写教学解释。不要只修改生成的 `index.html`、`section-*.html`，否则下次构建会覆盖改动。

最近补齐的函数指针、QSBR、临时记录生命周期和规则版本讲解，可从首页“最近补齐的讲解”直接进入。原理与例子在当前步骤和“展开全部步骤”两种阅读方式中都会显示；内容同步不会自动推进学习停点。

## 验证记录

本次讲解补齐的检查见 [content-check.json](content-check.json)，检查新增原理块、连续阅读、源码链接以及学习停点保持不变。

本次学习进度更新的检查见 [progress-check.json](progress-check.json)，包含首页进度、九节状态、继续学习按钮和页面交互验证。

迁移验证见 [migration-check.json](migration-check.json)。它检查迁移后的页面、导航与交互，不代表 DPDK 运行验证，也不代表个人已掌握全部章节。

原有 `browser-check.json`、`source-baseline.json`、`source-check.json`、`completion-audit.json`、各节截图及检查记录保留为历史证据，其中的旧路径、源码版本和检查时间不代表当前状态。

`source_audit.py`、`verify.py`、`completion_audit.py` 是原阶段的检查工具。旧源码基线不适用于后续所有代码变更；重新验证应使用独立基线和报告，不直接覆盖历史记录。`review.html` 保留了当时的整理说明，后续维护以本文件为准。
