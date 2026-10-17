# M3 共享 COUNT

共享 COUNT 让多条规则把命中包数和字节数记到同一个对象。例如一对双向 transfer
规则可以汇总两个方向的流量；删除其中一条规则后，另一条继续使用原来的累计值。
对象有自己的 ID 和版本，规则保存这两个值，不保存驱动指针。

首期只实现 **共享 COUNT**，使用 DPDK indirect action。共享 RSS、METER、AGE、动作
更新不在首期共享动作范围，后续已接通 [复位重建模拟](m3_reset_simulation.md)。真实 PMD、固件和 ASIC 的支持尚未验收；
控制测试和模拟报文通过不等于硬件卸载、CPU 旁路或性能验收通过。

## 对象与规则的约束

共享对象最多 64 个，每个指定一个安装 ethdev 和 `ingress|egress|transfer` 域。
引用规则必须使用**相同安装端口和相同域**；只有 switch domain 相同还不够。
transfer 图仍须满足安装、源、目的端点处于同一已知交换域的原有要求。

规则动作 `DPPD_ACTION_SHARED_COUNT` 保存精确对象 ID/generation，每条规则最多一个，
不能与本规则私有 COUNT 同时出现。它可以与 AGE 共用，仍须保留唯一的末尾去向动作。
管理 transfer 模板生成的顺序为：

```text
可选私有 COUNT 或共享 COUNT → 可选 AGE → 明确目的端点
```

共享动作没有等价软件降级。对象不存在、版本过期、安装端口或域不符，分别明确返回
`ENOENT`、`ESTALE`、`EXDEV`；缺少 indirect 或绑定规则接口时返回 `ENOTSUP`。
plan 已检查逻辑身份和范围，apply 会重复检查，正式 backend 再将精确身份解析为
对应动作位置的借用 handle，不临时创建一个“看起来一样”的共享对象。

## 管理命令与版本

当前管理协议为 **v26**，正式 daemon、模拟服务和 `dppctl` 必须配套更新。

```text
shared-count-create ID PORT ingress|egress|transfer
shared-count-get ID EXPECTED
shared-count-query ID EXPECTED
shared-count-delete ID EXACT_GENERATION
shared-count-list [AFTER_ID [EXPECTED_REVISION]]
transfer-plan ID INSTALL SRC DST TEMPLATE oneway|bidir [count|shared-count:ID:GEN] [priority:N] [age:SECONDS]
transfer-apply ID INSTALL SRC DST TEMPLATE oneway|bidir EXPECTED [count|shared-count:ID:GEN] [priority:N] [age:SECONDS]
```

create 分配新版本，不隐式创建引用规则。get/query 的 `EXPECTED` 可以是精确版本或
`any`；delete 只接受精确非零版本，不能用 `any`。create/get 输出对象身份、安装口、
域和 `references`；query 再加 `hits/bytes`。查询不会清计数，驱动缺少有效包数或
字节数时明确失败，不伪造零值。

**只有 `shared-count-query` 返回共享汇总**。`count RULE_ID EXPECTED` 始终查询该规则
的私有 COUNT；仅有共享动作的规则返回 `ENODATA`，不能把汇总值冒充该方向独立计数。

对象 generation、共享表 revision 和规则账本 generation 是三种不同的版本：

- 对象 generation 在创建时分配，删除重建同 ID 也得到新版本，旧引用不能命中新对象
- 共享表 revision 在对象成功创建、删除后推进，独立于规则账本版本
- transfer apply 的 `EXPECTED` 仍是 plan 返回的规则账本版本，不能填对象版本

list 按对象 ID 升序，每页最多 4 个。续页用上一页的 `next` 和 `revision`；对象表中途
变化返回 `ESTALE`，需重读第一页。`references` 是查询当时的活值，规则安装、删除和
AGE 回收会改变它而不推进共享表 revision，因此跨页引用数不是同一时刻的全表快照。

## 引用、失败与 AGE

正式 backend 从 prepare 预留开始持有引用，直到对象槽位最终释放才归还。引用数
包含准备中、已安装及补偿删除失败的规则，不只是 desired 账本中的规则数量。
只要引用非零，删除共享对象返回 `EBUSY`，保护仍在使用的驱动 handle。

成功删除单条规则只释放它的引用，不清共享累计。AGE 明确到期后走同一个精确版本
删除流程，成功释放引用；删除失败则保留规则、handle 和引用，按 AGE 节流规则重试。
双向两条规则独立老化，不承诺一起释放。

创建失败不发布对象；销毁失败保留原对象供重试。规则创建中途失败会补偿已创建规则，
若补偿也失败则保留实际引用并进入恢复隔离。隔离期间 get/list 允许只读定位，
create/query/delete 返回 `EUCLEAN`；不能根据只读信息宣称设备已恢复转发。
存储失败也可能在实际创建或删除已经完成后返回 `EUCLEAN`，应检查 dirty 状态并修复
保存，不能盲目重复整个请求。

## 单个 v5 快照与重启

共享定义、共享表 revision 和引用规则一起保存在**同一个 v5 规则快照**，避免分成
两个文件后只保存了一半。即使对象全部删除，只要共享 revision 非零仍写 v5，保留
删除历史，防止重启后复用旧 generation。当前加载器兼容 v2/v3/v4/v5。

恢复先验证全部定义及引用，再创建共享对象，随后恢复引用规则；失败走既有补偿和
隔离流程。对象与规则精确版本保留，引用数重新建立；驱动 handle、包数和字节数
不持久化，因此共享累计从零开始。带 AGE 的规则也重新开始闲置计时。
这不保证进程崩溃后真实 ASIC 残留已清理。

启用 native `--recovery-path` 所接入的 `before_create` 安装日志模式时，共享创建和
绑定明确返回 **`ENOTSUP`**。现有跨进程恢复日志不能记录 indirect 对象生命周期，
不能将新对象绕过日志创建，也不静默改成私有 COUNT。共享 v5 快照不等于旧恢复保护
已经支持 indirect；生产部署仍需单独解决设备清理与复位恢复。

## 功能模拟

按 [模拟说明](m3_simulation.md) 启动独立服务后，用真实客户端先创建对象：

```sh
./build/dppctl --socket /tmp/dppd-my-m3-demo/ctl.sock shared-count-create 900 0 transfer
./build/dppctl --socket /tmp/dppd-my-m3-demo/ctl.sock shared-count-get 900 any
```

从回执读取实际 generation，再将 `shared-count:900:实际版本` 加到 plan/apply；apply
同时使用本次 plan 的实际 `repository-generation`。不能把示例编号当作环境证据。
注入两个方向后查询 `shared-count-query 900 any`，删除一边再注入，累计应继续增加。
删除对象前须先删除所有引用规则，且使用当前对象精确版本。

```sh
python3 scripts/simulate_m3.py demo --build-dir build
python3 scripts/simulate_m3.py fault --io-socket /tmp/dppd-my-m3-demo/io.sock shared-query 1
```

模拟故障 `shared-create/shared-query/shared-destroy` 分别返回 `ENOSPC/EIO/EBUSY`。
status 区分 `shared_objects/shared_refs` 与 desired 规则，便于看见补偿残留；它们是
模拟驱动实际对象和借用数，正式 get/list 的引用数还包含 prepare 预留。

本期新增双向汇总、删除保护、AGE 引用释放、三类故障、范围拒绝、ABA、列表版本、
快照归零和回滚残留演练。共享 COUNT 交付时为 24 组，后续完整 demo 还包含
[复位恢复](m3_reset_simulation.md)。实机验收始终单列为未执行。

## 本轮验收结果

2026-10-18，135 系统 DPDK 21.11.9 / GCC 11、135 ASan/UBSan、134 私有 DPDK
21.11.9 / GCC 13.2 三套最终构建分别通过 **60/60 单测、四步核心回归和 24 组模拟**，
没有失败、跳过或超时。驱动契约、控制面持久化和现有软件 worker 均在同轮回归中。

三套统一报告、软件回归和模拟报告的 233 个源码摘要逐项一致，并与本地核对。
摘要表排序并紧凑 JSON 编码后的 SHA-256 为：

```text
04fa3559dc588bba8c1a0f2c1342b42078055066f5130bb20731cf99e0987d96
```

185 个 C 源码和头文件通过格式检查，Python 语法、中文注释结尾、行宽和文档链接
通过检查。报告复制件位于 `build/native-check/m3-shared-artifacts/`，不提交构建产物。
ASan/UBSan 日志没有错误，LeakSanitizer 关闭；mbuf 归还由正式 worker 内存池记录核对。
本轮没有重新计入八组完整 TAP，也没有登记为真实硬件或完整 M3 验收。

主要实现见 [共享身份](../include/dppd/shared_count.h)、[DPDK indirect 适配](../lib/offload/shared_count.c)、
[对象与引用](../lib/offload/rte_flow_backend.c)、[控制与保存](../lib/control/control.c) 和
[模拟演练](../scripts/simulate_m3.py)。
