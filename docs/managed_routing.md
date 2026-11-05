# 统一管理的双栈路由与静态邻居

本项在 M5 managed daemon 中接入 M2 的 IPv4/IPv6 路由器。规则、模板、共享 COUNT、路由、静态邻居和持久请求使用同一个配置文件与写锁

这是 M5 软件验收之后新增的整合交付。既有 M5 历史报告不作为本项验收结果，实际测试记录见文末

## 启动与恢复

在既有 managed 启动命令中增加 `--managed-routing`：

```sh
build/dppd-flow-state init-software /absolute/path/service.state 0 64 2 64
build/dppd EAL_ARGUMENTS -- --ports 0,1 --queues 1 \
  --flow-state /absolute/path/service.state --managed-routing \
  --control-socket /absolute/path/control.sock
```

`EAL_ARGUMENTS` 必须替换为实际设备、核心和内存参数。测试应使用独立网络命名空间和专用 TAP，不能直接套用到管理网口

首次启用保存版本一的空路由表，并将统一配置修订号加一。空表没有默认出口，报文不能沿旧端口对路径绕过路由查询。应用程序配置网口和启动 worker 之前，先完成配置恢复与端口身份核对

如果随后设备初始化或源 MAC 检查失败，已经保存的路由模式仍会保留，程序不会撤销配置后自动改跑端口对模式。修复环境后继续使用原文件和 `--managed-routing` 重试

重启继续使用相同参数。程序核对完整端口集合、DPDK 设备名、驱动及 TAP 接口名，不因端口编号相同就认为是原设备。启用过路由的文件缺少 `--managed-routing` 时启动失败，避免意外恢复为端口对转发

原 `--ip-routing PATH` / `--ipv4-routing PATH` 仍属于独立路由模式，不能与本模式混用。此轮静态三层模式允许一个或多个端口，不要求端口数为偶数；端口必须具备有效的单播源 MAC

## 配置与版本

以下文件一次描述四张表，未列出的条目会删除，空文件表示清空所有路由与静态邻居：

```text
route 198.51.100.0/24 1 198.51.100.1
neighbor 1 198.51.100.1 02:11:22:33:44:55
route6 2001:db8:2::/64 1 2001:db8:2::1
neighbor6 1 2001:db8:2::1 02:11:22:33:44:55
```

先查询当前版本，再使用长期客户端身份和递增序号提交：

```sh
build/dppctl --json --socket /absolute/path/control.sock routing-show
build/dppctl --socket /absolute/path/control.sock \
  request-submit 81 1 routing-replace /absolute/path/routes.txt EXACT_REVISION
build/dppctl --socket /absolute/path/control.sock request-get 81 1
```

`EXACT_REVISION` 替换为查询返回的 `revision`，不是路由的 `generation`：

| 字段 | 含义 |
| --- | --- |
| revision | 规则、在线定义和路由共用的配置修订号，作为此次修改的并发条件 |
| durable_revision | 已可靠保存的统一修订号，落盘失败时可能落后于当前版本 |
| generation | 路由和静态邻居自身的版本，仅在路由整表更新时变化 |
| routing_generation | 持久请求回执中的路由结果版本 |

只有 `phase=done status=0 applied=yes durable=yes` 才表示此次修改成功且已可靠保存。相同请求键和相同规范内容返回原结果；同键不同内容拒绝，失败后修改参数应使用新的序号

路由和静态邻居共用一次快照发布，worker 不会读到半张表。此操作不包含规则增删改；规则事务与路由事务各自提交，共用修订号防止并发覆盖，不宣称跨两者的逐包原子切换

每个地址族最多 256 条路由、256 条静态邻居。配置在发布前检查前缀、地址族、下一跳、单播 MAC、重复项及出口归属。没有动态 ARP/ND 回退；下一跳缺少静态邻居时按既有数据面语义丢弃

## HTTP 管理

网关沿用既有 TLS、认证文件和 reader/operator/admin 角色。读取使用 `GET /v1/routing` 或 `GET /v1/routing/status`，写入要求 admin：

```json
{
  "operation": "routing_replace",
  "sequence": "1",
  "revision": "1",
  "routes": [{"prefix": "198.51.100.0/24", "port": 1, "next_hop": "198.51.100.1"}],
  "neighbors": [{"port": 1, "address": "198.51.100.1", "mac": "02:11:22:33:44:55"}],
  "routes6": [{"prefix": "2001:db8:2::/64", "port": 1, "next_hop": "2001:db8:2::1"}],
  "neighbors6": [{"port": 1, "address": "2001:db8:2::1", "mac": "02:11:22:33:44:55"}]
}
```

将以上对象发送到 `POST /v1/requests`，该端点目前只接受 `routing_replace`。示例修订号必须换成当前查询结果；客户端身份来自认证配置，序号由客户端长期维护。四个数组都必须显式提供，空数组表示清空对应表；`next_hop: "direct"` 表示直连

HTTP 层按表的语义键排序后生成受控配置文件，数组排列变化不影响同键重试。直接 CLI/C 请求按规范编码比较内容，修改条目顺序也属于不同请求内容

完整路由请求体上限为 256 KiB，其他写接口保持原 64 KiB 门槛。查询返回完整四表；64 位版本以十进制字符串输出，避免 JSON 客户端丢失整数精度

## 持久化和故障

含路由的 `DPPM4CFG` 文件使用 v3。新程序继续读取无路由的 v1/v2；旧程序必须拒绝不认识的 v3，不允许通过丢弃路由或回执把文件降级

持久请求先保存 pending，再构建候选表。全部校验和分配成功后才更新统一版本并发布；随后将实际配置和 done 回执一起原子保存。两次保存分别同步文件和父目录

- 候选校验或分配失败：保持旧表，保存失败终态
- 受理保存失败：不发布路由，保留失败结果并进入 dirty
- 发布后保存失败：保留已生效新表及原请求结果，停止 worker，禁止成功回执
- 修复磁盘后调用 `persistence-flush` 或 HTTP `POST /v1/recovery/flush`：可靠保存后恢复转发，原请求键可以继续查询结果
- 异常重启只恢复文件中完整配置；pending 请求记录为中止，不擅自重做

与既有 managed 语义一致，从发布到发现写盘失败之间可能已有报文使用新表，不能将失败返回解释为完全没有生效。丢失响应时查询原请求，不猜测结果或换序号重复提交

## 验收和剩余范围

2026-11-05，135 主机的独立源码目录 `/home/wq7/dppd-managed-routing-20261105/source` 完成验收：

| 构建 | DPDK | 全量单测 | 新路由正式进程与 TAP |
| --- | --- | --- | --- |
| build-perf | 21.11.9，严格优化 | 95/95 | 六组通过，64 帧 |
| build-asan | 21.11.9，ASan/UBSan | 95/95 | 六组通过，64 帧 |
| build-modern-private | 24.11.8，严格优化 | 95/95 | 六组通过，64 帧 |
| build-modern-asan | 24.11.8，ASan/UBSan | 95/95 | 六组通过，64 帧 |

四套都开启 `tests=true`、`werror=true`、断言。sanitizer 使用 `detect_leaks=0`，不将结果登记为完整 LSAN 验收

每套实际发送 64 帧：40 帧立即转发并逐字节核对，16 帧确认进入 worker 后丢弃，8 帧在故障停转发期间等待，修复后通过 RX/TX 增量确认消费并转发。等待的 8 帧只计一次发送，不重复增加报文总数

六组验收覆盖空表启动、双栈配置、权限和幂等、CLI/HTTP 共用版本、规则 DROP 与路由共存、SIGKILL 恢复、错误端口身份/模式拒绝、发布后写盘失败停转发及修复、一个与三个端口的启动和退出

21.11.9 与 24.11.8 两套优化构建另外通过 `verify_m5.py` 的既有 M2 核心、M3、M4、进程恢复、正式 daemon 和持久请求回归，以及 `m5_northbound.py` 的四阶段 HTTP/TLS 回归。net_ring 默认源 MAC 无效的拒绝路径单独通过，不作为路由正向报文证据

重复入口如下，TAP 必须在独立网络命名空间中运行：

```sh
meson test -C build-perf --print-errorlogs
sudo env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" unshare --net \
  python3 tests/integration/managed_routing.py --build-dir build-perf \
  --report-dir build-perf/managed-routing-tap --tap
python3 scripts/verify_m5.py --build-dir build-perf --report-dir build-perf/previous-m5-ring
python3 tests/integration/m5_northbound.py --build-dir build-perf --report-dir build-perf/previous-northbound-ring
```

使用其他构建时需替换目录并设置对应 SDK。24.11 首次回归发现 `find_library` 缓存选中了系统 21.11 的 ring PMD，导致同一进程混载两版 DPDK 并在 EAL 初始化时失败。补齐私有 SDK 的链接搜索和 rpath、清除 Meson 依赖缓存后，独立核对实际动态库并重跑了受影响构建的单测、TAP 和旧功能回归。首次失败证据保留，未降低测试条件或修改业务逻辑来绕过

全部报告对应同一份 347 项源码，按路径排序的摘要表 SHA256 为：

```text
8e88495e17980dd3904af6b72c9e8e17743d6264f8db0bbfe8cc2ffe6e86dbb7
```

本地摘要和原始证据保存在 `build/native-check/managed-routing/summary.json` 与 `host-135-evidence.tar.gz`，归档 SHA256 为 `21b2ee2d6c9cbc921c7ba4ebac9de5ddc54bdd7274397eb732381cbf3209ca07`。该目录被 Git 忽略，提交中保留的是实现、可重复入口和本说明

本轮没有重跑原八项性能门禁、发布包双目录复现、完整十二步 M2 TAP 或 134 主机验收，不据此扩大原阶段报告

本项验收范围不包括连接跟踪/NAT 策略、动态邻居配置、会话持久化、硬件卸载或真实设备复位
后续 [managed CT/NAT](managed_conntrack_nat.md) 已单独接入连接策略管理，其他限制仍保留，不能据此宣称全部功能已统一管理
