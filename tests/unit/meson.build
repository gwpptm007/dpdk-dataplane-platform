packet_test = executable(
  'test_packet',
  'test_packet.c',
  dependencies: dppd_core_dep,
)
test('packet parser', packet_test)

config_test = executable(
  'test_config',
  'test_config.c',
  dependencies: dppd_core_dep,
)
test('runtime configuration', config_test)

stats_test = executable('test_stats', 'test_stats.c', dependencies: dppd_core_dep)
test('port and queue statistics', stats_test)

device_link_test = executable('test_device_link', 'test_device_link.c',
  dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=rte_eth_link_get_nowait',
              '-Wl,--wrap=rte_eth_dev_is_valid_port', '-Wl,--wrap=rte_eth_dev_is_removed'])
test('device link monitoring', device_link_test)

device_cleanup_test = executable('test_device_cleanup', 'test_device_cleanup.c',
  dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=rte_eth_dev_stop', '-Wl,--wrap=rte_eth_dev_close',
              '-Wl,--wrap=rte_eth_dev_callback_unregister',
              '-Wl,--wrap=rte_mempool_avail_count', '-Wl,--wrap=rte_mempool_free'])
test('device cleanup failures', device_cleanup_test)

rule_test = executable(
  'test_rule',
  'test_rule.c',
  dependencies: dppd_core_dep,
)
test('rule validation', rule_test)

rule_repository_test = executable(
  'test_rule_repository',
  'test_rule_repository.c',
  dependencies: dppd_core_dep,
)
test('rule repository', rule_repository_test)

planner_test = executable(
  'test_planner',
  'test_planner.c',
  dependencies: dppd_core_dep,
)
test('rule planner', planner_test)

transaction_test = executable(
  'test_transaction',
  'test_transaction.c',
  dependencies: dppd_core_dep,
)
test('rule transaction', transaction_test)

rte_flow_backend_test = executable(
  'test_rte_flow_backend',
  'test_rte_flow_backend.c',
  dependencies: dppd_core_dep,
)
test('rte_flow transaction backend', rte_flow_backend_test)

control_test = executable(
  'test_control',
  'test_control.c',
  dependencies: dppd_core_dep,
)
test('control service', control_test)

batch_update_test = executable(
  'test_batch_update',
  'test_batch_update.c',
  dependencies: dppd_core_dep,
)
test('atomic batch update', batch_update_test)

software_batch_test = executable(
  'test_software_batch',
  'test_software_batch.c',
  dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=calloc'],
)
test('software batch snapshot publication', software_batch_test, timeout: 60)

pipeline_test = executable(
  'test_pipeline',
  'test_pipeline.c',
  dependencies: dppd_core_dep,
)
test('pipeline snapshot', pipeline_test)

software_backend_test = executable(
  'test_software_backend',
  'test_software_backend.c',
  dependencies: dppd_core_dep,
)
test('software rule backend', software_backend_test)

management_test = executable(
  'test_management',
  'test_management.c',
  dependencies: dppd_core_dep,
)
test('management protocol and socket', management_test)

# 健康状态测试不访问网卡，覆盖就绪条件和正式 worker 初始化失败的状态发布
health_test = executable('test_health', 'test_health.c', dependencies: dppd_core_dep)
test('health and worker readiness', health_test)

# 只在状态测试中包装时钟，验证统计不可用时规则仍能正确安装
rule_status_test = executable('test_rule_status', 'test_rule_status.c',
  dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=clock_gettime'])
test('rule installation status', rule_status_test)

# 控制时钟而不等待实际过期，并验证缓存初始化失败时没有残留分配
capability_test = executable('test_capability', 'test_capability.c',
  dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=clock_gettime', '-Wl,--wrap=calloc'])
test('capability profile and probe cache', capability_test)

dppctl_test = executable(
  'test_dppctl',
  'test_dppctl.c',
  include_directories: dppd_inc,
)
test('dppctl batch update arguments', dppctl_test)

persistence_test = executable(
  'test_persistence',
  'test_persistence.c',
  dependencies: dppd_core_dep,
)
test('rule snapshot persistence', persistence_test)
