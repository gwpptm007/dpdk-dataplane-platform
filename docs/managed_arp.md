# 动态 ARP 与统一管理

本项在正式 `--flow-state FILE --managed-routing` 模式提供 IPv4 动态邻居解析
它回答的是“路由已经选好了出口和下一跳，下一跳的 MAC 是什么”
路由仍由管理员配置，静态邻居仍然优先；只有静态 MAC 缺失时，才向对应出口发送 ARP

ARP、本机 IPv6 ND、CT/NAT、路由和规则共用同一份配置文件与全局修订号
本机地址、VLAN 和计时参数持久保存，动态邻居、地址冲突检测进度、等待报文、设备 MAC 与会话不落盘
协议升级到 **v34**，daemon 与 dppctl 必须一起升级
首次受理 ARP 配置请求时使用第六版 flow-state；新程序仍能读取 v1～v5，旧程序不能读取 v6

## 报文怎样走

1. ACL 和路由先决定是否允许转发、从哪个端口出去
2. 查到静态邻居就使用静态 MAC，不让动态学习覆盖静态配置
3. 缺静态邻居时，按出口、下一跳和配置的 VLAN 域查询 ARP 缓存
4. 缓存没有 MAC 就发送 ARP 请求，把少量首包暂存在有上限的等待队列
5. 收到有效响应后，等待包回到原 worker，重新检查当前 ACL、路由和 NAT，再真正转发

等待期间不提交新的 CT/NAT 会话；策略变化、邻居失败或等待超时后释放报文
每个邻居最多等待 4 个包，全局最多 128 个，首次等待期限为 5 秒，重试不会延长期限
动态缓存最多 256 个邻居，最多配置 32 个本机地址

## 地址冲突与邻居刷新

本机地址启用后先进入 `tentative`，按 [RFC 5227](https://www.rfc-editor.org/rfc/rfc5227) 的探测与通告时序执行检测
初次随机等待 0～1 秒，发送 3 次源 IP 为零的探测，相邻探测间隔 1～2 秒
最后一次探测后再等 2 秒，发送 2 次地址通告，间隔 2 秒；第一次通告成功后地址变为 `preferred`
本机探测发送失败不推进次数，会继续等待并重试

同一端口所有已配置 IPv4 地址都可用后，该端口 IPv4 才允许转发
检测期间收到另一个 MAC 声明相同地址，或收到针对该地址的并发探测，会进入 `duplicate`
已启用地址后来发生冲突时也直接停止使用，不执行主动防御或自动换地址
`ready` 会显示 `arp-not-ready`，worker 继续运行以处理检测报文；IPv6 不受 ARP 地址门禁影响
清空邻居缓存不能解除冲突，需要修正地址或明确替换策略后重新检测

邻居响应只用于完成已经发起的解析或刷新，不允许未经请求的广播凭空创建大量缓存或覆盖已确认 MAC
以太网源 MAC 必须与 ARP 的发送者 MAC 一致，并核对硬件类型、协议类型、地址长度、操作码、目标和 VLAN 域
ARP 报文结构依据 [RFC 826](https://www.rfc-editor.org/rfc/rfc826)，这里的缓存老化策略属于项目自身设计

缓存状态为 `incomplete → reachable → stale → delay → probe → failed`
可达有效期由 `reachable_ms` 控制，过期后成为 `stale`；再次使用才安排 5 秒后的刷新
刷新期间沿用旧 MAC，并以 1 秒间隔最多广播请求 3 次，允许下一跳换网卡后返回新 MAC
无应答则停止提供 MAC，冷却 5 秒后由新流量重新发起解析，闲置 60 秒的 stale/failed 条目可回收
邻居查询的发送失败消耗重试预算，避免设备异常时无限占用等待包

## 配置与版本保护

每次替换完整 ARP 策略，字段为 `reachable_ms` 和 `locals`
启用时计时范围为 1000～3600000 毫秒；关闭时两个字段必须同时变为 `0` 和 `[]`
地址拒绝首字节为 0、127 或不小于 224 的 IPv4 地址，不支持子网广播识别，因为本机地址配置没有前缀长度
同一端口允许多个不同本机地址，但只允许一个 VLAN 域，支持无标签、单标签和双标签，VID 为 1～4094
单标签使用 802.1Q，双标签使用外层 802.1ad、内层 802.1Q
保留地址声明顺序，同端口第一个地址作为邻居查询的源地址

| 字段 | 含义 |
| --- | --- |
| `revision` | 全局配置版本，其他对象的更新也可能使它变化 |
| `generation` / 回执 `arp_generation` | ARP 策略自身的代次 |
| `neighbor_epoch` + `neighbor_revision` | 本次启动身份及 ARP 运行态版本，与 ND 缓存独立 |

更新策略和清缓存前，正式管理入口先停止并等待 worker，防止已经取出的等待包跨越策略替换
`reset_neighbors=false` 时，只要存在邻居或等待包就返回 `EBUSY`
明确设为 `true` 才允许回收它们；任何成功的策略替换都会重新进行地址冲突检测
无论是否重置 ARP，都保留 CT/NAT 会话和 ND 配置、缓存
这不是无损切换：更新有短暂暂停，已配置 IPv4 端口还需要重新等待地址检测

查询和提交之间缓存可能变化，`ESTALE` 表示版本已经过期
重新查询后，应重新判断是否同意清理，再用新的请求序号提交
相同客户端和序号、相同内容的重试只返回原回执，不再清缓存或重做检测；同键不同内容被拒绝
写盘失败时区分“已应用”和“已可靠保存”，保持停止转发，修复存储并执行 `persistence-flush` 后恢复

## CLI 示例

```sh
dppctl --socket /run/dppd/control.sock --json arp-policy-show
dppctl --socket /run/dppd/control.sock --json arp-status
dppctl --socket /run/dppd/control.sock --json arp-list 0
```

策略文件 `arp.conf`：

```text
arp 30000
local 0 192.0.2.1
local 1 198.51.100.1
local 1 203.0.113.9
```

其中每条 `local` 是平台自己拥有并愿意回答 ARP 的地址，不能填写对端下一跳地址
如果需要应答 NAT 公网地址，也要显式加入该地址；配置 NAT 本身不会自动声明地址
带 VLAN 时在地址后追加 VID，例如 `local 1 198.51.100.1 37 38`

从 `arp-policy-show` 读取当前全局版本与缓存身份，再提交：

```sh
dppctl --socket /run/dppd/control.sock request-submit CLIENT SEQUENCE \
  arp-replace arp.conf GLOBAL_REVISION false ARP_EPOCH ARP_REVISION
```

以上大写字段是需要替换的实际值，序号由调用方单调递增管理
`arp-policy-show` 的普通文本输出可直接保存为策略文件再次使用，注释行中的版本仍须单独核对

分页和清缓存：

```sh
dppctl --socket /run/dppd/control.sock --json arp-list AFTER ARP_REVISION ARP_EPOCH
dppctl --socket /run/dppd/control.sock arp-flush ARP_REVISION ARP_EPOCH
```

后续页必须携带同一版本和启动身份，清缓存也必须精确匹配
清缓存不改持久策略，不重新执行地址检测，也不清 CT/NAT 会话

## HTTP

| 方法与路径 | 权限 | 用途 |
| --- | --- | --- |
| `GET /v1/arp/policy` | reader | 策略、全局版本、策略代次及缓存身份 |
| `GET /v1/arp/status` | reader | 地址检测状态、缓存占用及 ARP 事件计数 |
| `GET /v1/arp/neighbors?after=0` | reader | 缓存第一页，后续带 epoch/revision |
| `POST /v1/arp/neighbors/flush` | operator | 请求体包含 epoch、revision |
| `POST /v1/requests` | admin | 持久替换 ARP 策略 |

替换请求示例，版本值应来自刚读取的策略，不能直接照抄：

```json
{
  "operation": "arp_replace",
  "sequence": "10",
  "revision": "8",
  "reset_neighbors": false,
  "expected_neighbor_epoch": "123456789",
  "expected_neighbor_revision": "1",
  "policy": {
    "reachable_ms": 30000,
    "locals": [{"port_id": 0, "address": "192.0.2.1", "vlans": []}]
  }
}
```

身份和版本使用十进制字符串，避免前端丢失 64 位整数精度
ARP 状态里的 `requests/replies` 统计实际成功提交发送的控制报文；队列事件计数可能包含同一首包的多次重试
全局 `arp_rx_packets/arp_tx_packets/arp_queued/arp_queue_drops/arp_not_ready_drops` 用于观察处理路径

## 边界

- 仅接入 managed-routing 软件数据面，没有独立启动参数，也未声明物理网卡或 DPU 卸载验收
- 本机地址配置不自动增加路由；不实现 DHCP、代理 ARP、自动地址分配或 IPv4 分片重组
- ARP 缺少认证，格式校验和限制学习范围不能提供防欺骗保证
- 地址冲突采取直接撤回，不实现完整的重复冲突速率限制、自动退避和地址防御策略
- 同一文件不代表跨对象同时提交：ARP、ND、路由、ACL、CT/NAT 仍是独立事务
- 重启重新检测与学习，不恢复动态邻居、等待包或已有连接，不提供跨主机会话迁移
- 尚未验证物理环境的吞吐、极端并发和真实设备移除，不扩大既有 M3/M5 验收范围

验收脚本为 `tests/integration/managed_arp.py`，核心状态机、资源所有权和文件兼容测试位于 `tests/unit/test_arp.c`、`test_flow_arp.c`


## 2026-11-11 验收记录

在 135 主机的隔离源码目录，以同一份 363 项测试源码完成以下验收
所有构建清理后重新编译，启用 tests/werror 和断言，独立核对实际运行的 DPDK 库

| 构建 | 测试 | 新 ARP 三 TAP |
| --- | --- | --- |
| DPDK 21.11.9 优化版 | 100/100，无跳过 | 10 组、41 个数据包 |
| DPDK 21.11.9 ASan/UBSan | 100/100，无跳过 | 10 组、41 个数据包 |
| DPDK 24.11.8 优化版 | 100/100，无跳过 | 10 组、41 个数据包 |
| DPDK 24.11.8 ASan/UBSan | 100/100，无跳过 | 10 组、41 个数据包 |

每套新 TAP 的 41 个数据包中，25 个完成整帧转发检查，16 个确认丢弃，控制 ARP 帧单独核对
覆盖真实 CLI/HTTP、地址探测与通告、NAT 首包及回包、坏报文、静态优先、精确版本、
三次刷新重试、队列溢出与超时、ACL 重查、地址冲突、单层 VLAN、CLI 导出回读、
ARP/ND 共存、SIGKILL 恢复、发布后 fsync 失败、显式保存恢复和 dirty 退出前归还等待包
单测另覆盖双标签完整 ARP 格式、256 项容量与分页、发包失败、候选发布失败、v5→v6 与旧文件兼容

两套优化版均通过既有 managed ND 11 组、managed NAT 8 组、managed 路由 6 组、
M5 的 4 步及其嵌套 M2/M3/M4 软件回归、HTTP/TLS 4 组和独立模式 ND 报文回归
ASan/UBSan 未报告错误；使用 detect_leaks=0，不宣称完成 LeakSanitizer 泄漏验收
现代构建实际使用私有 DPDK 24.11.8，未混用系统 21.11.9 的核心库或 PMD
验收进程全部退出，没有遗留占用 TAP 的后台服务

源码摘要为 `2add2a542418bb790793e76b2785d8a67fe72c415aed249bce680f7ccb0d57bc`
含嵌套验收的 28 份 report.json 均成功并核对相同源码，二进制摘要与实际文件一致
原始报告和日志保存在本地忽略目录 `build/native-check/managed-arp/`
汇总为 `summary.json`，归档为 `host-135-evidence.tar.gz`，归档 SHA256 为 `72d26d34260a8fba4657b91e64c3b89bc2eefd4c615223960598a6c3340b5e6b`

本次未在 134 主机重跑，也未重做完整性能门禁和发布包可复现验收，物理网卡/DPU 验收仍未执行
