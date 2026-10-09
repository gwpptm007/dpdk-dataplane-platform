# 网卡能力画像与规则探测缓存

2026-10-03 最初随 v12 实现，当前管理协议 v15。`dppd` 和 `dppctl` 必须一起更新，v14 及更旧客户端
返回 `EPROTO`。磁盘规则快照仍为 v2，不需要迁移已有 v2 文件。

## 使用方法

```bash
./build/dppctl capability-show 0
./build/dppctl probe-drop 100 0
./build/dppctl probe-drop 100 0 10 refresh
./build/dppctl probe-filter 101 0 tcp 192.0.2.0/24 any any 443 drop count mark:7 priority:10
./build/dppctl probe-filter 102 0 udp any any any 53 queue:0 refresh
./build/dppctl probe-cache-clear 0
```

`capability-show PORT` 读取启动时保存的设备信息和本进程累计的校验记录，不重新访问驱动。
`probe-drop`、`probe-filter` 询问完整候选规则能否通过驱动校验，不要求 ID 已存在，也不
创建、删除或预留规则对象。候选 ID 必须非零。`probe-filter` 的参数顺序是：

```text
RULE_ID PORT ipv4|udp|tcp SRC_CIDR DST_CIDR SRC_PORT DST_PORT drop|queue:N
    [count] [mark:N] [priority:N] [refresh]
```

地址、端口通配使用 `any`，数值端口 `0` 表示精确匹配端口零。IPv4 规则的两个 L4 参数
都必须为 `any`。动作按 MARK、COUNT、最终 DROP/QUEUE 构造；可选动作不可重复。
`refresh` 必须放在最后，表示放弃同一规则的旧答复并重新校验。

探测返回示例，数值仅说明字段：

```text
flow-probe rule=100 port=0 hardware=unsupported software=yes cached=yes hardware-error=-38 epoch=1 age-ms=25
  detail: flow API not supported
```

| 字段 | 含义 |
|---|---|
| `hardware=supported` | 本次完整规则通过了 `rte_flow_validate`，尚未创建对象 |
| `hardware=unsupported` | 驱动返回 `ENOTSUP` 或 `ENOSYS` |
| `hardware=rejected` | 驱动返回 `EINVAL` 或 `EEXIST`，不能据此判定永久不支持 |
| `hardware=unavailable` | 内存不足、设备忙、设备错误等其他答复 |
| `software=yes/no` | 当前平台软件后端是否能等价执行该规则；QUEUE 不会被静默模拟 |
| `cached`、`age-ms` | 是否使用旧答复及答复的年龄；时钟失败时年龄为 `unknown` |
| `hardware-error`、`detail` | 驱动原始返回码及附带说明 |
| `epoch` | 该端口诊断缓存的失效版本，与规则 generation 无关 |

有效诊断请求取得“不支持、拒绝、暂不可用”答复时，CLI 仍以零退出；脚本应检查
`hardware` 字段。非法规则、不存在端口、已知停止或移除端口、恢复隔离等属于请求或
服务错误，CLI 返回失败。探测不会改变事务编号、规则版本、COUNT、软件快照或磁盘文件。

## 画像内容

画像分三行展示，启动信息与校验记录分别保存：

| 内容 | 来源和边界 |
|---|---|
| `device`、`driver`、`dpdk`、`firmware` | 本次启动的设备实例、PMD 和版本；固件无法取得时明确为 `unknown`，保留错误码 |
| `socket`、`kind` | NUMA socket 与 ethdev/representor；原有 `port-show` 继续提供 switch-domain、MAC 和链路信息 |
| `max-rx/max-tx`、RSS、RX/TX offloads | PMD 声明的能力；`queues`、`rss-configured`、`tx-configured` 是实际配置 |
| `rx/tx-desc-min/max/align`、`rx/tx-desc` | 驱动的描述符限制与调整后的实际数量 |
| `validations`、`supported/unsupported/failed` | 实际驱动校验累计次数，包含探测和正式安装校验；缓存命中不增加这些数值 |
| `hits/misses`、`invalidations` | 诊断缓存命中、重新校验和端口缓存清空次数 |
| `cache-entries/capacity`、`ttl-ms` | 当前端口保留的答复槽位、全进程共享上限 64、有效期 5000 ms |
| `observed-domains/items/actions` | 成功校验的完整规则中出现过的 IR 枚举位集合，初始为零 |

位编号对应 `include/dppd/rule.h` 中的枚举值，某个位出现不能推断任意组合都受支持。
失败与不支持答复不会补入成功观察集合；集合只记录本次进程历史，不随清缓存而清零。
`cache-entries` 是仍占用的槽位，过期清理由下一次明确的探测完成，因此只读画像可能
暂时显示已经到期的记录。该数字不代表网卡的规则容量或当前已安装对象数。

固件 API 不支持或返回错误时，不阻止原有端口配置；缓冲区不够时保留 `ENOSPC`，不展示
截断字符串。设备身份、版本和描述符信息仅在配置阶段读取，画像查询只复制本地值。
这符合 [DPDK ethdev 的固件查询返回约定](https://doc.dpdk.org/api-21.11/rte__ethdev_8h.html)。

## 缓存与正式安装的边界

缓存键包含安装端口、完整 ID、domain、group、priority、全部有效 match/spec/mask 和
action 参数。COUNT 编译使用规则 ID，因此不同 ID 不共享答复。generation 和后端偏好
不影响探测语义，硬件校验统一使用 require 偏好；未使用的 union 字段与填充字节不参与比较。
缓存保存完整规则并逐字段比较，不依赖哈希值相同就判断命中。

- 全部端口共享 64 个固定槽位，满时循环替换；缓存本身不在查询时分配堆内存
- 仅成功、`ENOTSUP` 和 `ENOSYS` 答复可缓存，最多五秒，支持强制刷新
- `EINVAL/EEXIST`、内存不足、忙、I/O 或设备错误等不缓存，下一次必须重新询问
- 刷新先删除同键旧答复，临时错误不能让旧成功结果继续生效
- 时钟失败不使用同键旧答复，时间倒退或到期会删除旧记录
- 正式驱动校验先清空该端口的旧结果；任何创建或删除尝试使全部端口结果失效，失败也相同
- 显式清缓存只推进该端口 epoch，不改变规则和累计统计；进程重启建立新缓存

正式安装始终重新调用驱动，既不借用旧成功结果跳过校验，也不因旧负结果拒绝 require
或直接改变 prefer 的选择。纯软件安装不改变驱动资源，因此不会清空其他端口的探测结果。
创建和删除采用全端口失效，是为了保守处理同设备或交换域可能共享的资源。

`rte_flow_validate` 不修改设备；其答复依赖当时配置、已有规则和可用资源，通过校验仍
不保证随后创建成功。缓存只用于短期诊断，不代表安装或容量保证。具体约定见
[DPDK flow 校验说明](https://doc.dpdk.org/guides-21.11/prog_guide/rte_flow.html)。

画像查询在恢复隔离期间仍可用，不调用 PMD、不查询或重置 COUNT、不回收软件快照、
不保存磁盘文件。隔离期间探测和清缓存返回 `EUCLEAN`。已知停止或移除的端口不会进入
驱动探测。所有缓存操作由串行管理线程执行，逐包路径没有增加锁、分配或时钟调用。

当前未提供任意 flow 能力矩阵、AGE/indirect/template/async 或资源容量探测，也未根据
厂商字符串选择后端。静态 queue/RSS/offload 信息与完整规则校验必须分别解读。

## 验证记录

环境：测试机 `.135`，Ubuntu 22.04.5、内核 6.8.0-138-generic、GCC 11.4、DPDK 21.11.9，
独立目录 `/tmp/dppd-capability-validation/source`。只使用 ring/null 和临时 TAP，
管理接口、物理数据接口、地址和绑定不参与本轮修改。

| 检查 | 结果 |
|---|---|
| 全量 `-Werror` 构建、21 项单测 | 通过，包括真实 socket、v11 拒绝、完整键、64 槽替换、分配失败、精确到期、时钟失败与负结果更新 |
| 普通用户 ring 进程 | 静态身份、固件未知、画像只读、命中/刷新/清空/实际过期、require 新校验、prefer 等价降级、无安装或保存副作用、COUNT 与重放通过 |
| 临时双 TAP 进程 | 真实驱动校验与创建/更新/删除、跨端口失效、软件对象共存、重启重新观察与接口回收通过 |
| 规则状态 2/4 条回归 | 只读、整批计时、精确版本、dirty/flush、重放与删除通过 |
| 两组恢复隔离故障 | 画像仍可只读查询，探测与清缓存被阻断，补偿重试与快照重放通过 |
| 健康与线程故障回归 | ring 单队列、null 双队列正常/存储恢复及两组注册故障通过 |
| 四组正式 worker 收发 | 2/4 条规则 × 1/2 队列，每组更新 1001 次；报文内容、COUNT、失败重试及 mbuf 回收通过 |
| AddressSanitizer / UBSan | 探测缓存、CLI 参数、安装状态三个相关单测通过，启用泄漏检查与未定义行为失败退出 |

ring 进程退出后 1024 个 mbuf 全部归还，TAP 进程退出后 8191 个 mbuf 全部归还，临时
接口被移除。TAP 结果证明该 PMD 的真实 flow 流程，不作为物理网卡卸载性能验收。

复现命令：

```bash
meson setup build -Dwerror=true
meson compile -C build
meson test -C build --print-errorlogs
python3 tests/integration/capability_probe.py --build-dir build
sudo python3 tests/integration/capability_probe.py --build-dir build --tap
python3 tests/integration/rule_status.py --build-dir build
python3 tests/integration/recovery_isolation.py --build-dir build --lcores 0,1
python3 tests/integration/health_readiness.py --build-dir build
python3 tests/integration/software_traffic.py --build-dir build

meson setup build-capability-asan -Dwerror=true -Db_sanitize=address,undefined
meson compile -C build-capability-asan -j 8 test_capability test_dppctl test_rule_status
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  meson test -C build-capability-asan --no-rebuild --print-errorlogs \
  'capability profile and probe cache' 'dppctl batch update arguments' 'rule installation status'
```
