#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "dppd/device.h"
#include "dppd/management.h"

/** 仅本测试使用可控单调时钟，不靠实际等待五秒验证缓存过期 */
static uint64_t clock_ns = UINT64_C(1000000000);
static bool clock_failed, create_failed, remove_failed;
static unsigned int fail_calloc, validate_calls, create_calls;
static int validation_code;

int __wrap_clock_gettime(clockid_t clock_id, struct timespec *value);
void *__wrap_calloc(size_t count, size_t size);
void *__real_calloc(size_t count, size_t size);

/** 模拟时钟不可用和时间推进，生产程序仍使用正常时钟 */
int __wrap_clock_gettime(clockid_t clock_id, struct timespec *value)
{
    assert(clock_id == CLOCK_MONOTONIC);
    if (clock_failed) {
        errno = EIO;
        return -1;
    }
    value->tv_sec = (time_t)(clock_ns / UINT64_C(1000000000));
    value->tv_nsec = (long)(clock_ns % UINT64_C(1000000000));
    return 0;
}

/** 分配失败只限定在本线程指定的位置，用来检查初始化清理和查询期间没有新增分配 */
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_calloc != 0 && --fail_calloc == 0)
        return NULL;
    return __real_calloc(count, size);
}

/** 校验计数表示实际访问驱动的次数，缓存命中和画像查询都不能使它增加 */
static int fake_validate(uint16_t port_id, const struct dppd_rule *rule,
                         struct dppd_flow_error *error)
{
    assert(port_id == 5 || port_id == 6);
    assert(rule->fallback != DPPD_FALLBACK_SOFTWARE_ONLY);
    validate_calls++;
    if (error != NULL) {
        error->code = validation_code;
        snprintf(error->message, sizeof(error->message), "test validation %d", validation_code);
    }
    return validation_code;
}

/** 测试创建只产生一个本地假 handle，不创建真实网卡对象 */
static int fake_create(uint16_t port_id, const struct dppd_rule *rule,
                       struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    create_calls++;
    if (create_failed)
        return -EIO;
    memset(handle, 0, sizeof(*handle));
    handle->rule_id = rule->id;
    handle->rule_generation = rule->generation;
    handle->port_id = port_id;
    handle->flow = (struct rte_flow *)(uintptr_t)(rule->id + 1U);
    for (uint16_t i = 0; i < rule->nb_actions; ++i) {
        if (rule->actions[i].type == DPPD_ACTION_COUNT)
            handle->has_count = true;
    }
    return 0;
}

/** 删除失败保留 handle，让测试继续使用正式的清理重试行为 */
static int fake_remove(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    if (remove_failed)
        return -EIO;
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static const struct dppd_flow_api api = {
    .validate = fake_validate, .create = fake_create, .remove = fake_remove,
};
static const struct dppd_topology topology = {
    .nb_endpoints = 2,
    .endpoints = {{.ethdev_port_id = 5}, {.ethdev_port_id = 6}},
};

/** 规则包含掩码、L4 参数和 COUNT，避免只用完全没有参数的 DROP 检查缓存键 */
static struct dppd_rule make_rule(uint64_t id)
{
    struct dppd_rule rule = {0};

    rule.id = id;
    rule.fallback = DPPD_FALLBACK_REQUIRE_HARDWARE;
    rule.nb_matches = 3;
    rule.matches[0].type = DPPD_MATCH_ETH;
    rule.matches[1].type = DPPD_MATCH_IPV4;
    rule.matches[2].type = DPPD_MATCH_TCP;
    rule.matches[2].spec.l4.dst_be = 80;
    rule.matches[2].spec.l4.dst_mask_be = UINT16_MAX;
    rule.nb_actions = 2;
    rule.actions[0].type = DPPD_ACTION_COUNT;
    rule.actions[1].type = DPPD_ACTION_DROP;
    return rule;
}

/** 返回正式控制接口的探测结果，验证它没有偷偷进行安装或占用事务编号 */
static struct dppd_flow_probe_result probe(struct dppd_control_service *service,
                                          uint16_t port_id, const struct dppd_rule *rule,
                                          bool refresh)
{
    struct dppd_flow_probe_result result;
    uint64_t transaction = service->next_transaction_id;
    uint64_t generation = service->rules.generation;
    unsigned int created = create_calls;

    assert(dppd_control_probe_rule(service, port_id, rule, refresh, &result) == 0);
    assert(service->next_transaction_id == transaction && service->rules.generation == generation);
    assert(create_calls == created);
    return result;
}

/** 端口、有效规则内容和 ID 必须区分，generation、偏好与无效字节不应造成误判 */
static void test_keys_and_expiry(void)
{
    struct dppd_control_service service;
    struct dppd_rule rule = make_rule(100), changed;
    struct dppd_flow_probe_result result;
    unsigned int calls;

    assert(dppd_control_init(&service, &topology, 2, &api) == 0);
    result = probe(&service, 5, &rule, false);
    assert(!result.cached && result.status == DPPD_FLOW_PROBE_SUPPORTED && result.software_equivalent);
    calls = validate_calls;
    clock_ns += 123;
    result = probe(&service, 5, &rule, false);
    assert(result.cached && result.age_available && result.age_ns == 123 && validate_calls == calls);
    changed = rule;
    changed.generation = 42;
    changed.fallback = DPPD_FALLBACK_SOFTWARE_ONLY;
    changed.matches[0].spec.ethdev_port_id = 55;
    changed.actions[7].conf.mark_id = 77;
    assert(probe(&service, 5, &changed, false).cached && validate_calls == calls);
    assert(!probe(&service, 6, &rule, false).cached);

    /** 每次修改一种有效语义，保留 ID 还可防止不同 COUNT 编号借用彼此的结果 */
    for (unsigned int i = 0; i < 7; ++i) {
        changed = rule;
        switch (i) {
        case 0: changed.id++; break;
        case 1: changed.priority = 1; break;
        case 2: changed.group = 1; break;
        case 3: changed.domain = DPPD_RULE_DOMAIN_EGRESS; break;
        case 4: changed.matches[1].spec.ipv4.src_mask_be = UINT32_MAX; break;
        case 5: changed.matches[2].spec.l4.dst_be = 81; break;
        case 6: changed.actions[1].type = DPPD_ACTION_QUEUE;
                changed.actions[1].conf.queue_id = 1; break;
        }
        calls = validate_calls;
        assert(!probe(&service, 5, &changed, false).cached && validate_calls == calls + 1);
    }
    result = probe(&service, 5, &rule, true);
    assert(!result.cached);
    clock_ns += DPPD_FLOW_PROBE_CACHE_TTL_NS - 1;
    assert(probe(&service, 5, &rule, false).cached);
    clock_ns++;
    calls = validate_calls;
    assert(!probe(&service, 5, &rule, false).cached && validate_calls == calls + 1);
    assert(service.rules.count == 0 && dppd_rte_flow_backend_count(&service.rte_flow) == 0);
    assert(dppd_software_backend_count(&service.software) == 0);
    assert(dppd_control_fini(&service) == 0);
}

/** 不支持可短期缓存，临时错误和无效规则必须每次重新询问，刷新不能留下旧成功结果 */
static void test_errors_and_clock(void)
{
    const int errors[] = {-EINVAL, -EEXIST, -ENOMEM, -ENOSPC, -EAGAIN, -EBUSY, -EIO, -ENODEV};
    struct dppd_control_service service;
    struct dppd_rule rule = make_rule(200);
    struct dppd_flow_probe_result result;
    struct dppd_flow_probe_statistics statistics;
    unsigned int calls;

    assert(dppd_control_init(&service, &topology, 1, &api) == 0);
    for (unsigned int i = 0; i < 2; ++i) {
        validation_code = i == 0 ? -ENOTSUP : -ENOSYS;
        result = probe(&service, 5, &rule, true);
        assert(!result.cached && result.status == DPPD_FLOW_PROBE_UNSUPPORTED);
        calls = validate_calls;
        result = probe(&service, 5, &rule, false);
        assert(result.cached && result.validation_code == validation_code && validate_calls == calls);
    }
    for (unsigned int i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        validation_code = 0;
        assert(!probe(&service, 5, &rule, true).cached);
        validation_code = errors[i];
        calls = validate_calls;
        result = probe(&service, 5, &rule, true);
        assert(result.validation_code == errors[i]);
        assert(result.status == (i < 2 ? DPPD_FLOW_PROBE_REJECTED : DPPD_FLOW_PROBE_UNAVAILABLE));
        assert(!probe(&service, 5, &rule, false).cached && validate_calls == calls + 2);
    }
    validation_code = 0;
    assert(!probe(&service, 5, &rule, true).cached);
    clock_failed = true;
    calls = validate_calls;
    result = probe(&service, 5, &rule, false);
    assert(!result.cached && !result.age_available);
    assert(!probe(&service, 5, &rule, false).cached && validate_calls == calls + 2);
    assert(dppd_flow_probe_cache_statistics(service.rte_flow.probes, 5, &statistics) == 0);
    assert(statistics.cache_entries == 0);
    clock_failed = false;
    assert(!probe(&service, 5, &rule, false).cached);
    clock_ns = 0;
    assert(!probe(&service, 5, &rule, false).cached);
    assert(dppd_control_fini(&service) == 0);
}

/** 正式安装拒绝旧的成功或不支持答复，创建、删除和失败尝试都会使跨端口旧记录失效 */
static void test_install_invalidation(void)
{
    struct dppd_control_service service;
    struct dppd_rule rule = make_rule(300);
    struct dppd_control_apply_result applied;
    struct dppd_flow_probe_statistics statistics;
    struct dppd_flow_error error;
    unsigned int calls;

    assert(dppd_control_init(&service, &topology, 2, &api) == 0);
    validation_code = 0;
    assert(!probe(&service, 5, &rule, false).cached);
    validation_code = -ENOTSUP;
    calls = validate_calls;
    assert(dppd_control_apply(&service, 5, &rule, 0, &applied) == -ENOTSUP);
    assert(validate_calls == calls + 1 && service.rules.count == 0);
    assert(!probe(&service, 5, &rule, false).cached);
    assert(probe(&service, 5, &rule, false).cached);
    validation_code = 0;
    assert(!probe(&service, 6, &rule, false).cached);
    calls = validate_calls;
    assert(dppd_control_apply(&service, 5, &rule, 0, &applied) == 0);
    assert(validate_calls == calls + 1 && applied.plan.backend == DPPD_PLAN_BACKEND_RTE_FLOW);
    assert(dppd_flow_probe_cache_statistics(service.rte_flow.probes, 6, &statistics) == 0);
    assert(statistics.cache_entries == 0 && statistics.epoch > 1);

    /** 创建失败也使其他端口的旧诊断失效，但当前已发布规则版本保持不变 */
    assert(!probe(&service, 6, &rule, false).cached);
    rule.priority = 1;
    create_failed = true;
    assert(dppd_control_apply(&service, 5, &rule, 1, &applied) == -EIO);
    create_failed = false;
    assert(service.rules.generation == 1);
    assert(dppd_flow_probe_cache_statistics(service.rte_flow.probes, 6, &statistics) == 0);
    assert(statistics.cache_entries == 0);
    assert(!probe(&service, 6, &rule, false).cached);
    remove_failed = true;
    assert(dppd_rte_flow_backend_remove_version(&service.rte_flow, 300, 1, &error) == -EIO);
    remove_failed = false;
    assert(dppd_flow_probe_cache_statistics(service.rte_flow.probes, 6, &statistics) == 0);
    assert(statistics.cache_entries == 0);
    assert(dppd_control_fini(&service) == 0);
}

/** 探测数量与规则账本容量无关，固定槽位满时替换旧结果，查询期间不分配内存 */
static void test_capacity_and_initialization(void)
{
    struct dppd_control_service service;
    struct dppd_rte_flow_backend backend;
    struct dppd_flow_probe_statistics statistics;
    struct dppd_rule rule = make_rule(400);
    unsigned int calls;

    fail_calloc = 2;
    assert(dppd_rte_flow_backend_init(&backend, 2, &api) == -ENOMEM);
    assert(backend.objects == NULL && backend.probes == NULL && fail_calloc == 0);
    assert(dppd_control_init(&service, &topology, 1, &api) == 0);
    fail_calloc = 1;
    for (unsigned int i = 0; i <= DPPD_FLOW_PROBE_CACHE_CAPACITY; ++i) {
        rule.id = 400 + i;
        assert(!probe(&service, 5, &rule, false).cached);
    }
    assert(fail_calloc == 1);
    fail_calloc = 0;
    assert(dppd_flow_probe_cache_statistics(service.rte_flow.probes, 5, &statistics) == 0);
    assert(statistics.cache_entries == DPPD_FLOW_PROBE_CACHE_CAPACITY);
    rule.id = 400;
    calls = validate_calls;
    assert(!probe(&service, 5, &rule, false).cached && validate_calls == calls + 1);
    assert(dppd_flow_probe_cache_clear(service.rte_flow.probes, 5) == 0);
    assert(dppd_flow_probe_cache_statistics(service.rte_flow.probes, 5, &statistics) == 0);
    assert(statistics.cache_entries == 0 && statistics.validations != 0);
    assert(dppd_control_fini(&service) == 0);
}

/** 用正式 socket 验证无指针画像报文，同时检查隔离、保留位和已移除端口的边界 */
static void test_management_profile(void)
{
    struct dppd_control_service service;
    struct dppd_device_set devices = {0};
    struct dppd_management_server server;
    struct dppd_management_request request = {0};
    struct dppd_management_response response, original;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    char path[108];
    unsigned int calls = validate_calls;
    int client;

    assert(dppd_control_init(&service, &topology, 1, &api) == 0);
    devices.topology = topology;
    devices.nb_ports = 1;
    devices.ports[0].port_id = 5;
    devices.ports[0].started = true;
    devices.ports[0].identity.rx_desc_min = 32;
    devices.ports[0].identity.configured_rx_desc = 512;
    devices.ports[0].identity.firmware_error = -ENOTSUP;
    atomic_init(&devices.removal_requested, false);
    atomic_init(&devices.ports[0].removed, false);
    atomic_init(&devices.ports[0].link_state, DPPD_LINK_UP);
    request.version = DPPD_MANAGEMENT_VERSION;
    request.operation = DPPD_MANAGEMENT_CAPABILITY_GET;
    request.size = sizeof(request);
    request.request_id = 19;
    request.payload.capability.port_id = 5;
    assert(dppd_management_handle(&service, &devices, NULL, &request, &original) == 0);
    assert(original.status == 0 && original.payload.capability.identity.rx_desc_min == 32);
    assert(original.payload.capability.identity.configured_rx_desc == 512);
    assert(!original.payload.capability.identity.firmware_known);
    for (unsigned int i = 0; i < 3; ++i) {
        assert(dppd_management_handle(&service, &devices, NULL, &request, &response) == 0);
        assert(memcmp(&response, &original, sizeof(response)) == 0);
    }
    snprintf(path, sizeof(path), "/tmp/dppd-capability-%ld.sock", (long)getpid());
    assert(dppd_management_start(&server, &service, &devices, NULL, path) == 0);
    client = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(client >= 0);
    memcpy(address.sun_path, path, strlen(path) + 1U);
    assert(connect(client, (const struct sockaddr *)&address, sizeof(address)) == 0);
    assert(send(client, &request, sizeof(request), 0) == (ssize_t)sizeof(request));
    assert(dppd_management_poll(&server) == 0);
    assert(recv(client, &response, sizeof(response), 0) == (ssize_t)sizeof(response));
    assert(memcmp(&response, &original, sizeof(response)) == 0);
    close(client);
    dppd_management_stop(&server);
    request.version = 11;
    assert(dppd_management_handle(&service, &devices, NULL, &request, &response) == 0);
    assert(response.status == -EPROTO);
    request.version = DPPD_MANAGEMENT_VERSION;
    request.payload.capability.reserved[0] = 1;
    assert(dppd_management_handle(&service, &devices, NULL, &request, &response) == 0);
    assert(response.status == -EINVAL);
    request.payload.capability.reserved[0] = 0;
    service.recovery_state = DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED;
    assert(dppd_management_handle(&service, &devices, NULL, &request, &response) == 0);
    assert(response.status == 0);
    request.operation = DPPD_MANAGEMENT_CAPABILITY_CLEAR;
    assert(dppd_management_handle(&service, &devices, NULL, &request, &response) == 0);
    assert(response.status == -EUCLEAN);
    request.operation = DPPD_MANAGEMENT_CAPABILITY_PROBE;
    memset(&request.payload, 0, sizeof(request.payload));
    request.payload.probe.install_port_id = 5;
    request.payload.probe.rule = make_rule(500);
    assert(dppd_management_handle(&service, &devices, NULL, &request, &response) == 0);
    assert(response.status == -EUCLEAN);
    service.recovery_state = DPPD_CONTROL_RECOVERY_READY;
    atomic_store(&devices.ports[0].removed, true);
    assert(dppd_management_handle(&service, &devices, NULL, &request, &response) == 0);
    assert(response.status == -ENODEV);
    assert(validate_calls == calls && service.rules.count == 0 && service.next_transaction_id == 1);
    assert(dppd_control_fini(&service) == 0);
}

/** 测试不启动 EAL，不需要网卡和 root，实际缓存、事务、画像与 socket 使用正式代码 */
int main(void)
{
    test_keys_and_expiry();
    test_errors_and_clock();
    test_install_invalidation();
    test_capacity_and_initialization();
    test_management_profile();
    return 0;
}
