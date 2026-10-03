# 规则安装状态与提交耗时

2026-10-03 实现，管理协议 v11。`dppd` 和 `dppctl` 必须一起更新，v10 及更旧客户端
返回 `EPROTO`。磁盘规则快照仍为 v2，格式没有改变。

## 使用方法

```bash
./build/dppctl rule-status 100
./build/dppctl rule-status 100 any
./build/dppctl rule-status 100 7
```

不指定版本或使用 `any`，查询该 ID 当前发布的版本。指定精确非零版本可检测并发更新：
规则已更新时返回 `ESTALE`，ID 不存在时返回 `ENOENT`。版本零、非法 ID 和额外参数被拒绝。
规则没有 COUNT 动作也能查询安装状态。

示例输出中的数值仅用于解释字段：

```text
rule-status rule=100 generation=7 port=1 backend=software fallback=prefer-hardware count=no install-scope=batch commit-rules=2 install-ns=18000 persistence=enabled dirty=no persisted-generation=8 repository-generation=8 last-error=0
```

| 字段 | 含义 |
|---|---|
| `rule`、`generation`、`port` | 当前安装对象的 ID、规则版本和 DPDK 安装端口 |
| `backend` | 实际对象所在的 `software` 或 `rte_flow` 后端 |
| `fallback` | 用户要求的后端策略，与实际后端分别显示 |
| `count` | 规则是否配置 COUNT，不表示已查询计数器或已有命中 |
| `install-scope`、`commit-rules` | 本次提交安装单条规则，还是一次发布的整个软件更新批次 |
| `install-ns` | 后端成功提交的测量耗时，单位纳秒；时钟不可用时显示 `unknown` |
| `persistence`、`dirty` | 整个规则账本是否启用保存、最近保存是否失败 |
| `persisted-generation`、`repository-generation` | 整个账本的磁盘版本和当前内存版本 |
| `last-error` | 最近保存错误，零表示没有保存错误 |

`prefer-hardware` 表示优先尝试驱动，不证明实际使用了驱动对象。查询分别检查两个后端的
精确 ID/版本，必须恰好找到一个对象；还核对安装端口、COUNT 配置及后端策略。驱动对象的
handle 身份与记录不符、两个后端都有同一版本、或者都没有对应对象时，返回 `EUCLEAN`，
不返回可当作成功使用的状态。

## 耗时边界

- `rte_flow` 测量成功的创建调用，不包含先前的驱动校验
- 单条软件安装测量副本分配、发布和当次可完成的退役回收
- 纯软件批量更新测量整批副本分配和一次发布，全批共享同一个耗时
- 不包含规划、prepare、规则账本提交、磁盘保存或管理 socket 往返

批量成员的 `commit-rules` 为批次条数，不能将成员耗时相加或理解为各自的单条耗时。
批量创建、硬件更新和混合更新仍逐条调用后端提交，因此对应记录的范围为单条。
计时只发生在控制面提交阶段，逐包处理路径没有增加时钟调用或锁。

同一版本随着其他规则变更而复制软件快照时保留原记录。单规则幂等 apply 不重装，也不
刷新耗时。纯软件更新发布前失败时，旧规则、COUNT 和安装记录全部保留。
安装新版本或补偿重建旧对象会测量新的实际提交，不继承已被删除对象的记录或历史 COUNT。

计时使用单调时钟，时钟失败只使统计不可用，不改变规则安装结果。安装记录不写入磁盘
快照；重启重放保留规则 ID、generation 和端口，重新测量本次安装。重放逐条提交，因此
上个进程的批量耗时不会被沿用。

## 只读语义和限制

查询不调用 PMD、不查询或清零 COUNT、不改变事务编号、规则版本、活跃表或退役表，也不
保存或修复磁盘快照。保存失败且进程仍处于 READY 时可以查看 `dirty=yes`，随后由
`persistence-flush` 修复。恢复隔离期间仍拒绝普通规则查询，使用 `health/ready` 和
`reconcile-status/reconcile-retry` 查看与处理隔离状态。

返回的安装状态来自本进程的控制面记录，不证明物理硬件卸载、设备当前可达、报文命中或
端到端转发成功。没有后台持续探测驱动对象，也没有保存失败安装的耗时直方图或错误分类。
规则状态尚未接到 DPDK telemetry；本轮提供本机管理查询接口。

## 验证记录

环境：Ubuntu 22.04.5、GCC 11.4、DPDK 21.11.9，独立测试目录
`/tmp/dppd-rule-status-validation/source`。管理网卡和物理数据网卡均未修改。

| 检查 | 结果 |
|---|---|
| 全量 `-Werror` 构建 | 通过 |
| 20 项单测 | 全部通过，含真实 socket、旧协议拒绝、身份失配、计时失败和分配失败 |
| 普通用户运行的 2/4 条规则场景 | 只读、prefer 降级、整批耗时、旧版本拒绝、dirty/flush、重放和删除通过 |
| 临时双 TAP 的真实 rte_flow | 创建、更新端口、删除、软件与驱动对象共存、只读及重放通过 |
| 正式 worker 的四组 RX/TX | 2/4 条规则 × 1/2 队列，每组成功更新 1001 次，COUNT 和内容检查通过 |
| 在线补偿失败的两组隔离回归 | 新增状态查询同样被阻断，清理重试和快照重放通过 |
| health/ready 回归 | ring/null/TAP、两组线程故障和 TAP 链路恢复通过 |

四组 worker 分别接收 24144、48384、72432、145152 个报文；每组 2048 个 mbuf 全部归还。
状态查询没有重置最终 COUNT，四个和六个软件分配失败位置均保留原安装记录。ring 进程
退出后 1024 个 mbuf 全部归还，双 TAP 进程退出后 8191 个 mbuf 全部归还，临时接口被清理。
TAP 结果证明该 PMD 的真实 `rte_flow` 生命周期，不作为物理网卡卸载性能验收。

复现命令：

```bash
meson setup build -Dwerror=true
meson compile -C build
meson test -C build --print-errorlogs
python3 tests/integration/rule_status.py --build-dir build
sudo python3 tests/integration/rule_status.py --build-dir build --tap
python3 tests/integration/software_traffic.py --build-dir build
python3 tests/integration/recovery_isolation.py --build-dir build --lcores 0,1
sudo python3 tests/integration/health_readiness.py --build-dir build --tap
```
