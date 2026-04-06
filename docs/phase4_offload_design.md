# Phase 4 Offload Design

## 1. 背景

上一版 `offload.h` 只有裸规则结构，无法承接 `rte_flow pattern × action` 模型。

## 2. 本版改法

- `match.h`：匹配对象
- `action.h`：动作对象
- `flow_obj.h`：backend + direction + priority + transfer + dst_port_id
- `backend.h`：后端接口

## 3. 目标落点

### 软件路径
- 由 `lib/flow/` 完成查表与动作执行

### `rte_flow` 路径
- 由 `rte_flow_map.c` 把 `dppd_flow_obj` 映射为 attr/pattern/action

### transfer 路径
- 由 `lib/switch/transfer_pipeline.*` 预留跨域旁路能力

## 4. 为什么现在就要做这层设计

因为如果前面不显式建模：
- direction
- queue action
- counter / mark
- representor 关联
- transfer

后面很容易把软件路径写死，Phase 4 会变成推倒重来。
