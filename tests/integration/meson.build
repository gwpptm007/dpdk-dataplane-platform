# 独立模拟服务只演练控制与报文字节，不初始化 EAL，也不进入正式 daemon
executable('dppd_transfer_sim', 'transfer_sim.c', dependencies: dppd_core_dep, install: false)

# 测试专用入口只包装读者注册，正式 daemon 的线程监控和资源清理保持不变
executable(
  'test_worker_failure',
  'worker_failure.c',
  dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=dppd_software_backend_worker_register'],
  install: false,
)

# 显式 LD_PRELOAD 的移除故障库，不链接到正式 dppd，也不安装
shared_library(
  'dppd_removal_faults',
  'removal_faults.c',
  dependencies: [dpdk, dependency('threads'), meson.get_compiler('c').find_library('dl')],
  install: false,
)

shared_library(
  'dppd_link_faults',
  'link_faults.c',
  dependencies: [dpdk, meson.get_compiler('c').find_library('dl')],
  install: false,
)

executable(
  'test_rss_receive',
  'rss_receive.c',
  dependencies: dpdk,
  install: false,
)

shared_library(
  'dppd_flow_faults',
  'flow_faults.c',
  dependencies: dpdk,
  install: false,
)

# 正式 daemon 通过真实恢复日志管理测试共享对象，不作为任何硬件动作支持证据
shared_library('dppd_shared_recovery_faults', 'shared_recovery_faults.c', dependencies: dpdk, install: false)

# 只为恢复窗口测试暂停真实 TAP 创建，不链接生产程序
shared_library('dppd_recovery_attempt_faults', 'recovery_attempt_faults.c',
  include_directories: dppd_inc,
  dependencies: [dpdk, meson.get_compiler('c').find_library('dl')], install: false)

# 仅测试版 CLI 暂停在两次查询之间，正式离线工具没有故障开关
executable('test_recovery_plan_window', 'recovery_plan_window.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=dppd_recovery_inspect', '-Wl,--wrap=recvmsg'], install: false)

# 复用正式监护入口，只在测试程序的第二次事件检查处注入丢包
executable('test_recovery_monitor_lost', '../../app/dppd_monitor/main.c', 'recovery_monitor_faults.c',
  dependencies: dppd_core_dep, link_args: ['-Wl,--wrap=dppd_recovery_watch_check'], install: false)

# 暂停业务入口中的真实保护操作，验证最后一次监护检查阻止进入驱动
executable('test_monitor_install_window', '../../app/dppd/main.c', 'monitor_install_window.c',
  dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=dppd_recovery_guard_begin', '-Wl,--wrap=dppd_recovery_guard_begin_owned'], install: false)

# 独立工具在取得租约后暂停，用真实锁与监护验证争抢和交接中断
executable('test_monitor_handoff_window', '../../app/dppd_recovery/main.c', 'monitor_handoff_window.c',
  dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=dppd_monitor_recovery_open', '-Wl,--wrap=dppd_recovery_watch_check'], install: false)

# 测试专用启动器新建网络命名空间，检查三个正式入口的降权、线程启动和授权拒绝
executable('test_recovery_scope_fixture', 'recovery_scope_fixture.c', dependencies: dppd_core_dep, install: false)

# 暂停真实清理调用前后并制造一次删除或结果保存失败，不在正式程序中添加开关
executable('test_cleanup_window', 'cleanup_window.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=dppd_recovery_delete_filter', '-Wl,--wrap=dppd_cleanup_journal_set',
              '-Wl,--wrap=dppd_monitor_recovery_open', '-Wl,--wrap=dppd_recovery_guard_acknowledge'], install: false)
executable('test_scope_business_window', 'scope_business_window.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=dppd_recovery_guard_created'], install: false)

ring_pmd = meson.get_compiler('c').find_library('rte_net_ring', required: false)
executable('test_m5_daemon_fault', '../../app/dppd/main.c', 'm5_daemon_fault.c',
  dependencies: dppd_core_dep, link_args: ['-Wl,--wrap=fsync'], install: false)
executable('test_ipv4_store_window', '../../app/dppd/main.c', 'ipv4_store_window.c',
  dependencies: dppd_core_dep, link_args: ['-Wl,--wrap=fsync'], install: false)
if ring_pmd.found()
  executable('test_m5_flow_restart', 'm5_flow_restart.c', dependencies: [dppd_core_dep, ring_pmd], install: false)
  executable('test_m4_flow_traffic', 'm4_flow_traffic.c', dependencies: [dppd_core_dep, ring_pmd],
    link_args: ['-Wl,--wrap=calloc'], install: false)
  executable('test_ipv4_traffic', 'ipv4_traffic.c', dependencies: [dppd_core_dep, ring_pmd], install: false)
  executable(
    'test_software_traffic',
    'software_traffic.c',
    dependencies: [dppd_core_dep, ring_pmd],
    link_args: ['-Wl,--wrap=calloc'],
    install: false,
  )
endif
