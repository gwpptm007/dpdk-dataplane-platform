# Repo Map V2

## 本版新增与重组

- `include/dppd/`：新增公共头目录
- `app/ctrl.c` / `app/port_init.c` / `app/worker.c`：按职责拆开
- `lib/offload/core|soft|rte_flow|hw`：为 pattern/action/backend 抽象预留结构
- `lib/representor/`：PF/VF representor 占位
- `lib/switch/`：transfer pipeline 占位
- `examples/`：最小可拆解样例
- `scenarios/`：芯片与 DPU 场景说明
- `docs/reviews/`：评审原文与响应


## 新增 legacy 经验文档

- `docs/legacy_dpdk17_mode.md`：说明 DPDK 17 时代运营商网元媒体面托管模式。
- `docs/legacy_vs_modern.md`：对比 DPDK 17 旧模式与当前现代项目主线。
