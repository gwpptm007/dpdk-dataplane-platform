# 统一管理的连接跟踪与 NAT 策略

本项把 M2 的双栈 TCP/UDP 连接跟踪、出站 SNAT、静态 DNAT 和端口转发接入正式 managed daemon
规则、模板、共享 COUNT、路由、静态邻居和 CT/NAT 策略使用同一份 `flow-state` 文件、文件写锁和全局修订号
CLI 与 HTTP 共用持久请求回执，重复请求不会再次替换策略或再次清空连接

## 启动与恢复

沿用 [统一路由模式](managed_routing.md) 的启动方法：`--flow-state FILE --managed-routing`
先配置实际路由和静态邻居，再在线提交 CT/NAT 策略
旧文件仍可读取，首次提交 CT 策略请求时升级为第四版文件，管理客户端和 daemon 需同时升级到协议 v32
第四版文件不能交给旧程序读取，降级前应保留升级前的配置备份

启动参数 `--conntrack`、SNAT/DNAT 参数仍属于旧独立模式，不能与 managed 模式混用
managed 模式只从统一文件恢复策略，避免启动参数和在线配置各自保存一份不同的策略
首次启用统一路由时 CT 默认关闭，常驻的空容器不会额外要求普通转发报文通过 TCP/UDP 校验

重启恢复容量、超时和全部 NAT 规则，活跃会话、已占用的动态端口和计数从空表重新建立
启动标识每次重新生成，旧的会话清理请求不能用于新进程

## 三种版本各自保护什么

| 字段 | 用途 | 什么时候变化 |
| --- | --- | --- |
| `revision` | 保护整个 managed 配置，防止拿旧配置覆盖别人的修改 | 规则、定义表、路由或 CT/NAT 策略成功发布 |
| `generation` / 回执 `conntrack_generation` | 标识当前 CT/NAT 策略 | 整份 CT/NAT 策略成功替换 |
| `session_epoch` 与 `session_revision` | 精确定位本次启动和会话表，保护分页与清理 | 重启更换 epoch，会话增删或策略替换推进 revision |

查询 `conntrack-policy-show` 或 `GET /v1/conntrack/policy`，取得本次提交需要的三个检查值
查询之后如果又有连接建立、过期或被清除，提交可能返回 `ESTALE`，应重新查询再决定是否提交新的请求
正常报文计数和已有连接内的状态推进不等同于会话集合变化，不能把分页看成冻结的逐包统计快照

## 更新策略与活跃连接

每次提交完整策略，容量范围为 1～65536，空闲超时范围为 1～86400 秒
最多各一组 IPv4、IPv6 SNAT，以及合计 32 条双栈 TCP/UDP DNAT
SNAT 地址族分别指定，DNAT 数组保留声明顺序，该顺序就是会话中显示的规则编号
所有 NAT 入口、出口必须已绑定到当前 managed 实例，端口编号相同并不能绕过设备身份检查

`reset_sessions` 必须显式填写，提交 `false` 时有任何活跃连接就返回 `EBUSY`，原策略和映射保持可用
确实需要更换运行中的策略时，提交 `reset_sessions=true`，同时携带精确的启动标识、会话版本和全局版本
成功后旧连接全部清空，新连接按新策略建立，现有 TCP 通信可能因此中断
即使提交内容与当前策略相同，也属于一次完整替换，不会隐式迁移或保留旧会话

程序先校验并准备完整候选表，之后锁住当前表、核对版本并发布
分配失败、端口不存在、版本过期和默认拒绝清会话均发生在发布前，不会清除旧映射
表容器地址始终不变，worker 借用该地址，在线启停不会留下已释放的表指针

关闭 CT 时提交 `capacity=0`、`timeout_s=0`、`snat=null`、`snat6=null`、`dnat=[]`
有活跃连接时同样需要明确选择清空，不能通过关闭功能绕过保护

## CLI 示例

先查询配置版本与会话检查值：

```sh
dppctl --socket /run/dppd/control.sock --json conntrack-policy-show
dppctl --socket /run/dppd/control.sock --json conntrack-status
dppctl --socket /run/dppd/control.sock --json conntrack-list 0
```

例如 `nat.conf` 保存以下完整策略，端口 0 为内侧、1 为外侧：

```text
conntrack 4096 300
snat 0 1 203.0.113.10 40000 49999
snat6 0 1 2001:db8:ffff::10 40000 49999
dnat tcp 1 0 203.0.113.10 8080 8080 10.0.0.10 80
dnat6 tcp 1 0 2001:db8:ffff::10 8080 8080 fd00:1::10 80
```

SNAT 字段顺序为内口、外口、公网地址、端口池起止
DNAT 字段顺序为协议、外口、内口、公网地址、公网端口起止、内网地址、内网起始端口
整地址 DNAT 用 `0 65535` 映射到 `0`，端口段映射按相同偏移逐个对应

```sh
# 这些变量必须填本次查询的实际值，每个新请求使用递增的 SEQUENCE
dppctl --socket /run/dppd/control.sock --json request-submit "$CLIENT" "$SEQUENCE" \
  conntrack-replace nat.conf "$REVISION" false "$SESSION_EPOCH" "$SESSION_REVISION"

# 重试和查询沿用原客户端标识及序号，不能改内容后复用同一个键
dppctl --socket /run/dppd/control.sock --json request-get "$CLIENT" "$SEQUENCE"

# 继续翻页时传回上一页的版本和启动标识
dppctl --socket /run/dppd/control.sock --json conntrack-list "$NEXT_ID" "$SESSION_REVISION" "$SESSION_EPOCH"

# 仅清理运行态连接，不改变策略或持久配置版本
dppctl --socket /run/dppd/control.sock --json conntrack-flush "$SESSION_REVISION" "$SESSION_EPOCH"
```

## HTTP 接口

| 接口 | 权限 | 内容 |
| --- | --- | --- |
| `GET /v1/conntrack/policy` | reader 及以上 | 完整策略、策略代次、全局版本与会话检查值 |
| `GET /v1/conntrack/status` | reader 及以上 | 容量、活跃数、累计计数与 NAT 状态 |
| `GET /v1/conntrack/sessions` | reader 及以上 | 会话分页，包括原始与转换后五元组 |
| `POST /v1/conntrack/sessions/flush` | operator 及以上 | 精确清理会话，需要 `epoch`、`revision` |
| `POST /v1/requests`，`operation=conntrack_replace` | admin | 持久替换完整策略 |

示例请求体如下，其中检查值应替换为当前查询结果
所有 64 位标识和版本使用十进制字符串，客户端身份来自认证凭据，不能在请求体中冒用其他客户端

```json
{
  "operation": "conntrack_replace",
  "sequence": "12",
  "revision": "105",
  "reset_sessions": false,
  "expected_session_epoch": "123456789",
  "expected_session_revision": "3",
  "policy": {
    "capacity": 4096,
    "timeout_s": 300,
    "snat": {
      "inside_port": 0,
      "outside_port": 1,
      "address": "203.0.113.10",
      "first_port": 40000,
      "last_port": 49999
    },
    "snat6": null,
    "dnat": [{
      "family": "ipv4",
      "protocol": "tcp",
      "outside_port": 1,
      "inside_port": 0,
      "public_address": "203.0.113.10",
      "public_first": 8080,
      "public_last": 8080,
      "private_address": "10.0.0.10",
      "private_first": 80
    }]
  }
}
```

必须同时检查响应中的 `data.status`、`data.applied` 和 `data.durable`，HTTP 200 本身不表示策略成功
例如活跃连接保护可以形成已可靠保存的失败回执，外层 `status=0`，而 `data.status=-16`（EBUSY）、`data.applied=false`、`data.durable=true`
同键同内容返回原回执，同键不同内容返回 `EEXIST`，已淘汰的旧序号返回 `ESTALE`
会话 flush 只修改易失表，没有持久请求键；响应丢失后应重新查询会话状态，不能换成最新版本后盲目再清一次

## 失败、恢复与边界

受理记录先落盘，才允许修改运行态
应用成功后，完整配置和终态回执一起落盘；这一步失败时可能已有少量在途报文使用新策略
daemon 按既有 managed 机制停止 worker，返回 `applied=true`、`durable=false`，不能把错误理解为旧会话仍在
修复存储后执行 `persistence-flush`，可靠保存成功才恢复转发
若最终配置未落盘就崩溃，重启仅恢复最后可靠配置，把残留 pending 请求标为 `ECANCELED`，不重新执行清空动作

本项把配置和请求管理统一起来，路由、规则、CT/NAT 仍是独立请求，不提供三者一次跨对象原子提交
不迁移活跃连接，不保存会话，不提供 HA 同步、hairpin NAT、NPTv6、RELATED/ICMP 转换或硬件有状态卸载
动态 ND 策略尚未接入 managed 配置，原独立模式仍保留已有 ND 能力

## 验收入口

```sh
meson test -C build-perf --print-errorlogs
sudo unshare --net --mount-proc \
  python3 tests/integration/managed_nat.py --build-dir build-perf \
  --report-dir build-perf/managed-nat-tap --tap
```

2026-11-05，在 135 主机隔离目录 `/home/wq7/dppd-managed-nat-20261105/source` 清理产物后完整重建并验收

| DPDK 与构建 | 单测 | 新 managed CT/NAT 三 TAP |
| --- | --- | --- |
| 21.11.9 优化版 | 96/96 | 8 组通过 |
| 21.11.9 ASan/UBSan | 96/96 | 8 组通过 |
| 24.11.8 优化版 | 96/96 | 8 组通过 |
| 24.11.8 ASan/UBSan | 96/96 | 8 组通过 |

每套三 TAP 均比对 52 个完整转发帧，确认 8 个实际进入 worker 的拒绝报文，另验证 1 个暂停期间缓存、恢复后处理的报文
覆盖双栈 SNAT、DNAT、整地址和端口段映射、TCP 握手、静态端口保留、池耗尽、错误入口、会话分页和清理
同一流程还验证 CLI/HTTP 一致、权限、重复请求、旧版本与旧 epoch 拒绝、SIGKILL 后策略恢复，以及发布后 fsync 失败停转发和修复恢复
单测补充候选分配失败、并发替换、满额 32 条双栈规则、非法文件编码、三个写盘失败位置和 pending 请求重启取消

四套均启用 `werror` 和断言，Sanitizer 使用 `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1` 与 `UBSAN_OPTIONS=halt_on_error=1`
因此本记录不宣称完成 LeakSanitizer 泄漏检查，也不替代吞吐或控制面延迟基准
351 个源码文件一致，源码摘要为 `40cb3850c85c726ece991e53f78e5b7dcecb320ce5f418757a496e9aa2f1cb0b`

两套优化版分别通过既有 managed-routing 三 TAP、M5 和 HTTP/TLS 回归，以及旧独立模式 CT/SNAT、DNAT、NAT66 三 TAP 回归
旧独立模式脚本没有单独输出源码摘要，归档记录其在同一冻结源码目录执行的命令、退出码和日志校验值
本轮清理重建后，四套 Meson 单测均无跳过，Python 子测试也无内部跳过；24.11.8 二进制的动态库来源已单独核对

摘要与原始证据保存在被 Git 忽略的 `build/native-check/managed-nat/summary.json` 和 `host-135-evidence.tar.gz`
归档 SHA256 为 `973e16e7f3cf2a7e9f2d455c74777fd08b12b9f6bf7024dcfa8111b1652c7e25`
提交保留实现、验收入口与本说明，另用 `commit-verification.json` 记录实际提交与 351 项验收源码的逐文件核对
本轮不包含 134 主机、物理网卡卸载、真实硬件复位、原八项性能门禁或发布包双目录复现
