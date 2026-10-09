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

# 只为恢复窗口测试暂停真实 TAP 创建，不链接生产程序
shared_library('dppd_recovery_attempt_faults', 'recovery_attempt_faults.c',
  dependencies: [dpdk, meson.get_compiler('c').find_library('dl')], install: false)

ring_pmd = meson.get_compiler('c').find_library('rte_net_ring', required: false)
if ring_pmd.found()
  executable(
    'test_software_traffic',
    'software_traffic.c',
    dependencies: [dppd_core_dep, ring_pmd],
    link_args: ['-Wl,--wrap=calloc'],
    install: false,
  )
endif
