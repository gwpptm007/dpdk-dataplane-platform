# 恢复服务的网络命名空间与权限边界

业务、监护和恢复查询需要观察同一组 TAP，但只有业务需要安装或删除 TC 规则。可以把项目
自建的 TAP 放入独立网络命名空间，让三个服务共享该命名空间，再只向业务服务授予
`CAP_NET_ADMIN`。监护和恢复服务以无网络 capability 的专用用户运行。

网络命名空间隔开接口和 TC 状态；capability 决定进程能否修改命名空间里的网络配置。
因此，普通无 capability 的进程不能写这些 TAP 的 TC 规则。运行在命名空间内的管理员、
获得 `CAP_NET_ADMIN` 的其他程序，以及宿主机管理员仍有能力改变状态；本配置不把这些
可信管理者排除在外。

## 创建仅含项目 TAP 的命名空间

下面示例使用专用服务账户 `dppd`，以及两个预先创建的持久 TAP。先确认命名空间名称未被
其他部署占用，并确认本机已有 `dppd` 账户：

```bash
id dppd
sudo ip netns list
```

由管理员创建网络命名空间和 TAP，并把 TAP 所有者设为服务账户：

```bash
sudo ip netns add dppd-recovery
sudo ip -n dppd-recovery link set lo up
sudo ip netns exec dppd-recovery ip tuntap add dev dppdtap0 mode tap user dppd
sudo ip netns exec dppd-recovery ip tuntap add dev dppdtap1 mode tap user dppd
sudo ip -n dppd-recovery link set dppdtap0 up
sudo ip -n dppd-recovery link set dppdtap1 up
sudo ip -n dppd-recovery -details link show
```

`/run/netns/dppd-recovery` 是这个命名空间的 systemd 可引用路径。该示例不移动或配置
宿主机的实体网卡；不要把其他设备加入此命名空间。服务和验证命令均使用相同路径。

## systemd 服务权限

在现有三个 unit 的 `[Service]` 配置中应用下面的共同设置。实际 unit 名称和可执行文件
路径按部署调整：

```ini
NetworkNamespacePath=/run/netns/dppd-recovery
User=dppd
Group=dppd
NoNewPrivileges=yes
```

业务进程负责修改 TC 规则，在它的 unit 中增加：

```ini
CapabilityBoundingSet=CAP_NET_ADMIN
AmbientCapabilities=CAP_NET_ADMIN
```

监护与恢复工具只观察接口和规则，在各自 unit 中清空 capability 集：

```ini
CapabilityBoundingSet=
AmbientCapabilities=
```

`CAP_NET_ADMIN` 允许管理该命名空间里的网络接口、路由和流量控制规则，因此只授予业务
服务。DPDK 的内存、CPU 亲和性、VFIO 或其他 PMD 设置可能需要额外的资源限制或权限；
部署时应根据实际端口后端单独加入所需项，并保持监护和恢复服务无网络写权限。不要直接
把业务服务改成全权限 root 来绕过启动问题。

应用 unit 后检查配置并重启相关服务：

```bash
sudo systemd-analyze verify /etc/systemd/system/dppd.service \
  /etc/systemd/system/dppd-monitor.service /etc/systemd/system/dppd-recovery.service
sudo systemctl daemon-reload
```

监护与业务仍须使用相同恢复文件、快照路径和 `PORT:IFNAME` 映射。参见
[监护启动步骤](recovery_monitor.md)和[业务握手](recovery_monitor_handshake.md)。

## 手工只读查询

若恢复命令由 systemd oneshot 启动，也应用共同设置和空 capability 集。临时核查可用
`systemd-run` 在同一命名空间内以无 capability 的 `dppd` 用户执行：

```bash
sudo systemd-run --wait --pipe --collect \
  --property=NetworkNamespacePath=/run/netns/dppd-recovery \
  --property=User=dppd --property=Group=dppd \
  --property=NoNewPrivileges=yes \
  --property=CapabilityBoundingSet= --property=AmbientCapabilities= \
  /usr/local/bin/dppd-recovery plan-monitored \
  /var/lib/dppd/hardware.recovery REVISION /var/lib/dppd/monitor-session
```

把 `REVISION` 换成 `dppd-recovery show` 返回的精确数字。查询仍要求业务已退出、监护
存活、会话有效和恢复文件版本匹配；权限隔离不会替代这些检查，也不会使计划可执行。

## 边界验证

以无 capability 的 `dppd` 用户确认能读取接口和订阅 netlink，但不能改变 TAP 状态：

```bash
sudo systemd-run --wait --pipe --collect \
  --property=NetworkNamespacePath=/run/netns/dppd-recovery \
  --property=User=dppd --property=Group=dppd \
  --property=NoNewPrivileges=yes \
  --property=CapabilityBoundingSet= --property=AmbientCapabilities= \
  /bin/sh -c 'ip link show dppdtap0 && ip link set dppdtap0 down'
```

这条命令应在 `ip link show` 成功后，于 `ip link set` 收到 `Operation not permitted`。
之后可重新运行 `ip -n dppd-recovery link show dppdtap0` 确认接口状态未改变。
项目回归脚本也会将真实 `dppd-recovery inspect` 与 `plan` 以无
`CAP_NET_ADMIN` 的普通用户运行，并尝试一次被内核拒绝的 TC 写入：

```bash
sudo python3 tests/integration/recovery_inspect.py --build-dir build
sudo python3 tests/integration/recovery_inspect.py --build-dir build-native
```

脚本只创建并清理自己拥有的 TAP。它验证只读查询不需要 `CAP_NET_ADMIN`，且拒绝写入前后
读取到的 TC 规则列表完全一致；隔离 systemd 命名空间本身仍应按本页的 `systemd-run` 示例
核查。

监护进程也有六个普通用户回归场景，可单独执行：

```bash
sudo python3 tests/integration/recovery_monitor.py --build-dir build --unprivileged
sudo python3 tests/integration/recovery_monitor.py --build-dir build-native --unprivileged
```

这些场景核对正在运行的监护进程的全部 capability 集为空，附加组为空，且不能通过后续
执行获得新权限；监护和查询均以同一普通用户运行。正常停止、进程被杀、接口重建及改名、
恢复文件替换和事件丢失分别核对持久证据，详见[监护验证记录](recovery_monitor.md#验证)。
测试驱动仍以 root 创建自己的 TAP 和注入接口变化，不安装或修改部署服务。

验证完成且相关服务已停止后，管理员可删除专用命名空间。删除前先确认其中只含这次部署
自建的接口；删除命名空间会删除其 TAP：

```bash
sudo ip -n dppd-recovery -details link show
sudo ip netns del dppd-recovery
```

此配置限制的是经过 systemd 配置的服务身份。若其他进程仍在同一命名空间且带有
`CAP_NET_ADMIN`，它仍能写 TC；应同时盘点服务、容器和手工启动进程。`plan-evidence`
目前仍显示 `writer-exclusion=no executable=no`，直到查询与后续删除操作之间也有内核
强制的独占写入边界。
