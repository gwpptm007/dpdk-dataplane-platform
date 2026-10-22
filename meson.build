project(
  'dpdk-dataplane-platform',
  'c',
  version: '0.3.0',
  default_options: ['c_std=gnu11', 'warning_level=3'],
  meson_version: '>=0.61.0',
)

dpdk = dependency('libdpdk', version: '>=21.11', required: true)
threads = dependency('threads')
dppd_inc = include_directories('include')

dppd_sources = files(
  'lib/runtime/config.c',
  'lib/runtime/runtime.c',
  'lib/runtime/numa.c',
  'lib/runtime/device_recovery.c',
  'lib/device/device.c',
  'lib/device/topology.c',
  'lib/device/safety.c',
  'lib/control/snapshot.c',
  'lib/control/rule.c',
  'lib/control/rule_repository.c',
  'lib/control/slot_index.c',
  'lib/control/persistence.c',
  'lib/control/routing.c',
  'lib/control/recovery_guard.c',
  'lib/control/recovery_inspect.c',
  'lib/control/recovery_plan.c',
  'lib/control/recovery_watch.c',
  'lib/control/recovery_monitor.c',
  'lib/control/recovery_scope.c',
  'lib/control/cleanup_journal.c',
  'lib/control/recovery_cleanup.c',
  'lib/control/recovery_delete.c',
  'lib/control/monitor_channel.c',
  'lib/control/tap_content.c',
  'lib/control/planner.c',
  'lib/control/transfer.c',
  'lib/control/transaction.c',
  'lib/control/control.c',
  'lib/control/admission.c',
  'lib/control/flow_template.c',
  'lib/control/async_flow.c',
  'lib/control/flow_service.c',
  'lib/control/flow_shared_count.c',
  'lib/control/flow_state.c',
  'lib/control/flow_request.c',
  'lib/control/flow_store.c',
  'lib/control/age.c',
  'lib/control/reset.c',
  'lib/management/server.c',
  'lib/management/flow_management.c',
  'lib/runtime/managed.c',
  'lib/dataplane/packet.c',
  'lib/dataplane/ipv6.c',
  'lib/dataplane/nd6.c',
  'lib/dataplane/ipv6_rewrite.c',
  'lib/dataplane/packet_mbuf.c',
  'lib/dataplane/pipeline.c',
  'lib/dataplane/ipv4_router.c',
  'lib/dataplane/ipv4_rewrite.c',
  'lib/dataplane/software_backend.c',
  'lib/dataplane/software_shared.c',
  'lib/dataplane/software_async_flow.c',
  'lib/dataplane/acl.c',
  'lib/dataplane/exact.c',
  'lib/dataplane/tunnel.c',
  'lib/dataplane/conntrack.c',
  'lib/dataplane/transport.c',
  'lib/dataplane/worker.c',
  'lib/offload/rte_flow.c',
  'lib/offload/flow_template.c',
  'lib/offload/async_flow.c',
  'lib/offload/flow_shared_count.c',
  'lib/offload/native_flow_lock.c',
  'lib/offload/shared_count.c',
  'lib/offload/rte_flow_backend.c',
  'lib/offload/capability.c',
  'lib/observability/stats.c',
  'lib/observability/production.c',
  'lib/observability/telemetry.c',
  'lib/observability/rule_observation.c',
  'lib/observability/rule_latency.c',
)

dppd_core = static_library(
  'dppd_core',
  dppd_sources,
  include_directories: dppd_inc,
  dependencies: [dpdk, threads],
  install: false,
)

dppd_core_dep = declare_dependency(
  link_with: dppd_core,
  include_directories: dppd_inc,
  dependencies: [dpdk, threads],
)

executable(
  'dppd',
  'app/dppd/main.c',
  dependencies: dppd_core_dep,
  install: false,
)

executable(
  'dppctl',
  'app/dppctl/main.c',
  'app/dppctl/config_cli.c',
  'app/dppctl/json.c',
  'app/dppctl/routing_cli.c',
  'app/dppctl/conntrack_cli.c',
  'app/dppctl/transfer_cli.c',
  include_directories: dppd_inc,
  install: false,
)

flow_state_cli = executable('dppd-flow-state', 'app/dppd-flow-state/main.c', dependencies: dppd_core_dep, install: false)

executable(
  'dppd-snapshot-migrate',
  'app/dppd_snapshot_migrate/main.c',
  dependencies: dppd_core_dep,
  install: false,
)

executable(
  'dppd-recovery',
  'app/dppd_recovery/main.c',
  dependencies: dppd_core_dep,
  install: false,
)

executable('dppd-monitor', 'app/dppd_monitor/main.c', dependencies: dppd_core_dep, install: false)
executable('dppd-scope', 'app/dppd_scope/main.c', dependencies: dppd_core_dep, install: false)

if get_option('tests')
  # 正式启动器逻辑保持一致，仅把恢复角色指向带确定性暂停点的测试工具
  executable('dppd-scope-test', 'app/dppd_scope/main.c', dependencies: dppd_core_dep,
    c_args: ['-DDPPD_SCOPE_RECOVERY_PROGRAM="tests/integration/test_cleanup_window"',
             '-DDPPD_SCOPE_BUSINESS_PROGRAM="tests/integration/test_scope_business_window"'], install: false)
  subdir('tests/unit')
  subdir('tests/integration')
  subdir('tests/bench')
endif

summary({
  'DPDK version': dpdk.version(),
  'Unit tests': get_option('tests'),
}, section: 'dppd V3')
