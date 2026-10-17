# IPv4 三层转发闭环

本项属于 M2 软件数据面阶段。目标是路由和静态邻居管理、最长前缀选择出口、MAC/TTL/
校验和改写、原子配置更新、持久化和重启恢复全部接通，再完成真实收发验收、提交和推送。
路由核心、报文改写、生产 worker、正式 daemon/CLI、独立路由持久化和重启恢复均已
接通。系统 DPDK 与原生 TAP 标识补丁两套构建均完成三端口真实收发及存储故障验收。
这是 M2 中 IPv4 LPM、rewrite 和静态 neighbor 的闭环，[软件 ACL](acl.md) 已另行接通，NAT 和整个 M2 仍未完成。

## 已落实的核心

- 路由由 IPv4 前缀、长度、出口和可选下一跳组成，支持默认路由和 /32 主机路由
- 下一跳为零时按目的 IP 查静态邻居，邻居键包含出口和 IP，同一 IP 可以用于不同出口
- 最长前缀的邻居缺失时明确丢弃，不改走较短前缀，不回退固定端口对
- 每份配置同时保存路由和邻居，以精确旧版本检查后一次发布，输入配置不能指定新版本
- 前缀树查询最多检查 32 位，邻居按出口和 IP 排序后做二分查找
- 工作线程无控制面锁，每个报文只读取一次配置指针，决定中复制出口和 MAC
- 控制面先编译、校验和分配，再执行可选的持久保存回调，成功后才发布
- 旧快照使用 DPDK QSBR 回收，退役数量有上限，慢读者只使新更新返回重试
- 软件规则先执行，DROP 不被路由错误覆盖，COUNT 保留原有命中语义

报文改写在读取完整头部、核对 IPv4 长度和头校验和后执行。源 MAC 来自实际出口，目的
MAC 来自同一版邻居表，TTL 递减一次并重新计算头校验和。IP 地址、UDP/TCP 内容、VLAN
标签和负载保持不变；清除旧 TX offload 请求，避免驱动再次误算校验和。

头部可以跨 mbuf 段。共享、间接或外部缓冲区先取得独占副本；分配失败保持原包和引用
不变，成功后把新的所有权交还 worker。无路由、无邻居、TTL 耗尽、坏校验和、不支持的
三层报文、改写失败和出口不可用分别统计。一次 RX burst 按实际出口分组发送，RX/丢弃
归入口、TX 归实际出口，同口转发同样不会重复释放或重复统计。

默认仍走已有端口对转发，三层路由显式启用。新增三层统计时管理协议提升至 v16，随后
[ACL](acl.md) 扩展为 v17，daemon 和 dppctl 必须同版本配套。原始单字段统计是独立原子采样，不承诺全部字段来自同一时刻。

TTL、头校验和及下一跳处理参考 [RFC 1812](https://www.rfc-editor.org/rfc/rfc1812.html)。
本阶段实现的是配置静态邻居的单播转发数据面，不宣称完整 RFC 路由器：不做 ARP 自动
发现、ICMP 错误生成、动态路由或重组。IPv4 分片逐片转发；初版 IPv4 options 仅接受
NOP/EOL 填充，其他选项明确拒绝，避免把源路由选项当作普通数据放行。
回收规则遵循 [DPDK 21.11 QSBR](https://doc.dpdk.org/guides-21.11/prog_guide/rcu_lib.html)，
注册后读表，一轮完整端口扫描后报告静默点，停止并注销全部读者后才允许销毁路由器。

## 当前验证

核心单测以独立线性模型对照 256 条路由和 20000 次查询，并检查重复配置、过期版本、
非法前缀、端口与邻居、内存不足、持久回调失败和版本耗尽。三个并发读者与 2000 次
路由/邻居联合更新交叉执行，按版本检查出口和 MAC 始终一致；慢读者覆盖回收上限。

改写单测遍历每一个头部拆分点，覆盖 0–2 层 VLAN、IPv4 填充选项、分片、TTL 和校验和
错误，以及共享缓冲复制失败时保留原包。真实 net_ring 测试使用正式 worker，分别运行
四端口的一队列和两队列，核对 17 类报文的完整输出字节、各出口统计、独占副本、邻居
缺失及其他丢弃原因、软件规则优先级和 1000 次在线更新，并检查 mbuf 和外部内存回收。

```sh
meson test -C build --print-errorlogs 'IPv4 routing configuration and lookup' 'IPv4 routed packet rewrite'
python3 tests/integration/ipv4_traffic.py --build-dir build
python3 tests/integration/ipv4_traffic.py --build-dir build-native
```

## 启用和管理

在已有 EAL 参数后的应用参数中加入 `--ipv4-routing /private/routes.bin`，路径父目录
必须存在。该模式接受 1–16 个配置端口，包括奇数个端口；没有路由时全部明确丢弃。
不加该参数仍使用原有偶数端口对。入口会核对出口源 MAC、设备名称、驱动及 TAP 接口名，
并在创建存储文件前检查路由快照及其 `.lock` 与规则、恢复、监护、管理 socket 路径冲突。

```sh
# 在已有双端口启动参数的 -- 后追加 --ipv4-routing /private/routes.bin
./build/dppctl routing-status
./build/dppctl neighbor-set 1 192.0.2.1 02:00:00:00:00:01 1
./build/dppctl route-set 10.0.0.0/8 1 192.0.2.1 2
./build/dppctl routing-show
./build/dppctl route-delete 10.0.0.0/8 3
./build/dppctl neighbor-delete 1 192.0.2.1 4
```

首次创建为空表、版本 1。每次成功修改递增一次，最后一个参数必须是查询到的精确旧
版本；不接受 `any` 或零，过期返回 ESTALE。`route-set` 按前缀/长度创建或替换，
`neighbor-set` 按出口/IP 创建或替换。直连路由使用 `direct`，其目的 IP 必须已有该出口
的静态邻居。CLI 会规范化 CIDR，服务端完整配置要求规范前缀。删除不存在项返回 ENOENT。

`routing-replace FILE EXPECTED_GENERATION` 一次替换全部路由和邻居，未列出的项会删除。
文本文件最多 64 KiB，支持空行和 `#` 注释，每行只有以下四个字段：

```text
route 10.0.0.0/8 1 192.0.2.1
neighbor 1 192.0.2.1 02:00:00:00:00:01
route 198.51.100.0/24 0 direct
neighbor 0 198.51.100.7 02:00:00:00:00:07
```

任意行无效则整份请求拒绝，不能把解析失败当作空表提交。每表最多 256 条。完整替换
保证路由与邻居共同发布，两个连续单项命令各自是一次提交，不提供跨命令事务。
规则快照与路由快照也各自提交，不提供跨文件原子更新。三层模式只接受 ingress 规则，
启动重放同样检查；拒绝 egress/transfer，避免交换直转绕过软件改写。送入 worker 的
报文先执行已有软件策略；硬件 DROP 可以在到达 worker 前结束报文。

## 持久化及故障处理

路由配置使用独立的 v1 文件，不改变规则 snapshot v2。文件精确 11712 字节：64 字节
小端头部，16 个 216 字节端口映射，256 个 16 字节路由槽和 256 个 16 字节邻居槽。
IP 保持网络字节顺序，未用槽位和保留区全零，全文件 CRC32 检查时把校验字段视为零。
恢复同时核对规范排序、字段语义和完整端口映射，拒绝损坏、截断、非规范编码及映射
不符。MAC 不作为稳定端口身份保存，重启后由实际出口重新提供。该映射不证明内核
规则所有权，也不替代已有 TAP 恢复保护。

文件与独立 `.lock` 要求当前用户所有、0600、普通单链接文件，拒绝符号链接。生命周期
独占锁随进程死亡释放；保存前再次核对目录、锁、快照 inode 及完整已知字节。先完整
写临时文件并同步，再原子替换，最后同步目录；所有候选内存先准备，保存成功后才发布。

保存失败返回 EUCLEAN，`routing-status` 报 `dirty=yes` 和原始 `last-error`，活跃表保持旧版，
`ready` 增加 `routing-dirty` 并失败，后续路由写入拒绝。旧配置继续转发，只读查询保留。
修复存储问题后显式执行 `routing-flush`，将当前活跃表重新保存；成功才解除 dirty，
版本不递增。正常退出不自动 flush 不确定路由状态，并以失败状态退出。

如果 rename 已发生但目录同步失败，磁盘可能已经包含候选版本。返回错误不能解释为
磁盘已回滚。进程仍存活时 flush 旧活跃版本可修复；若在修复前崩溃，重启按磁盘上完整
有效的版本恢复，可能是候选版本。`persisted-generation` 仅是本进程最后确认同步的版本。
外部文件替换或损坏会继续拒绝 flush，需要停机核对和恢复备份，不能自动覆盖未知文件。

## 可复现验收

Ubuntu 22.04、DPDK 21.11.9 系统构建与原生 TAP 标识补丁构建均为 `-Werror`，各 40 项
单测通过。持久化单测覆盖短写、空间/文件同步/目录同步/rename 故障、重启、单实例锁、
权限、硬链接、符号链接、inode 替换及路径别名。相关五项单测通过 AddressSanitizer/UBSan。

```sh
meson test -C build --print-errorlogs
python3 tests/integration/ipv4_traffic.py --build-dir build
sudo python3 tests/integration/ipv4_routing.py --build-dir build
sudo python3 tests/integration/ipv4_routing.py --build-dir build-native
python3 tests/integration/software_traffic.py --build-dir build
```

daemon 验收自行创建空网络命名空间和三个持久 TAP，逐字节检查 LPM、直连、VLAN、MAC、
TTL、校验和及负载，检查缺邻居不回退、无路由、坏校验和、TTL、软件 DROP/COUNT 顺序，
再执行 CLI 整表替换、过期/无效请求、SIGKILL 重启、坏文件和端口映射错配拒绝。
两种同步故障均核对旧活跃配置、磁盘版本、dirty 就绪状态、阻断更新、显式修复；另检验
目录同步失败后崩溃恢复候选完整版本。故障只在测试版入口包装 fsync，正式程序无开关。
抓包使用内核辅助信息还原剥离的 VLAN，再比较完整字节；重启后重建抓包 socket，避免
前次接口关闭留下的 ENETDOWN。测试结束检查自有 TAP 全部删除，不改实体网卡。

旧二层 worker 一队列、两队列及重放回归同时通过；新 worker 四端口两种队列均无 mbuf
泄漏，1000 次热更新保持出口与 MAC 一致。尚未进行物理网卡线速、动态邻居或完整 RFC
路由器验收，这些不计入本次 IPv4 静态转发目标。
