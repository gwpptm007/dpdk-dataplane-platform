# Explicit LD_PRELOAD fixture, never linked into dppd or installed.
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
