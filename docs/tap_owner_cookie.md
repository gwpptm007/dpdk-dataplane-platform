# TAP 原生标识与恢复记录 v4

2026-10-09，新增可选 `--tap-owner-cookie`。本地 TAP 的 DROP/QUEUE 创建请求会携带一个
16 字节随机标识，标识先随安装意图写盘并完成 fsync，再由适配后的驱动放入同一次
TC 创建消息。进程在内核创建成功、驱动尚未返回时被终止，离线检查也能按标识关联残留。

这一步完成了本地 TAP 的原生标识写入和回读。完整规则内容、接口实例连续性、其他
PMD 与自动精确删除尚未实现。`inspect` 仍只读，标识匹配不会解除恢复保护。

## 支持范围和启用方式

需要本仓库的 [DPDK 21.11.9 TAP 补丁](../patches/dpdk-21.11.9-tap-owner-cookie.patch)，
以及支持 `TCA_ACT_COOKIE` 的 Linux 内核和编译头文件。验证环境为 Ubuntu 22.04.5、
Linux 6.8.0-138、DPDK 21.11.9、GCC 11.4、Meson 0.61.2。

- 仅本地 `net_tap`、ingress 域、单个 DROP 或 QUEUE 动作
- 支持原 TAP 驱动能接受的匹配条件，本轮实际验证 ETH/DROP 与 IPv4/QUEUE
- 不支持 `remote=`、egress、transfer、RSS、COUNT、MARK 或多个业务动作
- 同时配置 `--state-path`、独立的 `--recovery-path` 和 `--tap-owner-cookie`
- 默认关闭，普通模式继续支持系统 DPDK；软件后端不调用原生标识接口

启用时，每次硬件创建前先执行带私有动作的无副作用校验。未打补丁的 TAP 驱动会
明确拒绝，在此情况下没有逐次意图、没有规则创建，也没有业务快照发布。
驱动支持范围不符时同样拒绝，不能把请求悄悄改成没有标识的硬件规则。

## 独立构建

补丁仅针对固定的上游稳定版本，不自动修改系统安装。以下命令从项目根目录执行，
安装到当前用户专用目录。驱动安装目录及其父目录不能允许所有用户写入；DPDK 会
拒绝从 `/tmp` 这类公共可写路径加载插件。构建源码和日志可以另放临时目录。

```bash
DPPD_SOURCE="$PWD"
DPPD_TAP_WORK="$HOME/.cache/dppd-tap-owner-21.11.9"
DPPD_TAP_PREFIX="$HOME/.local/dppd-tap-owner-21.11.9"
mkdir -p "$DPPD_TAP_WORK"
curl -fL https://fast.dpdk.org/rel/dpdk-21.11.9.tar.xz \
  -o "$DPPD_TAP_WORK/dpdk-21.11.9.tar.xz"
printf '%s  %s\n' \
  051664744579097af7ea7b4960d3362f0ff93307a3c33166df6a4657984e2f07 \
  "$DPPD_TAP_WORK/dpdk-21.11.9.tar.xz" | sha256sum -c -
tar -xJf "$DPPD_TAP_WORK/dpdk-21.11.9.tar.xz" -C "$DPPD_TAP_WORK"
patch -d "$DPPD_TAP_WORK/dpdk-stable-21.11.9" -p1 --forward \
  < "$DPPD_SOURCE/patches/dpdk-21.11.9-tap-owner-cookie.patch"
meson setup "$DPPD_TAP_WORK/build" "$DPPD_TAP_WORK/dpdk-stable-21.11.9" \
  --prefix="$DPPD_TAP_PREFIX" --libdir=lib -Dplatform=generic \
  -Denable_drivers=net/tap,net/ring,net/null,mempool/ring \
  -Dtests=false -Dexamples= -Ddefault_library=shared
ninja -C "$DPPD_TAP_WORK/build" -j4
ninja -C "$DPPD_TAP_WORK/build" install
PKG_CONFIG_PATH="$DPPD_TAP_PREFIX/lib/pkgconfig" \
  meson setup build-tap-owner -Dwerror=true
ninja -C build-tap-owner
DPPD_TAP_LIBS="$DPPD_TAP_PREFIX/lib:$DPPD_TAP_PREFIX/lib/dpdk/pmds-22.0"
env LD_LIBRARY_PATH="$DPPD_TAP_LIBS" meson test -C build-tap-owner --print-errorlogs
```

`mempool/ring` 是正常创建报文池所需的驱动，不能因为只测试 TAP 就将它省略。
使用不同 DPDK 安装时应重新建立项目构建目录，并始终配套使用同一套运行库与插件。
补丁中增加的私有协议头与项目 [tap_owner.h](../include/dppd/tap_owner.h) 一致。

## 创建与回读

1. 无副作用检查带标识的规则是否受驱动支持
2. 核对并保存 TAP 的内核索引、名称、boot ID 和网络命名空间
3. 使用 `getrandom()` 生成 16 字节非零标识，拒绝本文件内重复值，随机源故障则停止
4. 保存尝试编号、业务版本、`intent` 和随机标识，完成 fsync 后才交给驱动
5. 驱动校验完整作用范围，把 cookie 与 gact DROP 或 skbedit QUEUE 放入同一创建消息
6. 保存真实创建结果，并重新只读查询内核；成功创建必须恰好读到一个相同标识
7. 回读失败、标识缺失或重复时，事务使用已有真实 handle 回滚；保留原始驱动结果

私有动作类型为 `-0x44505001`，配置由版本号、结构长度和 16 字节标识组成。
它不改变业务匹配或报文处理结果，也不是 DPDK 标准 API。上游 TAP 实现见
[DPDK 21.11.9 源码](https://github.com/DPDK/dpdk-stable/blob/v21.11.9/drivers/net/tap/tap_flow.c)，
内核动作标识定义见 [Linux 6.8 pkt_cls.h](https://github.com/torvalds/linux/blob/v6.8/include/uapi/linux/pkt_cls.h)。

## 离线结果

`show` 增加 `owner-token attempt=N cookie=...`。`inspect` 的相关性输出包括：

| status / owner | 含义 |
|---|---|
| `token-present / token-match` | 当前完整本地观察中恰有一个对象带相同标识 |
| `token-absent / unknown` | 当前观察范围没有相同标识，即使原坐标仍有别的规则 |
| `token-ambiguous / unknown` | 标识在多个对象中出现，不能选择其中一个 |
| `inspection-unavailable / unknown` | 环境不符或查询不完整，不能推断不存在 |
| `removed-record / unknown` | 本进程已记录删除成功，不再认领后来的对象 |

普通模式和旧文件继续使用原来的候选坐标相关性。底层 filter 行仍是原始清单，
具体尝试的标识匹配结论在 `correlation` 行给出。

解析器逐层检查 flower 动作属性。损坏、重复属性、非 16 字节 cookie、带 cookie 的
多动作或未知动作会让整次观察失败，避免漏掉无法解释的标识后错误宣称唯一匹配。

所有结论仍带 `content=unchecked`，整体仍是 `cleanup-confirmed=no`。cookie 是关联
证据，不是身份认证：拥有网络管理权限的外部程序可以复制标识，或在保留标识时改写
规则内容。接口索引、名称和命名空间检查也不是接口实例永不复用的证明。检查结果
不允许直接自动删除，旧的外部清理与精确修订号确认流程继续适用。

## v4 文件与兼容性

管理协议仍为 v15，业务规则快照仍为 v2。独立恢复文件 v4 固定 **37952 字节**。
前 9248 字节及之后 32 字节的逐次记录头沿用 v3；256 个尝试槽位从每个 96 字节变为
112 字节，偏移 96–111 为创建前持久保存的标识。全零表示普通模式或旧记录。
候选坐标内运行时观察到的 cookie 不单独编码，不能冒充创建前的意图标识。

解码除原有检查外，还拒绝本文件中重复的非零标识，以及缺少可靠 TAP 定位却携带
标识的尝试。CRC 仅检测损坏，不用于认证。

v1/v2/v3 待确认记录保持原格式；离线查看、检查和外部确认均不升级。只有干净旧文件
由新 daemon 打开时，才在原 inode 上升级 v4 并推进修订号。v3 的最后尝试编号保留。
旧版程序不能读取 v4，不能直接降级使用这些恢复文件。

## 验证

本轮系统 DPDK 构建和独立补丁构建均使用 `-Werror`，各 29/29 单测通过。六项相关
单测通过 AddressSanitizer/UBSan：原生标识、检查器、逐次记录、恢复保护、硬件 backend
和控制服务。

真实 TAP 验证包括创建前/后 SIGKILL、无 handle 创建失败、DROP/QUEUE cookie 与
`tc -j` 对齐、正常删除、同坐标无标识替换、复制标识歧义、旧驱动拒绝、remote 拒绝和
创建后丢失标识的真实 handle 回滚。普通模式的三个创建窗口、TAP 只读检查与 ring/TAP
恢复保护回归通过。
三组原恢复隔离流程、单/双队列健康与 worker 注册失败、TAP 链路 down/up 回归也通过。
以下测试仅修改自建 TAP；remote 场景额外使用自建 dummy 接口，不操作物理网卡。

```bash
# 系统 DPDK 的普通构建应拒绝原生标识模式
sudo python3 tests/integration/tap_owner_cookie.py --build-dir build --expect-unsupported

# 独立适配构建验证真实标识与崩溃窗口
sudo env LD_LIBRARY_PATH="$DPPD_TAP_LIBS" \
  python3 tests/integration/tap_owner_cookie.py --build-dir build-tap-owner
sudo env LD_LIBRARY_PATH="$DPPD_TAP_LIBS" \
  python3 tests/integration/tap_owner_cookie.py --build-dir build-tap-owner --remote
sudo env LD_LIBRARY_PATH="$DPPD_TAP_LIBS" \
  python3 tests/integration/tap_owner_cookie.py --build-dir build-tap-owner --omit-owner
sudo env LD_LIBRARY_PATH="$DPPD_TAP_LIBS" \
  python3 tests/integration/recovery_attempt.py --build-dir build-tap-owner --native
```

故障夹具通过显式预加载模拟创建成功却没有标识；正式回读必须触发回滚并删除实际
对象。夹具不链接生产程序。下一步是核验规则内容与对象完整作用范围，再设计按
精确对象删除的恢复流程。
