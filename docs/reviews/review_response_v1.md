# 评审响应 V1

本文件说明：项目骨架如何吸收 `project_review.md` 中的意见。

## 已落实

### 1. 目录结构
- 已新增 `include/dppd/`
- 已将 `main_loop.c` 重命名为 `worker.c`
- 已将 `port.c` 重命名为 `port_init.c`
- 已将 `pipeline.c` 重命名为 `pipeline_fwd.c`
- 已将 `rewrite.c` 重命名为 `pkt_rewrite.c`
- 已把 `meson.build` 改为按子目录 `subdir()` 聚合

### 2. SmartNIC / DPU 方向抽象
- 已新增 `lib/offload/core/match.h`
- 已新增 `lib/offload/core/action.h`
- 已新增 `lib/offload/core/flow_obj.h`
- 已新增 `lib/offload/rte_flow/rte_flow_map.c`
- 已新增 `lib/representor/`
- 已新增 `lib/switch/`
- 已新增 `scenarios/`

### 3. 学习与验证支撑
- 已新增 `examples/`
- 已新增 `tests/pdump/README.md`
- 已新增 `docs/reviews/` 目录保存评审与响应

## 暂未落地但已留接口
- 真实 `rte_flow` pattern / action 构建
- representor 真实探测与 VF 绑定
- transfer bypass 行为实现
- 厂商特定后端（ice/cn10k/BlueField）

## 结论

本版目标不是一次做完所有功能，而是确保项目从现在开始沿着 **“Phase 1 软件路径扎实 + Phase 4 offload 可落地”** 的方向演进。
