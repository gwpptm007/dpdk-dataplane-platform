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
  'lib/device/device.c',
  'lib/device/topology.c',
  'lib/control/snapshot.c',
  'lib/control/rule.c',
  'lib/control/rule_repository.c',
  'lib/control/persistence.c',
  'lib/control/planner.c',
  'lib/control/transaction.c',
  'lib/control/control.c',
  'lib/management/server.c',
  'lib/dataplane/packet.c',
  'lib/dataplane/packet_mbuf.c',
  'lib/dataplane/pipeline.c',
  'lib/dataplane/software_backend.c',
  'lib/dataplane/worker.c',
  'lib/offload/rte_flow.c',
  'lib/offload/rte_flow_backend.c',
  'lib/offload/capability.c',
  'lib/observability/stats.c',
  'lib/observability/telemetry.c',
  'lib/observability/rule_observation.c',
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
  include_directories: dppd_inc,
  install: false,
)

executable(
  'dppd-snapshot-migrate',
  'app/dppd_snapshot_migrate/main.c',
  dependencies: dppd_core_dep,
  install: false,
)

if get_option('tests')
  subdir('tests/unit')
  subdir('tests/integration')
endif

summary({
  'DPDK version': dpdk.version(),
  'Unit tests': get_option('tests'),
}, section: 'dppd V3')
