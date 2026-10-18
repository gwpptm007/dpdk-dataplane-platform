packet_test = executable(
  'test_packet',
  'test_packet.c',
  dependencies: dppd_core_dep,
)
test('packet parser', packet_test)

# 真实多线程同时争抢额度，覆盖整批拒绝、残留保留、旧凭据和动态缩额
admission_test = executable('test_admission', 'test_admission.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=calloc'])
test('atomic global and per-port resource admission', admission_test)

# 倒序和随机复用核对排序与固定槽位，分配失败不能留下半份索引
slot_index_test = executable('test_slot_index', 'test_slot_index.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=calloc'])
test('stable slot index ordering reuse and allocation failure', slot_index_test)

# 容量、引用凭据和部分失败保留实际句柄及配额，旧版本明确拒绝原生模板
flow_template_test = executable('test_flow_template', 'test_flow_template.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=calloc'])
test('template masks leases quotas and partial cleanup', flow_template_test)

native_template_test = executable('test_native_template', 'test_native_template.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=rte_flow_info_get', '-Wl,--wrap=rte_flow_configure',
              '-Wl,--wrap=rte_flow_pattern_template_create', '-Wl,--wrap=rte_flow_actions_template_create',
              '-Wl,--wrap=rte_flow_template_table_create', '-Wl,--wrap=rte_flow_template_table_destroy',
              '-Wl,--wrap=rte_flow_actions_template_destroy', '-Wl,--wrap=rte_flow_pattern_template_destroy'])
test('native DPDK template API and version contract', native_template_test)

# 乱序完成、未领取结果背压、资源保留和真实线程竞争不能混淆受理与完成
async_flow_test = executable('test_async_flow', 'test_async_flow.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=calloc'])
test('bounded asynchronous flow lifecycle and completion identity', async_flow_test)

native_async_flow_test = executable('test_native_async_flow', 'test_native_async_flow.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=rte_flow_async_create', '-Wl,--wrap=rte_flow_async_destroy',
              '-Wl,--wrap=rte_flow_push', '-Wl,--wrap=rte_flow_pull', '-Wl,--wrap=rte_flow_get_q_aged_flows'])
test('native asynchronous enqueue push pull and retained storage', native_async_flow_test)

# 混合增删改在满表下只发布一次，分配故障保留旧 COUNT，并由真实读线程检查中间状态
software_change_batch_test = executable('test_software_change_batch', 'test_software_change_batch.c',
  dependencies: dppd_core_dep, link_args: ['-Wl,--wrap=calloc'])
test('atomic software create replace delete and allocation failures', software_change_batch_test)

# 正式模板、配额、异步完成、账本和 classifier 连通，部分失败要完整补偿再产生终态
flow_service_test = executable('test_flow_service', 'test_flow_service.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=calloc'])
test('M4 asynchronous batch publication and exact restoration', flow_service_test)

# 驱动通知可提前或迟到，自动维护不受未领取回执阻塞，删除失败保留通知并退避重试
async_age_test = executable('test_async_age', 'test_async_age.c', dependencies: dppd_core_dep)
test('M4 queue AGE identity quotas maintenance and retry', async_age_test)

# 共享对象和计数器一起扣额，完整引用凭据阻止跨实例、槽位复用及补偿期间误删
flow_shared_count_test = executable('test_flow_shared_count', 'test_flow_shared_count.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=calloc'])
test('M4 shared COUNT quota leases concurrency and residual cleanup', flow_shared_count_test)

# 只替换 PMD 公共入口，生产原生适配贯通模板、动态 INDIRECT、64 流共享、AGE 和失败补偿
native_shared_flow_test = executable('test_native_shared_flow', 'test_native_shared_flow.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=rte_flow_action_handle_create', '-Wl,--wrap=rte_flow_action_handle_query',
              '-Wl,--wrap=rte_flow_action_handle_destroy', '-Wl,--wrap=rte_flow_info_get', '-Wl,--wrap=rte_flow_configure',
              '-Wl,--wrap=rte_flow_pattern_template_create', '-Wl,--wrap=rte_flow_actions_template_create',
              '-Wl,--wrap=rte_flow_template_table_create', '-Wl,--wrap=rte_flow_template_table_destroy',
              '-Wl,--wrap=rte_flow_actions_template_destroy', '-Wl,--wrap=rte_flow_pattern_template_destroy',
              '-Wl,--wrap=rte_flow_async_create', '-Wl,--wrap=rte_flow_async_destroy',
              '-Wl,--wrap=rte_flow_push', '-Wl,--wrap=rte_flow_pull', '-Wl,--wrap=rte_flow_get_q_aged_flows'])
test('native shared COUNT template queue AGE and compensation contract', native_shared_flow_test)

# 联合报告缺少同源、完整 TAP、构建覆盖或固定门槛时不能得到整体完成标记
test('M4 joint acceptance evidence gates', find_program('python3'),
  args: [files('test_m4_acceptance.py')])

# 配置文件不能保存指针，损坏、版本错配和写盘各阶段故障都有独立结果
flow_state_test = executable('test_flow_state', 'test_flow_state.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=write', '-Wl,--wrap=fsync', '-Wl,--wrap=rename'])
test('M5 M4 configuration codec integrity and atomic persistence', flow_state_test)

# 真正的 SIGKILL 与 exec 重启，不沿用旧地址空间，写盘失败不能领取成功回执
flow_store_test = executable('test_flow_store', 'test_flow_store.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=fsync', '-Wl,--wrap=calloc', '-Wl,--wrap=dppd_flow_service_status'])
test('M5 M4 durable receipts crash replay and residual cleanup', flow_store_test, timeout: 120)
test('M5 16384 rule configuration replay and exact teardown', flow_store_test, args: ['--scale'], timeout: 120)
test('M5 configuration CLI create check and software replay', find_program('python3'),
  args: [files('test_flow_state_cli.py'), flow_state_cli], timeout: 60)

topology_test = executable('test_topology', 'test_topology.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=rte_eth_dev_info_get', '-Wl,--wrap=rte_dev_name', '-Wl,--wrap=socket',
              '-Wl,--wrap=setsockopt', '-Wl,--wrap=sendto', '-Wl,--wrap=recvmsg', '-Wl,--wrap=close',
              '-Wl,--wrap=open', '-Wl,--wrap=open64', '-Wl,--wrap=read', '-Wl,--wrap=realpath',
              '-Wl,--wrap=if_nametoindex', '-Wl,--wrap=__realpath_chk'])
test('verified endpoint roles and bounded topology discovery', topology_test)

flow_contract_test = executable('test_flow_contract', 'test_flow_contract.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=rte_flow_validate', '-Wl,--wrap=rte_flow_create', '-Wl,--wrap=rte_flow_destroy',
              '-Wl,--wrap=rte_flow_query', '-Wl,--wrap=rte_flow_flush'])
test('transfer compilation and hardware counter validity', flow_contract_test)

age_contract_test = executable('test_age_contract', 'test_age_contract.c', dependencies: dppd_core_dep)
test('AGE driver contract and exact generation', age_contract_test)
age_test = executable('test_age', 'test_age.c', dependencies: dppd_core_dep)
test('AGE automatic expiry and persistence', age_test)

shared_control_test = executable('test_shared_control', 'test_shared_control.c', dependencies: dppd_core_dep)
test('shared COUNT versioned control and atomic persistence', shared_control_test)
shared_contract_test = executable('test_shared_contract', 'test_shared_contract.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=rte_flow_action_handle_create', '-Wl,--wrap=rte_flow_action_handle_query',
              '-Wl,--wrap=rte_flow_action_handle_destroy', '-Wl,--wrap=rte_flow_validate', '-Wl,--wrap=rte_flow_create'])
test('shared COUNT indirect driver contract', shared_contract_test)
shared_backend_test = executable('test_shared_backend', 'test_shared_backend.c', dependencies: dppd_core_dep)
test('shared COUNT prepared and installed references', shared_backend_test)
reset_test = executable('test_reset', 'test_reset.c', dependencies: dppd_core_dep)
test('simulated reset isolation and exact desired rebuild', reset_test)

# 回调只发布原子事件，复位、移除和销毁都不能绕过收发门禁
device_events_test = executable('test_device_events', 'test_device_events.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=rte_eth_dev_callback_register', '-Wl,--wrap=rte_eth_dev_callback_unregister',
              '-Wl,--wrap=rte_eth_dev_stop', '-Wl,--wrap=rte_eth_dev_close', '-Wl,--wrap=rte_eth_dev_info_get',
              '-Wl,--wrap=rte_eth_dev_get_name_by_port', '-Wl,--wrap=rte_eth_dev_is_valid_port', '-Wl,--wrap=rte_dev_name'])
test('real device event latch and resume barriers', device_events_test)

worker_device_gate_test = executable('test_worker_device_gate', 'test_worker_device_gate.c', dependencies: dppd_core_dep)
test('worker RX TX event gates and mbuf return', worker_device_gate_test)

device_backend_gate_test = executable('test_device_backend_gate', 'test_device_backend_gate.c', dependencies: dppd_core_dep)
test('unknown device handles block every backend driver entry', device_backend_gate_test)

# 全部设备操作为显式夹具，只验证正式顺序与失败后资源保留，不登记为实机复位
device_reset_test = executable('test_device_reset', 'test_device_reset.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=rte_eth_dev_callback_register', '-Wl,--wrap=rte_eth_dev_callback_unregister',
              '-Wl,--wrap=rte_eth_dev_info_get', '-Wl,--wrap=rte_eth_dev_get_name_by_port',
              '-Wl,--wrap=rte_eth_dev_fw_version_get', '-Wl,--wrap=rte_eth_dev_is_valid_port',
              '-Wl,--wrap=rte_eth_dev_is_removed', '-Wl,--wrap=rte_eth_dev_socket_id', '-Wl,--wrap=rte_eth_dev_stop',
              '-Wl,--wrap=rte_eth_dev_reset', '-Wl,--wrap=rte_eth_dev_configure',
              '-Wl,--wrap=rte_eth_dev_adjust_nb_rx_tx_desc', '-Wl,--wrap=rte_eth_dev_start',
              '-Wl,--wrap=rte_eth_dev_close', '-Wl,--wrap=rte_dev_name', '-Wl,--wrap=rte_eth_macaddr_get',
              '-Wl,--wrap=rte_eth_rx_queue_setup', '-Wl,--wrap=rte_eth_tx_queue_setup',
              '-Wl,--wrap=rte_eth_promiscuous_enable', '-Wl,--wrap=rte_eth_allmulticast_enable',
              '-Wl,--wrap=rte_pktmbuf_pool_create', '-Wl,--wrap=rte_mempool_avail_count', '-Wl,--wrap=rte_mempool_free',
              '-Wl,--wrap=rte_eth_link_get_nowait', '-Wl,--wrap=dppd_topology_discover',
              '-Wl,--wrap=dppd_topology_rediscover_verified'])
test('device stop reset reconfigure identity and failure contract', device_reset_test)

device_safety_test = executable('test_device_safety', 'test_device_safety.c', dependencies: dppd_core_dep)
test('management PCI protection before planned reset', device_safety_test)

# 正式协调器使用真实控制计划，设备边界为夹具，覆盖补偿重试与事件窗口
device_recovery_test = executable('test_device_recovery', 'test_device_recovery.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=dppd_runtime_wait', '-Wl,--wrap=dppd_runtime_start',
              '-Wl,--wrap=dppd_devices_enable_event_tracking', '-Wl,--wrap=dppd_devices_pause_io',
              '-Wl,--wrap=dppd_devices_reset_reconfigure', '-Wl,--wrap=dppd_devices_reset_identity_ready',
              '-Wl,--wrap=dppd_devices_check_reset_safety', '-Wl,--wrap=dppd_topology_rediscover_verified',
              '-Wl,--wrap=dppd_devices_resume_after_event', '-Wl,--wrap=dppd_devices_poll_links'])
test('formal device reset coordination and unknown event isolation', device_recovery_test)

transfer_test = executable('test_transfer', 'test_transfer.c', dependencies: dppd_core_dep)
test('simulated transfer templates and control lifecycle', transfer_test)

transfer_management_test = executable('test_transfer_management', 'test_transfer_management.c', dependencies: dppd_core_dep)
test('transfer management protocol and role evidence', transfer_management_test)

python_test = find_program('python3')
test('M3 read-only inventory semantics', python_test, args: [meson.current_source_dir() / 'test_m3_inventory.py'])

# 实际管理协议、独立管理接口和物理报文证据的反例只验证门禁，不算实机通过
test('M3 physical hardware acceptance gates', python_test, args: [meson.current_source_dir() / 'test_m3_hardware.py'])

acl_test = executable('test_acl', 'test_acl.c', dependencies: dppd_core_dep)
test('compiled ACL and independent reference', acl_test)

exact_test = executable('test_exact', 'test_exact.c', dependencies: dppd_core_dep)
test('exact match hash and independent IR reference', exact_test)

count_batch_test = executable('test_software_count_batch', 'test_software_count_batch.c', dependencies: dppd_core_dep)
test('batched rule counters and protected version lifetime', count_batch_test)

tunnel_test = executable('test_tunnel', 'test_tunnel.c', dependencies: dppd_core_dep)
test('bounded tunnel parsing and scattered packet boundaries', tunnel_test)

conntrack_test = executable('test_conntrack', 'test_conntrack.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=calloc'])
test('connection tracking lifecycle and concurrency', conntrack_test)

dnat_test = executable('test_dnat', 'test_dnat.c', dependencies: dppd_core_dep)
test('static DNAT forwarding and reservations', dnat_test)

# 双栈共用会话容量但独立分配端口，核对 NAT66 双向地址、端口与静态保留
nat66_test = executable('test_nat66', 'test_nat66.c', dependencies: dppd_core_dep)
test('NAT66 mappings and address family isolation', nat66_test)

# 使用独立线缆校验和和可控时钟，验证 ND、DAD、VLAN、等待首包和可达性探测
nd6_test = executable('test_nd6', 'test_nd6.c', dependencies: dppd_core_dep)
test('IPv6 neighbor discovery lifecycle and validation', nd6_test)

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

# 最长前缀与线性参考对照，验证路由和邻居联合发布及慢读者的有界回收
ipv4_router_test = executable('test_ipv4_router', 'test_ipv4_router.c',
  dependencies: dppd_core_dep, link_args: ['-Wl,--wrap=calloc'])
test('IPv4 routing configuration and lookup', ipv4_router_test, timeout: 30)

# 每个头部拆分点都核对真实 mbuf 改写，并检查 TTL、坏校验和及复制失败保留原包
ipv4_rewrite_test = executable('test_ipv4_rewrite', 'test_ipv4_rewrite.c',
  dependencies: dppd_core_dep, link_args: ['-Wl,--wrap=rte_pktmbuf_copy'])
test('IPv4 routed packet rewrite', ipv4_rewrite_test)

ipv6_packet_test = executable('test_ipv6_packet', 'test_ipv6_packet.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=rte_pktmbuf_copy'])
test('IPv6 parser and scattered packet rewrite', ipv6_packet_test)

ipv6_control_test = executable('test_ipv6_control', 'test_ipv6_control.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=fsync'])
test('IPv6 LPM and compatible durable snapshots', ipv6_control_test)

# 完整配置先落盘后发布，验证单实例锁、损坏拒绝和替换后同步失败的显式修复
routing_test = executable('test_routing', 'test_routing.c', dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=pwrite64', '-Wl,--wrap=fsync', '-Wl,--wrap=rename', '-Wl,--wrap=renameat2'])
test('durable IPv4 routing service', routing_test)

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

# 故障注入覆盖原始错误、补偿错误、保存后失败与整批软件发布失败
rule_observation_test = executable('test_rule_observation', 'test_rule_observation.c',
  dependencies: dppd_core_dep, link_args: ['-Wl,--wrap=calloc'])
test('rule failure classification and outcomes', rule_observation_test)

# 正式管理分发搭配真实配置实例，验证落盘失败不能领取成功回执
flow_management_test = executable('test_flow_management', 'test_flow_management.c',
  dependencies: dppd_core_dep, link_args: ['-Wl,--wrap=fsync'])
test('M5 managed protocol durable receipts and session boundaries', flow_management_test)

# 真实容器配合并发回调，检查完整副本发布与退出时的指针生命周期
rule_telemetry_unsigned_suffix = dpdk.version().version_compare('>=23.03') ? 'uint' : 'u64'
rule_telemetry_test = executable('test_rule_telemetry', 'test_rule_telemetry.c',
  dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=calloc', '-Wl,--wrap=rte_telemetry_register_cmd',
              '-Wl,--wrap=rte_tel_data_add_dict_' + rule_telemetry_unsigned_suffix,
              '-Wl,--wrap=rte_tel_data_add_dict_int',
              '-Wl,--wrap=rte_tel_data_add_array_' + rule_telemetry_unsigned_suffix,
              '-Wl,--wrap=rte_tel_data_add_dict_container',
              '-Wl,--wrap=dppd_runtime_stats_read'])
test('rule telemetry publication and lifetime', rule_telemetry_test, timeout: 30)

# 固定区间、分位数与溢出边界，并检查提交计时复用和失败后的历史保留
rule_latency_test = executable('test_rule_latency', 'test_rule_latency.c',
  dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=clock_gettime', '-Wl,--wrap=calloc'])
test('rule commit latency history', rule_latency_test)

# 跨进程文件锁、安装前同步、崩溃保留和部分创建失败的退出清理
recovery_guard_test = executable('test_recovery_guard', 'test_recovery_guard.c',
  dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=fsync'])
test('cross-process recovery guard', recovery_guard_test)

# 内核消息边界、未知身份和只读查询，TAP 实际残留由独立 root 集成测试覆盖
recovery_inspect_test = executable('test_recovery_inspect', 'test_recovery_inspect.c',
  '../../lib/control/tap_content.c', '../../lib/control/rule.c',
  include_directories: dppd_inc)
test('TAP recovery inspection', recovery_inspect_test)

# 只分析内核观察副本，验证跨端口歧义、内容与范围拒绝及不完整列表的整体拒绝
recovery_plan_test = executable('test_recovery_plan', 'test_recovery_plan.c', dependencies: dppd_core_dep)
test('read-only TAP recovery plan', recovery_plan_test)

# 事件丢失、报文损坏、持续事件流及删除重建后的变化标志都不能恢复为可信
recovery_watch_test = executable('test_recovery_watch', 'test_recovery_watch.c', include_directories: dppd_inc)
test('recovery observation window', recovery_watch_test)

# 会话锁真实跨进程释放，损坏编码、写盘失败和文件替换不能产生可用证据
recovery_monitor_test = executable('test_recovery_monitor', 'test_recovery_monitor.c', include_directories: dppd_inc)
test('independent recovery monitor evidence', recovery_monitor_test)

# 真正的跨进程往返覆盖暂停超时、随机数与会话错配、消息边界和失败锁存
monitor_channel_test = executable('test_monitor_channel', 'test_monitor_channel.c', dependencies: dppd_core_dep)
test('recovery monitor handshake', monitor_channel_test)

# 内核进程身份、描述符传递和同一文件锁共同约束交接，暂停或主动解锁不能绕过
monitor_handoff_test = executable('test_monitor_handoff', 'test_monitor_handoff.c', dependencies: dppd_core_dep)
test('cooperative writer recovery handoff', monitor_handoff_test)

recovery_scope_test = executable('test_recovery_scope', 'test_recovery_scope.c', dependencies: dppd_core_dep)
test('managed recovery process restrictions', recovery_scope_test)

# 短写、同步失败、原子替换失败和日志篡改不能被当作已完成删除
cleanup_journal_test = executable('test_cleanup_journal', 'test_cleanup_journal.c',
  dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=pwrite64', '-Wl,--wrap=fsync', '-Wl,--wrap=rename', '-Wl,--wrap=renameat2'])
test('durable managed cleanup journal', cleanup_journal_test)

# 持久化意图、结果和删除事件，覆盖驱动前后写盘故障及清理时的真实 handle 回收
recovery_attempt_test = executable('test_recovery_attempt', 'test_recovery_attempt.c',
  dependencies: dppd_core_dep, link_args: ['-Wl,--wrap=fsync'])
test('durable recovery attempts', recovery_attempt_test)

# 共享对象的类型、精确引用与监护要求持久化，覆盖跨进程崩溃和记录写入故障
shared_recovery_journal_test = executable('test_shared_recovery_journal', 'test_shared_recovery_journal.c',
  dependencies: dppd_core_dep, link_args: ['-Wl,--wrap=fsync', '-Wl,--wrap=pwrite', '-Wl,--wrap=pwrite64'])
test('typed shared COUNT recovery journal', shared_recovery_journal_test)

# 正式后端先写意图再创建，部分成功和日志故障保留句柄，退出按引用顺序删除
shared_recovery_backend_test = executable('test_shared_recovery_backend', 'test_shared_recovery_backend.c',
  dependencies: dppd_core_dep, link_args: ['-Wl,--wrap=fsync'])
test('shared COUNT durable backend lifecycle', shared_recovery_backend_test)

# 验证随机标识先落盘、熵源故障、私有动作编译和禁止无标识降级
tap_owner_test = executable('test_tap_owner', 'test_tap_owner.c',
  dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=getrandom', '-Wl,--wrap=fsync', '-Wl,--wrap=rte_eth_dev_info_get',
              '-Wl,--wrap=rte_flow_validate', '-Wl,--wrap=rte_flow_create', '-Wl,--wrap=rte_flow_destroy'])
test('native TAP owner cookie', tap_owner_test)

# 预期内容必须独立于内核读回结果，验证字节序、掩码规范化和优先级映射边界
tap_content_test = executable('test_tap_content', 'test_tap_content.c', dependencies: dppd_core_dep)
test('TAP expected rule content', tap_content_test)

# 完成态环形历史、覆盖缺口、稳定分页、整数边界和真实隔离 socket 查询
rule_history_test = executable('test_rule_history', 'test_rule_history.c',
  dependencies: dppd_core_dep,
  link_args: ['-Wl,--wrap=clock_gettime', '-Wl,--wrap=calloc'])
test('rule failure history and pagination', rule_history_test)

dppctl_test = executable(
  'test_dppctl',
  'test_dppctl.c',
  '../../app/dppctl/routing_cli.c',
  '../../app/dppctl/conntrack_cli.c',
  '../../app/dppctl/transfer_cli.c',
  include_directories: dppd_inc,
)
test('dppctl batch update arguments', dppctl_test)

persistence_test = executable(
  'test_persistence',
  'test_persistence.c',
  dependencies: dppd_core_dep,
)
test('rule snapshot persistence', persistence_test)
