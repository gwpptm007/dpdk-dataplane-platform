#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dppd/device.h"
#include "dppd/management.h"

/** 模拟驱动只负责确定性故障，管理分发、正式事务和端口快照都使用产品实现 */
static unsigned int validates, creates, removes, queries;
static int create_failure = -1, query_error;
static bool remove_failure;

static int fake_validate(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_error *error)
{
    (void)error;
    assert(port == 5 && rule->domain == DPPD_RULE_DOMAIN_TRANSFER);
    validates++;
    return 0;
}

static int fake_create(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_handle *handle,
    struct dppd_flow_error *error)
{
    (void)error;
    unsigned int call = creates++;
    memset(handle, 0, sizeof(*handle));
    if ((int)call == create_failure)
        return -ENOSPC;
    handle->rule_id = rule->id;
    handle->rule_generation = rule->generation;
    handle->port_id = port;
    handle->flow = (struct rte_flow *)(uintptr_t)(call + 1U);
    for (uint16_t i = 0; i < rule->nb_actions; ++i)
        handle->has_count |= rule->actions[i].type == DPPD_ACTION_COUNT;
    return 0;
}

static int fake_remove(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    assert(handle->flow != NULL);
    removes++;
    if (remove_failure)
        return -EBUSY;
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static int fake_query(const struct dppd_flow_handle *handle, uint64_t *hits, uint64_t *bytes,
    struct dppd_flow_error *error)
{
    (void)error;
    assert(handle->has_count && handle->flow != NULL);
    queries++;
    if (query_error != 0)
        return query_error;
    *hits = UINT64_C(0x100000009);
    *bytes = UINT64_C(0x200000017);
    return 0;
}

static const struct dppd_flow_api api = {.validate = fake_validate,
    .create = fake_create,
    .remove = fake_remove,
    .query_count = fake_query};

static void initialize_request(struct dppd_management_request *request, enum dppd_management_operation operation)
{
    memset(request, 0, sizeof(*request));
    request->version = DPPD_MANAGEMENT_VERSION;
    request->size = sizeof(*request);
    request->operation = operation;
    request->request_id = UINT64_C(0x100000007);
}

static void transfer_request(struct dppd_management_request *request, enum dppd_management_operation operation)
{
    initialize_request(request, operation);
    request->payload.transfer = (struct dppd_transfer_spec){.first_rule_id = 100,
        .expected_repository_generation = operation == DPPD_MANAGEMENT_TRANSFER_PLAN ? DPPD_RULE_GENERATION_ANY : 0,
        .priority = 17,
        .install_port_id = 5,
        .source_port_id = 5,
        .destination_port_id = 6,
        .scenario = DPPD_TRANSFER_UPLINK_VF,
        .bidirectional = 1,
        .with_count = 1};
}

static void dispatch(struct dppd_control_service *control, const struct dppd_device_set *devices,
    const struct dppd_management_request *request, struct dppd_management_response *response, int status)
{
    memset(response, 0xa5, sizeof(*response));
    assert(dppd_management_handle(control, devices, NULL, request, response) == 0);
    assert(response->status == status && response->version == DPPD_MANAGEMENT_VERSION);
    assert(response->size == sizeof(*response) && response->operation == request->operation);
    assert(response->request_id == request->request_id);
}

/** 可识别角色经正式解析器生成，未知端口保留错误，不能按端口顺序被解释成 PF */
static void make_devices(struct dppd_device_set *devices)
{
    memset(devices, 0, sizeof(*devices));
    devices->nb_ports = devices->topology.nb_endpoints = 3;
    for (uint16_t i = 0; i < 3; ++i) {
        struct dppd_port *port = &devices->ports[i];
        struct dppd_endpoint *endpoint = &devices->topology.endpoints[i];
        port->port_id = endpoint->ethdev_port_id = 5 + i;
        port->peer_port_id = i == 0 ? 6 : 5;
        port->configured = port->started = true;
        atomic_init(&port->link_state, DPPD_LINK_UP);
        atomic_init(&port->removed, false);
        snprintf(port->identity.device_name, sizeof(port->identity.device_name), "0000:03:00.%u", i);
        snprintf(port->identity.dpdk_version, sizeof(port->identity.dpdk_version), "test-driver-only");
        port->identity.device_name_known = true;
        endpoint->kind = i == 1 ? DPPD_ENDPOINT_REPRESENTOR : DPPD_ENDPOINT_ETHDEV;
        endpoint->has_switch_domain = true;
        endpoint->switch_domain_id = 42;
        endpoint->switch_port_id = 100 + i;
        endpoint->if_index = 200 + i;
        snprintf(endpoint->interface_name, sizeof(endpoint->interface_name), "test%u", i);
        snprintf(endpoint->device_name, sizeof(endpoint->device_name), "0000:03:00.%u", i);
        snprintf(endpoint->driver_name, sizeof(endpoint->driver_name), "%s", i == 2 ? "net_vmxnet3" : "net_mlx5");
        if (i != 2)
            assert(dppd_topology_resolve_role(endpoint, "net_mlx5", i == 1, i == 0 ? "p2" : "c7pf3vf9") == 0);
        else
            assert(dppd_topology_resolve_role(endpoint, "net_vmxnet3", false, "0") == -ENOTSUP);
    }
}

/** 两个公开查询都必须完整保留角色来源、编号和原始身份，查询不会重新触碰驱动 */
static void identity_queries(struct dppd_control_service *control, const struct dppd_device_set *devices)
{
    struct dppd_management_request request;
    struct dppd_management_response response;
    for (unsigned int i = 0; i < devices->nb_ports; ++i)
        for (unsigned int capability = 0; capability < 2; ++capability) {
            initialize_request(&request, capability ? DPPD_MANAGEMENT_CAPABILITY_GET : DPPD_MANAGEMENT_PORT_GET);
            if (capability)
                request.payload.capability.port_id = devices->ports[i].port_id;
            else
                request.payload.port_get.port_id = devices->ports[i].port_id;
            dispatch(control, devices, &request, &response, 0);
            const struct dppd_management_port_info *port =
                capability ? &response.payload.capability.port : &response.payload.port;
            const struct dppd_endpoint *endpoint = &devices->topology.endpoints[i];
            assert(port->port_id == endpoint->ethdev_port_id && port->endpoint_kind == endpoint->kind);
            assert(port->role == endpoint->role && port->role_source == endpoint->role_source);
            assert(port->role_error == endpoint->role_error && port->if_index == endpoint->if_index);
            assert(port->has_controller == endpoint->has_controller && port->has_pf == endpoint->has_pf);
            assert(
                port->has_function == endpoint->has_function && port->has_physical_port == endpoint->has_physical_port);
            assert(port->controller_id == endpoint->controller_id && port->pf_id == endpoint->pf_id);
            assert(port->function_id == endpoint->function_id && port->physical_port_id == endpoint->physical_port_id);
            assert(strcmp(port->interface_name, endpoint->interface_name) == 0);
            assert(strcmp(port->driver_name, endpoint->driver_name) == 0);
            assert(strcmp(port->device_name, endpoint->device_name) == 0);
            assert(strcmp(port->phys_port_name, endpoint->phys_port_name) == 0);
            if (i == 2)
                assert(port->role == DPPD_ENDPOINT_ROLE_UNKNOWN && port->role_error == -ENOTSUP);
            if (capability)
                assert(strcmp(response.payload.capability.identity.dpdk_version, "test-driver-only") == 0);
        }
    assert(validates == 0 && creates == 0 && removes == 0);
}

/** 正式分发验证只读规划、精确版本安装及 COUNT，不把模拟成功描述成真实硬件验收 */
static void successful_requests(struct dppd_control_service *control, const struct dppd_device_set *devices)
{
    struct dppd_management_request request;
    struct dppd_management_response response;
    transfer_request(&request, DPPD_MANAGEMENT_TRANSFER_PLAN);
    dispatch(control, devices, &request, &response, 0);
    assert(response.payload.transfer.graph.count == 2 && response.payload.transfer.graph.repository_generation == 0);
    assert(response.payload.transfer.graph.rules[0].matches[0].spec.ethdev_port_id == 5);
    assert(response.payload.transfer.graph.rules[1].matches[0].spec.ethdev_port_id == 6);
    assert(response.payload.transfer.results[0].transaction_id == 0);
    assert(response.payload.transfer.message[0] != '\0');
    assert(validates == 0 && creates == 0 && control->rules.generation == 0);
    request.operation = DPPD_MANAGEMENT_TRANSFER_APPLY;
    dispatch(control, devices, &request, &response, -ESTALE);
    request.payload.transfer.expected_repository_generation = 1;
    dispatch(control, devices, &request, &response, -ESTALE);
    assert(validates == 0 && creates == 0);
    request.payload.transfer.expected_repository_generation = 0;
    dispatch(control, devices, &request, &response, 0);
    assert(response.payload.transfer.graph.repository_generation == 2 && control->rules.generation == 2);
    assert(validates == 2 && creates == 2);
    for (unsigned int i = 0; i < 2; ++i) {
        const struct dppd_rule *rule = &response.payload.transfer.graph.rules[i];
        assert(rule->generation == i + 1U);
        assert(response.payload.transfer.graph.plans[i].rule_generation == rule->generation);
        assert(response.payload.transfer.results[i].generation == rule->generation);
        assert(response.payload.transfer.results[i].transaction_id == 1);
    }
    initialize_request(&request, DPPD_MANAGEMENT_RULE_COUNT_QUERY);
    request.payload.count_query.rule_id = 100;
    request.payload.count_query.expected_generation = 1;
    dispatch(control, devices, &request, &response, 0);
    assert(response.payload.count.hits == UINT64_C(0x100000009));
    assert(response.payload.count.bytes == UINT64_C(0x200000017) && queries == 1);
    query_error = -ENODATA;
    dispatch(control, devices, &request, &response, -ENODATA);
    assert(response.payload.count.rule_id == 0 && response.payload.count.generation == 0);
    query_error = 0;
    transfer_request(&request, DPPD_MANAGEMENT_TRANSFER_APPLY);
    request.payload.transfer.first_rule_id = 200;
    dispatch(control, devices, &request, &response, -ESTALE);
    assert(validates == 2 && creates == 2);
}

/** 旧协议和每个非零保留字节都在驱动前拒绝，错误答复不残留上次成功的图 */
static void invalid_requests(struct dppd_control_service *control, const struct dppd_device_set *devices)
{
    struct dppd_management_request request;
    struct dppd_management_response response;
    transfer_request(&request, DPPD_MANAGEMENT_TRANSFER_PLAN);
    request.payload.transfer.first_rule_id = 200;
    for (unsigned int i = 0; i < sizeof(request.payload.transfer.reserved); ++i) {
        request.payload.transfer.reserved[i] = 1;
        dispatch(control, devices, &request, &response, -EINVAL);
        assert(response.payload.transfer.graph.count == 0);
        request.payload.transfer.reserved[i] = 0;
    }
    request.version = DPPD_MANAGEMENT_VERSION - 1U;
    dispatch(control, devices, &request, &response, -EPROTO);
    assert(response.payload.transfer.graph.count == 0);
    request.version = DPPD_MANAGEMENT_VERSION;
    request.size--;
    dispatch(control, devices, &request, &response, -EPROTO);
    assert(validates == 2 && creates == 2);
}

/** 创建中途失败且删除失败时，公开管理接口保留只读计划，阻断写入并支持正式恢复重试 */
static void recovery_requests(const struct dppd_device_set *devices)
{
    struct dppd_control_service control;
    struct dppd_management_request request;
    struct dppd_management_response response;
    creates = validates = removes = queries = 0;
    create_failure = 1;
    remove_failure = true;
    assert(dppd_control_init(&control, &devices->topology, 8, &api) == 0);
    transfer_request(&request, DPPD_MANAGEMENT_TRANSFER_APPLY);
    dispatch(&control, devices, &request, &response, -EUCLEAN);
    assert(response.payload.transfer.message[0] != '\0');
    assert(control.recovery_state == DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED);
    assert(control.rules.generation == 0 && control.rules.count == 0);
    assert(creates == 2 && removes == 1 && dppd_rte_flow_backend_count(&control.rte_flow) == 1);
    transfer_request(&request, DPPD_MANAGEMENT_TRANSFER_PLAN);
    request.payload.transfer.first_rule_id = 200;
    dispatch(&control, devices, &request, &response, 0);
    assert(response.payload.transfer.graph.count == 2 && creates == 2 && validates == 2);
    request.operation = DPPD_MANAGEMENT_TRANSFER_APPLY;
    request.payload.transfer.expected_repository_generation = 0;
    dispatch(&control, devices, &request, &response, -EUCLEAN);
    assert(response.payload.transfer.graph.count == 0 && creates == 2 && validates == 2);
    initialize_request(&request, DPPD_MANAGEMENT_CAPABILITY_GET);
    request.payload.capability.port_id = 7;
    dispatch(&control, devices, &request, &response, 0);
    assert(response.payload.capability.port.role == DPPD_ENDPOINT_ROLE_UNKNOWN);
    remove_failure = false;
    initialize_request(&request, DPPD_MANAGEMENT_RECOVERY_RETRY);
    dispatch(&control, devices, &request, &response, 0);
    assert(removes == 2 && control.recovery_state == DPPD_CONTROL_RECOVERY_RESTART_REQUIRED);
    assert(dppd_rte_flow_backend_count(&control.rte_flow) == 0);
    transfer_request(&request, DPPD_MANAGEMENT_TRANSFER_PLAN);
    dispatch(&control, devices, &request, &response, 0);
    request.operation = DPPD_MANAGEMENT_TRANSFER_APPLY;
    request.payload.transfer.expected_repository_generation = 0;
    dispatch(&control, devices, &request, &response, -EUCLEAN);
    assert(dppd_control_fini(&control) == 0);
}

/** 通用身份不授予角色，管理查询完整保留 UNKNOWN，只有 pair 可以进入正式硬件事务 */
static void generic_identity_requests(void)
{
    struct dppd_device_set devices;
    struct dppd_control_service control;
    struct dppd_management_request request;
    struct dppd_management_response response;
    make_devices(&devices);
    for (uint16_t i = 0; i < devices.nb_ports; ++i) {
        struct dppd_endpoint *endpoint = &devices.topology.endpoints[i];
        snprintf(endpoint->driver_name, sizeof(endpoint->driver_name), "net_contract");
        assert(dppd_topology_resolve_role(endpoint, endpoint->driver_name, endpoint->kind == DPPD_ENDPOINT_REPRESENTOR,
                   i == 2 ? "pf0vf9" : "") == -ENOTSUP);
    }
    creates = validates = removes = queries = 0;
    create_failure = -1;
    query_error = 0;
    remove_failure = false;
    assert(dppd_control_init(&control, &devices.topology, 8, &api) == 0);
    identity_queries(&control, &devices);
    transfer_request(&request, DPPD_MANAGEMENT_TRANSFER_PLAN);
    request.payload.transfer.scenario = DPPD_TRANSFER_PAIR;
    dispatch(&control, &devices, &request, &response, 0);
    assert(response.payload.transfer.graph.count == 2 && response.payload.transfer.graph.repository_generation == 0);
    assert(creates == 0 && validates == 0 && control.rules.generation == 0);
    request.operation = DPPD_MANAGEMENT_TRANSFER_APPLY;
    request.payload.transfer.expected_repository_generation = response.payload.transfer.graph.repository_generation;
    dispatch(&control, &devices, &request, &response, 0);
    assert(creates == 2 && validates == 2 && control.rules.generation == 2 && control.rules.count == 2);
    struct dppd_flow_handle handles[2];
    for (uint64_t i = 0; i < 2; ++i) {
        const struct dppd_rule *rule = &response.payload.transfer.graph.rules[i];
        assert(rule->id == 100U + i && rule->generation == 1U + i);
        assert(response.payload.transfer.graph.plans[i].rule_generation == rule->generation);
        handles[i] = *dppd_rte_flow_backend_find_version(&control.rte_flow, rule->id, rule->generation);
    }
    for (uint64_t i = 0; i < 2; ++i) {
        initialize_request(&request, DPPD_MANAGEMENT_RULE_COUNT_QUERY);
        request.payload.count_query.rule_id = 100U + i;
        request.payload.count_query.expected_generation = 1U + i;
        dispatch(&control, &devices, &request, &response, 0);
        assert(response.payload.count.rule_id == 100U + i && response.payload.count.generation == 1U + i);
        assert(response.payload.count.hits == UINT64_C(0x100000009) &&
            response.payload.count.bytes == UINT64_C(0x200000017));
    }
    assert(queries == 2);
    request.payload.count_query.expected_generation = 99;
    dispatch(&control, &devices, &request, &response, -ESTALE);
    assert(queries == 2 && response.payload.count.rule_id == 0 && response.payload.count.generation == 0);
    for (uint8_t scenario = DPPD_TRANSFER_UPLINK_VF; scenario < DPPD_TRANSFER_TEMPLATE_COUNT; ++scenario) {
        transfer_request(&request, DPPD_MANAGEMENT_TRANSFER_PLAN);
        request.payload.transfer.first_rule_id = 200;
        request.payload.transfer.expected_repository_generation = 2;
        request.payload.transfer.scenario = scenario;
        dispatch(&control, &devices, &request, &response, -ENOTSUP);
        assert(response.payload.transfer.graph.count == 0);
        request.operation = DPPD_MANAGEMENT_TRANSFER_APPLY;
        dispatch(&control, &devices, &request, &response, -ENOTSUP);
        assert(response.payload.transfer.graph.count == 0);
    }
    struct dppd_topology saved = devices.topology;
    for (unsigned int variant = 0; variant < 3; ++variant) {
        devices.topology = saved;
        if (variant == 0)
            devices.topology.endpoints[1].has_switch_domain = false;
        else if (variant == 1)
            devices.topology.endpoints[1].switch_domain_id++;
        else
            devices.topology.endpoints[1].switch_port_id = devices.topology.endpoints[0].switch_port_id;
        transfer_request(&request, DPPD_MANAGEMENT_TRANSFER_PLAN);
        request.payload.transfer.first_rule_id = 200;
        request.payload.transfer.expected_repository_generation = 2;
        request.payload.transfer.scenario = DPPD_TRANSFER_PAIR;
        int expected = variant == 0 ? -ENOTSUP : variant == 1 ? -EXDEV : -EINVAL;
        dispatch(&control, &devices, &request, &response, expected);
        assert(response.payload.transfer.graph.count == 0);
        request.operation = DPPD_MANAGEMENT_TRANSFER_APPLY;
        dispatch(&control, &devices, &request, &response, expected);
        assert(response.payload.transfer.graph.count == 0);
    }
    devices.topology = saved;
    assert(creates == 2 && validates == 2 && removes == 0 && queries == 2);
    assert(control.rules.count == 2 && control.rules.generation == 2 && control.shared_count_revision == 0);
    assert(control.recovery_state == DPPD_CONTROL_RECOVERY_READY);
    for (uint64_t i = 0; i < 2; ++i) {
        const struct dppd_flow_handle *handle = dppd_rte_flow_backend_find_version(&control.rte_flow, 100U + i, 1U + i);
        assert(handle != NULL && memcmp(handle, &handles[i], sizeof(*handle)) == 0);
    }
    for (uint16_t i = 0; i < devices.nb_ports; ++i) {
        const struct dppd_endpoint *endpoint = &devices.topology.endpoints[i];
        assert(
            endpoint->role == DPPD_ENDPOINT_ROLE_UNKNOWN && endpoint->role_source == DPPD_ENDPOINT_ROLE_SOURCE_UNKNOWN);
        assert(endpoint->role_error == -ENOTSUP && !endpoint->has_controller && !endpoint->has_pf &&
            !endpoint->has_function && !endpoint->has_physical_port);
        for (unsigned int capability = 0; capability < 2; ++capability) {
            initialize_request(&request, capability ? DPPD_MANAGEMENT_CAPABILITY_GET : DPPD_MANAGEMENT_PORT_GET);
            if (capability)
                request.payload.capability.port_id = devices.ports[i].port_id;
            else
                request.payload.port_get.port_id = devices.ports[i].port_id;
            dispatch(&control, &devices, &request, &response, 0);
            const struct dppd_management_port_info *port =
                capability ? &response.payload.capability.port : &response.payload.port;
            assert(port->role == DPPD_ENDPOINT_ROLE_UNKNOWN && port->role_source == DPPD_ENDPOINT_ROLE_SOURCE_UNKNOWN &&
                port->role_error == -ENOTSUP && port->if_index == endpoint->if_index);
            assert(strcmp(port->driver_name, "net_contract") == 0 &&
                strcmp(port->interface_name, endpoint->interface_name) == 0 &&
                strcmp(port->device_name, endpoint->device_name) == 0 &&
                strcmp(port->phys_port_name, endpoint->phys_port_name) == 0);
        }
    }
    assert(creates == 2 && validates == 2 && removes == 0 && queries == 2);
    assert(dppd_control_fini(&control) == 0 && removes == 2);
}

int main(void)
{
    struct dppd_device_set devices;
    struct dppd_control_service control;
    make_devices(&devices);
    assert(dppd_control_init(&control, &devices.topology, 8, &api) == 0);
    identity_queries(&control, &devices);
    successful_requests(&control, &devices);
    invalid_requests(&control, &devices);
    assert(dppd_control_fini(&control) == 0);
    recovery_requests(&devices);
    generic_identity_requests();
    puts("PASS transfer management with simulated driver; hardware acceptance remains pending");
    return 0;
}
