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

persistence_test = executable(
  'test_persistence',
  'test_persistence.c',
  dependencies: dppd_core_dep,
)
test('rule snapshot persistence', persistence_test)
