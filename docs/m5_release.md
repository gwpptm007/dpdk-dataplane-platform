# M5 软件发布、部署与升级恢复

`scripts/m5_release.py` 生成带完整内容清单的发布包，并提供独立目录二进制复现检查
`scripts/m5_deploy.py` 管理私有目录中的正式 `dppd --flow-state` 进程，支持单实例升级、回滚、
中断恢复和多个独立实例的串行滚动升级

升级始终保留当前声明策略和嵌入其中的持久请求记录，不保存或恢复一份旧策略来掩盖失败
单实例交接会暂停转发；多实例滚动只检查其他实例就绪，流量调度、会话复制和无损接管由外部系统负责
此能力仅适用于软件受管理模式，不把启动或恢复软件进程当作清理网卡旧对象的证明

## 发布包与两种复现证据

```sh
# 从已经完成验收的源码与对应构建生成发布包，目标文件必须尚不存在
python3 scripts/m5_release.py pack --source . --build-dir build-perf --output /tmp/dppd-release.tar

# 从发布方独立取得 SHA256 后校验，不能只相信包内自带的摘要
python3 scripts/m5_release.py verify --package /tmp/dppd-release.tar --sha256 PACKAGE_SHA256

# 相同 SDK 环境中，两个新目录各自完整编译三个正式程序并逐字节比较
python3 scripts/m5_release.py reproduce --source . \
  --work-dir /tmp/dppd-reproduce-unique --report build-perf/reproduce.json --jobs 4
```

包中包含 `bin/dppd`、`bin/dppctl`、`bin/dppd-flow-state`、`libexec/dppd-api.py`、
两个发布部署脚本、`source.tar`、`build.json` 和 `manifest.json`
`source.tar` 只从明确的源码、测试、脚本和文档目录取文件，统一文本 LF，不包含 Git 元数据、
构建目录、部署实例的 `var`/`etc`、令牌或 TLS 私钥

清单登记每个成员的长度、权限和 SHA256；清单自身摘要作为发布版本 ID
归档固定成员顺序、时间戳、用户和组，同一份输入可以重复产生相同包字节
安装先核对外部提供的完整包摘要，再拒绝重复成员、越界路径、符号链接、硬链接及超限内容
这提供完整性检查，不提供发布者数字签名；发布摘要本身必须经可信渠道取得

`pack` 不重新构建，也不替代源码与已验收二进制的一致性证明，发布流程必须先完成对应验收
重复打包相同二进制与从源码独立构建是两种不同证据，不能互相代替
`reproduce` 将同一份源码复制到两个不同路径，设置 `SOURCE_DATE_EPOCH=0`、固定 locale 和时区，
用编译器路径映射消除构建目录差异，保留两次日志及三个正式程序的摘要
构建使用 `debugoptimized`、测试开启、断言开启、警告视为错误，只有三个程序全部相同才成功
复现证明限定于记录的编译器、链接器、架构、DPDK 和构建参数，不承诺不同 SDK 或编译器的二进制相同

`--jobs` 显式传给 `meson compile -j`，省略时按当前进程允许使用的 CPU 数量决定并发
在八核验收机上可用 `taskset -c 4-7` 包住整个 `reproduce` 命令并加 `--jobs 4`，所有子进程继承该 CPU 范围
参考 TAP 可另用 CPU 0–3；固定性能门禁必须等两边构建、TAP 和故障计时测试都退出后单独运行
参考 DPDK 21.11.9 复现前应清除私有 SDK 的 `PKG_CONFIG_PATH`、`LD_LIBRARY_PATH` 和额外编译/链接环境参数
以实际 `build.json` 中记录的版本为准，不把两套 SDK 混进一次复现

最终证据前需冻结源码和文档，将提交树按 `source_files` 同一白名单及 LF 规则重新归档，核对其 SHA256
该摘要必须等于普通发布验收、TAP 发布验收和双目录复现中的 `source.tar` 摘要
`code_source_sha256` 只覆盖可执行源码与测试，不能用它代替包含文档的完整 `source.tar` 核对
若验收后改动发布包内的说明文档，需重新生成并核对发布和复现证据，不能沿用旧包摘要

发布包不捆绑系统 libc、DPDK 及 PMD 动态库，部署环境需提供对应依赖与权限
私有 SDK 需要在部署命令及服务环境中设置相同的 `LD_LIBRARY_PATH`；实际启动失败会触发回滚或封闭失败

## 初始化和安装

部署器需要 Linux、Python 3.10 及以上、`flock`、`/proc` 和 pidfd 支持
同一实例由同一专用服务用户管理，根目录及 `releases`、`var`、`run`、`etc` 均为 `0700`
初始化目录必须尚不存在，不能经过符号链接，不会覆盖已经存在的业务目录

先用本项目配置工具准备初始策略，再准备不含程序名的参数 JSON
状态文件和管理 socket 路径由部署器追加，参数文件不得再次指定 `--flow-state`、`--state-path` 或 `--control-socket`

```json
[
  "-l", "0-1", "--no-huge", "--no-pci", "--no-shconf", "--no-telemetry",
  "--vdev=net_ring0", "--vdev=net_ring1", "--",
  "--ports", "0,1", "--queues", "1", "--mbufs", "2048", "--cache", "0"
]
```

```sh
python3 scripts/m5_deploy.py --root /var/lib/dppd-instance init \
  --state /absolute/initial.state --arguments /absolute/arguments.json

python3 scripts/m5_deploy.py --root /var/lib/dppd-instance install \
  --package /tmp/dppd-release.tar --sha256 PACKAGE_SHA256

python3 scripts/m5_deploy.py --root /var/lib/dppd-instance start --release RELEASE_ID
python3 scripts/m5_deploy.py --root /var/lib/dppd-instance status
```

实际策略固定在 `var/flow.state`，管理入口在 `run/control.sock`
版本文件放在 `releases/RELEASE_ID`，重复安装相同版本会重新检查实际内容，不改写已安装版本
`current` 链接在候选 ready 后原子切换到当前发布，`current.json` 保存当前与前一个版本
`deployment.json` 保存实例参数，支持 `{root}` 和 `{release}` 两个路径占位符
升级不删除 `var`、`etc` 或旧发布版本，空间清理由部署管理员在核对引用后单独处理

## 升级、回滚和中断恢复

```sh
python3 scripts/m5_deploy.py --root /var/lib/dppd-instance upgrade --release NEW_RELEASE_ID
python3 scripts/m5_deploy.py --root /var/lib/dppd-instance rollback
python3 scripts/m5_deploy.py --root /var/lib/dppd-instance recover
python3 scripts/m5_deploy.py --root /var/lib/dppd-instance stop
```

升级先用候选 `dppd-flow-state restore-check` 恢复一份私有副本，校验文件格式和实际软件恢复能力
旧进程存在 dirty、残留、隔离或未领取任务时拒绝交接；不会替管理客户端丢弃未完成任务
正常退出可能提交最后一批规则，因此停止后还会针对最终持久文件再次预检
整个过程由实例独占文件锁串行保护，事务阶段写入 `transaction.json` 并执行文件和目录同步

启动器在 `exec` 正式 daemon 前保存 PID、系统 boot ID 和进程开始时刻
启动意图另有持久 attempt 和交接锁，身份登记到 `exec` 处于同一锁内
控制程序中断后先取消未登记的启动，迟到子进程必须拒绝旧意图，不能覆写新实例身份或退出回执
停止时用 pidfd 锁定实际进程对象，避免 PID 被复用后误杀其他程序
独立监督进程记录真实退出码，只有正常退出才继续健康升级；超时不会强杀旧实例或启动第二个写入者
就绪后登记 socket 的设备号与 inode，异常退出后只清理身份匹配且已无人监听的旧入口
没有完整身份记录的入口保留并报错，不擅自删除其他进程或用户文件

候选失败后，旧二进制仍须读取和恢复**当前文件**才能回滚
新文件含旧版本不认识的格式时，回滚保持封闭失败和完整文件，管理员应选择兼容该格式的版本再次恢复
部署器不会拿旧版本配置覆盖已经确认成功的新策略或持久请求记录

控制程序中断后使用 `recover`：已就绪的候选完成原事务；其他未完成阶段尝试恢复原版本
已完成事务的实例若进程退出，则从当前持久策略重启当前版本
进程仍活着但未就绪时保留它，先用 `persistence-flush` 或 `flow-retry` 修复隔离状态，不能用部署重启替代修复
失败的恢复可以再次执行；不认识的运行版本、活动 socket、文件身份冲突和不兼容格式不会被自动清除
新策略收到持久成功回执之前的内存状态仍按原 daemon 的持久化约定处理，部署器不另行承诺保留未确认操作

## 多实例滚动升级和 API companion

先在每个独立实例目录安装同一候选版本，再顺序升级

```sh
python3 scripts/m5_deploy.py rolling --release NEW_RELEASE_ID \
  --minimum-ready 1 --roots /var/lib/dppd-a /var/lib/dppd-b
```

部署器按固定目录顺序取得全部实例锁，避免两个滚动任务分别停止不同实例
每次停止一个实例前检查其他实例的实际 `ready` 数量，数量不足立即停止后续升级
一个候选失败会尝试恢复该实例原版本，并停止后续步骤，已经升级成功的实例保留当前版本
当前滚动命令管理同一 Linux 主机上的独立实例；跨主机编排可在每台主机调用单实例命令并另行控制流量
这里没有共享状态复制、跨主机一致性、自动流量接管或无损转发承诺

北向网关由服务守护器作为独立 companion 进程启动

```sh
python3 /var/lib/dppd-instance/current/libexec/dppd-api.py \
  --socket /var/lib/dppd-instance/run/control.sock \
  --dppctl /var/lib/dppd-instance/current/bin/dppctl \
  --tokens /var/lib/dppd-instance/etc/api-tokens.json \
  --unix /var/lib/dppd-instance/run/api.sock
```

网关每次请求选择当前版本 CLI，daemon 暂不可用或协议切换时返回后端错误，不自动重提有副作用的请求
升级网关自身 Python 代码仍需服务守护器重启 companion，不把 daemon 切换描述成 API 进程已升级
认证文件和 TLS 私钥保持在私有配置目录，详情见 [北向 API](m5_northbound.md)
本机管理 socket 与脚本入口属于可信管理员边界，不能直接暴露给未授权网络客户端

## 验收入口与证据范围

```sh
python3 tests/unit/test_m5_release.py
python3 tests/integration/m5_release.py --build-dir build-perf --report-dir build-perf/m5-release

# 在独立网络命名空间内使用真实 TAP，禁止对业务网络命名空间运行
unshare --net python3 tests/integration/m5_release.py \
  --build-dir build-perf --report-dir build-perf/m5-release-tap --tap
```

单测检查归档与路径边界、内容篡改、锁互斥、PID 复用、dirty/任务保护、策略保留、失败回滚、
格式不兼容封闭失败、中断恢复和滚动就绪门禁
集成测试使用正式 daemon/CLI，覆盖重复包字节一致、两个独立实例、升级与回滚、候选启动失败、
格式预检拒绝、升级控制进程中断、实际滚动与 daemon SIGKILL 后恢复
两端健康版本的 `dppd`、`dppctl` 和 `dppd-flow-state` 三个正式程序逐字节相同
测试仅添加不同的发布标签并重新生成清单，用两个不同版本 ID 验证实际进程交接和持久策略保留
预检失败包把 `dppd-flow-state` 替换成退出码为 23 的测试脚本；启动失败包同样替换 `dppd`
这证明部署器会处理预检拒绝和候选启动失败，不是从真实历史版本提取旧 reader 来验证格式迁移
不能据此声称任意历史版本之间已经验证 ABI、真实旧 reader 或状态格式迁移
报告用 `release_pair=same_binaries_distinct_manifests`、`incompatible_reader=injected_failure`
及 `failed_start=injected_failure` 明确记录上述证据范围

TAP 模式共 72 个实际报文，8 个逐字节转发、64 个由声明 DROP 策略丢弃
每包检查 worker RX 增量，避免把 TAP 内核丢包算作策略效果
测试保留私有临时目录和日志作为失败证据，退出时只停止本轮登记的实例
源码摘要、二进制摘要、包摘要、阶段结果和真实报文计数写入 `report.json`
双目录二进制复现由单独的 `reproduce.json` 记录，最终 M5 联合报告必须同时引用两类证据
