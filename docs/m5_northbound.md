# M5 北向 HTTP/JSON v1 与权限边界

本接口由 `app/dppd-api/main.py` 提供，使用 Python 3 标准库，经同发布包的
`dppctl --json` 访问正式 `dppd --flow-state` 实例。网关不直接打开状态文件，也不创建第二个
classifier 写入者。本机 C ABI 只存在于配套 CLI 与 daemon 之间，外部客户使用明确版本的 JSON。

## 启动与部署

默认部署采用独立 Unix socket，文件权限为 `0600`，服务端不会删除已有 socket。与 daemon
使用同一专用服务用户，确保该用户可访问 `control.sock` 与认证文件。

```sh
python3 /opt/dppd/current/libexec/dppd-api.py \
  --socket /var/lib/dppd/run/control.sock \
  --dppctl /opt/dppd/current/bin/dppctl \
  --tokens /var/lib/dppd/etc/api-tokens.json \
  --unix /var/lib/dppd/run/api.sock
```

如需 TCP，显式传 `--listen 127.0.0.1 --port 8443`。非回环 IP 必须同时提供
`--tls-cert certificate.pem --tls-key private.key`，最低 TLS 版本为 1.2。远程客户端必须验证
证书链与服务器名称，不能关闭校验。接口不支持明文的非回环监听、CORS 或 Cookie 登录。

服务守护器分别监管 daemon 与网关。网关每次请求重新执行配置路径下的 CLI，因此可以在
daemon 重启期间保持运行，也能使用 `current/bin/dppctl` 的新发布版本。daemon 尚未恢复或
本机传输失败时返回 `503`；网关不会自动重复一次有副作用的请求。升级网关代码仍应重启网关。

## 凭据与角色

认证文件只保存随机令牌的 SHA-256，与 `Bearer` 令牌分开发放。文件必须是当前服务用户拥有、
没有硬链接和符号链接的普通文件，权限不得开放给组或其他用户。凭据只在启动时读取，轮换后
重启网关。打开文件时使用非阻塞模式，FIFO 等非普通文件会立即拒绝，不等待其他进程写入。
最多 64 条凭据，同一客户端可以配置新旧两枚令牌实现有重叠窗口的轮换。

```json
{
  "version": 1,
  "tokens": [
    {"sha256": "替换为64个小写十六进制字符", "role": "reader", "client": "1"},
    {"sha256": "另一枚令牌的SHA256", "role": "operator", "client": "2"},
    {"sha256": "管理员令牌的SHA256", "role": "admin", "client": "3"}
  ]
}
```

用 `secrets.token_hex(32)` 等密码学随机源生成令牌，通过受控渠道保存和发放，禁止把真实令牌
写入源码、发布包或命令行参数。每个 HTTP 请求必须携带 `Authorization: Bearer TOKEN`，健康
检查也需要认证。日志记录时间、角色、客户端 ID、HTTP 状态与耗时，持久回执另带序号和操作类别，
不记录认证头、URL 或请求体。

网关审计日志采用 **best-effort** 交付，不承诺持久审计。请求线程只向最多 128 条、每条最多
1024 字节的内存队列投递，由独立守护线程写 stderr。日志管道满或消费者退出不会占用请求槽，
也不会阻止 SIGTERM 退出；队列满、写入失败和退出时未发送的队列项会计入丢弃数。
`/v1/health` 与 `/v1/ready` 的 `data.gateway` 包含 `audit_dropped`、`audit_queue_depth`、
`audit_sink_failed`，便于发现采集故障。策略请求的持久回执仍由 daemon 状态文件可靠保存，
不依赖该日志队列。

| 角色 | 允许操作 |
|---|---|
| reader | 健康、就绪、配置、规则、计数、统计和本客户端的持久请求结果 |
| operator | reader 权限，以及精确版本的规则创建、更新、删除 |
| admin | operator 权限，以及模板/共享 COUNT 配置事务与显式恢复 |

`client` 与凭据绑定，HTTP 请求不能指定或冒用其他客户端 ID。所有角色都能查看全局策略，
此版本不提供多租户策略隔离。持久请求序号由同一客户端协调分配，不能让多台调用方独立使用
相同序号。daemon 的 Unix 管理 socket 属于受信任本机管理边界，必须保持 `0600`。

## 版本、类型与错误

所有端点以 `/v1/` 开头，JSON 有 `api_version: "v1"`。成功响应与 daemon 业务错误采用：

```json
{"api_version":"v1","status":0,"data":{}}
```

`status` 是本机管理操作的 Linux `-errno`，0 表示该次查询或受理成功。持久请求另有
`data.status` 表示被查询任务的业务结果，不能只检查 HTTP 200 就认为原写入成功。
认证、JSON 校验、传输或网关错误采用：

```json
{"api_version":"v1","error":{"code":"authentication_required"}}
```

主要 HTTP 状态：`200` 查询/完成、`202` 持久任务待完成、`400` 参数无效、`401` 未认证、
`403` 权限不足、`404` 未找到、`409` 版本冲突/幂等键冲突/已过期序号、`422` 当前模式不支持、
`503` 后端不可用或隔离、`504` 后端超时。队列或账本容量耗尽为 `507`。

所有 64 位 ID、代次、修订号、请求序号、统计累计值都在响应中使用十进制字符串，保留超过
JavaScript 安全整数范围的值。请求允许安全范围内的小整数，但超过 `2^53-1` 必须使用字符串。
布尔值不能冒充整数，字符串不得带正负号、空白、前导零或 `any`。未知字段和重复字段直接拒绝。
未来兼容扩展只能增加响应字段；客户端应忽略未知响应字段。改变既有字段含义需新增 API 版本。

## 查询端点

| GET 端点 | 查询参数 | 主要数据 |
|---|---|---|
| `/v1/health` | 无 | 主线程响应、就绪原因、实际 worker/端口与持久状态 |
| `/v1/ready` | 无 | 同 health，未就绪返回 503 |
| `/v1/status` | 无 | 配置修订、任务数、dirty/residual/isolated |
| `/v1/persistence` | 无 | 内存与磁盘修订、上次错误 |
| `/v1/stats` | `port=all`、`queue=all`，也可填具体编号 | 端口/队列累计统计 |
| `/v1/rules` | `after`、`revision` | 规则摘要分页 |
| `/v1/rules/ID` | 无 | 完整匹配项与动作 |
| `/v1/rules/ID/count` | 必填 `generation` | 私有 COUNT |
| `/v1/counts/ID` | 必填 `generation` | 共享 COUNT 与引用数 |
| `/v1/configuration` | `revision`、`after_table`、`after_count` | 模板/共享对象及各自分页游标 |
| `/v1/requests/SEQUENCE` | 无 | 当前凭据 client 的持久回执 |

首个配置页可以省略修订号，后续页须使用返回的同一修订号，不能把不同版本的页拼成完整配置。
规则分页同样使用返回的 `revision` 与 `next`。COUNT 在 daemon 重启后从零开始，逻辑 ID 和
generation 保留，调用方不能将跨重启累计值直接相减。

## 持久写入

POST 必须使用 `Content-Type: application/json`。每个策略写入包含 `sequence`，复用同键同内容
会查询原任务而不会再次执行；同键不同内容返回冲突。序号超过有限回执保留窗口后，旧序号仍会
被高水位拒绝，不能当作新请求重放。响应丢失、连接关闭或超时后，按原序号查询或重发原内容。

```json
{
  "sequence":"1",
  "table":"1",
  "queue":0,
  "rule":{"id":"100","port":0,"kind":"drop","priority":0}
}
```

将以上内容 POST 到 `/v1/rules/create`。`/v1/rules/update` 使用相同结构，再加必填的精确旧
`generation`。`/v1/rules/remove` 使用 `sequence`、`queue`、`id`、精确 `generation` 四个字段。

规则 `kind` 支持 `drop`、`ipv4_acl`、`ipv6_acl`。ACL 还接受 `protocol`（ipv4/ipv6、udp、tcp、
ip:N）、`source`/`destination`（CIDR 或 any）、`source_port`/`destination_port`（单端口、闭区间
或 any）、`action`（allow/drop）、`count`、`mark`。可添加
`shared_count:{"id":"900","generation":"21"}` 引用精确共享对象。规则均使用正式软件模式，
CLI/本地 C API 的其他构造器没有自动映射为任意字符串执行入口。

持久回执包含 `client`、`sequence`、`operation`、`state`（pending/complete）、业务 `status`、
`applied`、`durable`、`blocked`、`revision`、`rules:[{id,generation}]`。成功写入要求任务为
complete、业务 status 为 0、durable 为 true，并且 blocked 为 false。dirty 或已知残留时，先
查询状态并恢复，不能把“文件已保存”解释为所有清理都完成。

POST `/v1/configuration` 要求 admin，格式如下；每批 1～8 项，不允许任意文件路径：

```json
{
  "sequence":"2",
  "revision":"10",
  "changes":[
    {"operation":"count_add","id":"900","port":0,"domain":"ingress"},
    {"operation":"template_add","key":"2","capacity":16,"dynamic_actions":0,
     "example":{"id":"1","port":0,"kind":"drop",
                "shared_count":{"id":"900","generation":"0"}}}
  ]
}
```

模板中的共享代次 0 只指向同批新建对象。`template_delete` 接受 `key`，`count_delete` 接受
`id` 和精确 `generation`。引用保护、配额、一次提交与持久化失败封闭仍由正式 store 实施。

POST `/v1/recovery/flush` 或 `/v1/recovery/retry` 使用空对象 `{}`，仅 admin 可调用。这两项是
显式恢复控制，沿用当前状态重复修复，不创建策略请求序号。不会释放无法识别的硬件对象。

## 资源限制与验收

请求体 64 KiB、全部请求头 16 KiB、URL 2048 字节、CLI 输出 128 KiB。默认最多 8 个处理线程、
每连接总时限 15 秒，每连接只处理一个请求。TLS 握手同样有时限且占用并发槽；持续缓慢发送
字节也不能无限占用 HTTP 线程。并发槽已满时关闭新连接；客户端读完响应时，服务器线程可能还在
完成审计入队和连接清理，因此紧接着的新连接也可能遇到正常准入背压。客户端不能把断连推断为写入
未执行，修改请求应按原序号查询结果。禁止 chunked、Expect 和重复关键头，
避免两层解析对消息边界产生不同理解。操作超时不会撤销已经送达 daemon 的写入。

`tests/unit/test_northbound.py` 使用真实 HTTP 服务器测试权限、输入注入、重复字段、大头部、
慢连接、Linux 凭据文件保护、FIFO 拒绝，以及真实 stderr 管道填满/关闭时继续响应和及时退出。
`tests/integration/m5_northbound.py` 使用未修改的正式 daemon、
CLI 与网关，覆盖幂等键冲突、超过 53 位的 ID、SIGKILL 后查询、模板/共享对象事务、规则迁移、
端口/队列统计、验证证书的 TLS、拒绝明文远程监听及日志不泄露令牌。该脚本不声称提供硬件
卸载或真实远端网络吞吐验收，真实数据包证据由已有 daemon 双 TAP 回归提供。

2026-10-22，本项在 DPDK 21.11.9、24.11.8 的优化与 ASan/UBSan 四套构建通过
每套 90/90 Meson 单测，北向 Python 内含 11 项真实服务器及资源边界测试，无跳过
各套既有 M2 核心、M3、M4、M5 恢复回归，以及四阶段正式 HTTP/TLS 闭环全部通过
329 项源码摘要一致：`7777ab35a52eaa3b1a9da87acb10295cdf1fa8aebab82025998494528a09a7b3`
证据保存在 `build/native-check/m5-api-artifacts/summary.json` 与同目录原始报告包
本项验收不代替包含其余生产化功能的最终同源 TAP、性能与发布联合验收
