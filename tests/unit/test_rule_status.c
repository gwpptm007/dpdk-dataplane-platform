#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "dppd/device.h"
#include "dppd/install_time.h"
#include "dppd/management.h"

/**
 * 只包装本测试程序的时钟引用，生产程序仍调用真实单调时钟
 * 两个预设样本分别代表提交开始和结束，便于稳定验证跨秒、时钟失败和重启后的重新计时
 */
static struct timespec clock_samples[2];
static unsigned int clock_index, clock_limit;
static int clock_failure = -1;
static unsigned int driver_calls;
static bool validation_unsupported;

int __real_clock_gettime(clockid_t clock_id, struct timespec *value);
int __wrap_clock_gettime(clockid_t clock_id, struct timespec *value);

/** 没有配置测试样本时透传真实时钟，避免影响库的其他使用场景 */
int __wrap_clock_gettime(clockid_t clock_id, struct timespec *value)
{
    if (clock_index < clock_limit) {
        unsigned int index = clock_index++;

        assert(clock_id == CLOCK_MONOTONIC);
        if ((int)index == clock_failure) {
            errno = EIO;
            return -1;
        }
        *value = clock_samples[index];
        return 0;
    }
    return __real_clock_gettime(clock_id, value);
}

/** 开始时间位于上一秒末尾，结束时间位于下一秒开头，正常情况下差值恰好为一百纳秒 */
static void set_clock(int failure)
{
    clock_samples[0] = (struct timespec){.tv_sec = 2, .tv_nsec = 999999950};
    clock_samples[1] = (struct timespec){.tv_sec = 3, .tv_nsec = 50};
    clock_index = 0;
    clock_limit = failure == 0 ? 1U : 2U;
    clock_failure = failure;
}

/** 驱动接口只记录调用次数，不访问设备，状态查询必须保持这个次数不变 */
static int fake_validate(uint16_t port_id, const struct dppd_rule *rule,
                         struct dppd_flow_error *error)
{
    (void)port_id;
    (void)rule;
    (void)error;
    driver_calls++;
    return validation_unsupported ? -ENOTSUP : 0;
}

/** 模拟成功创建并填写真实接口要求的对象身份和 COUNT 配置 */
static int fake_create(uint16_t port_id, const struct dppd_rule *rule,
                       struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    driver_calls++;
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

/** 模拟删除，清空 handle 表示后端不再持有实际对象 */
static int fake_remove(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    driver_calls++;
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

/** 生成最小的合法入口 DROP 规则，是否有 COUNT 与后端选择分别由用例决定 */
static struct dppd_rule make_rule(uint64_t id, enum dppd_fallback_policy fallback,
                                  bool has_count)
{
    struct dppd_rule rule = {0};

    rule.id = id;
    rule.fallback = fallback;
    rule.nb_matches = 1;
    rule.matches[0].type = DPPD_MATCH_ETH;
    rule.nb_actions = has_count ? 2 : 1;
    rule.actions[0].type = has_count ? DPPD_ACTION_COUNT : DPPD_ACTION_DROP;
    if (has_count)
        rule.actions[1].type = DPPD_ACTION_DROP;
    return rule;
}

/** 用真实管理 socket 往返状态请求，验证新增操作的报文映射和响应版本 */
static void socket_status(struct dppd_control_service *service,
                           const struct dppd_control_rule_status *expected)
{
    struct dppd_management_server server;
    struct dppd_device_set devices = {0};
    struct dppd_management_request request = {0};
    struct dppd_management_response response;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    char path[108];
    int client;

    snprintf(path, sizeof(path), "/tmp/dppd-rule-status-%ld.sock", (long)getpid());
    assert(dppd_management_start(&server, service, &devices, NULL, path) == 0);
    client = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(client >= 0);
    memcpy(address.sun_path, path, strlen(path) + 1U);
    assert(connect(client, (const struct sockaddr *)&address, sizeof(address)) == 0);
    request.version = DPPD_MANAGEMENT_VERSION;
    request.operation = DPPD_MANAGEMENT_RULE_STATUS;
    request.size = sizeof(request);
    request.request_id = 17;
    request.payload.rule_status.rule_id = expected->installation.rule_id;
    request.payload.rule_status.expected_generation = expected->installation.generation;
    assert(send(client, &request, sizeof(request), 0) == (ssize_t)sizeof(request));
    assert(dppd_management_poll(&server) == 0);
    assert(recv(client, &response, sizeof(response), 0) == (ssize_t)sizeof(response));
    assert(response.status == 0 && response.request_id == 17);
    assert(response.version == DPPD_MANAGEMENT_VERSION);
    assert(memcmp(&response.payload.rule_status, expected, sizeof(*expected)) == 0);
    close(client);
    dppd_management_stop(&server);

    /** v10 不能解释新增操作，即使结构体大小巧合相同，也必须明确返回协议错误 */
    request.version = 10;
    assert(dppd_management_handle(service, NULL, NULL, &request, &response) == 0);
    assert(response.status == -EPROTO);
    request.version = DPPD_MANAGEMENT_VERSION;
    service->recovery_state = DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED;
    assert(dppd_management_handle(service, NULL, NULL, &request, &response) == 0);
    assert(response.status == -EUCLEAN);
    service->recovery_state = DPPD_CONTROL_RECOVERY_READY;
}

/** 硬件安装、prefer 降级和强制软件三种路径都应显示真实位置，并保持查询只读 */
static void test_backends(void)
{
    for (unsigned int mode = 0; mode < 3; ++mode) {
        struct dppd_control_service service;
        struct dppd_control_apply_result applied;
        struct dppd_control_rule_status original, queried;
        struct dppd_rule rule = make_rule(100 + mode,
            mode == 2 ? DPPD_FALLBACK_SOFTWARE_ONLY : DPPD_FALLBACK_PREFER_HARDWARE,
            mode != 2);
        uint64_t next_transaction;
        unsigned int calls;

        validation_unsupported = mode == 1;
        assert(dppd_control_init(&service, &topology, 4, &api) == 0);
        set_clock(-1);
        assert(dppd_control_apply(&service, 5, &rule, 0, &applied) == 0);
        assert(clock_index == 2);
        assert(dppd_control_rule_status(&service, rule.id, 1, &original) == 0);
        assert(original.installation.rule_id == rule.id);
        assert(original.installation.generation == 1);
        assert(original.installation.install_port_id == 5);
        assert(original.installation.has_count == (mode != 2));
        assert(original.installation.timing_available);
        assert(original.installation.install_duration_ns == 100);
        assert(original.installation.commit_rule_count == 1);
        assert(original.backend == (mode == 0 ? DPPD_PLAN_BACKEND_RTE_FLOW :
                                                DPPD_PLAN_BACKEND_SOFTWARE));
        assert(original.fallback == rule.fallback);
        assert(!original.persistence.enabled && !original.persistence.dirty);
        calls = driver_calls;
        next_transaction = service.next_transaction_id;
        for (unsigned int i = 0; i < 5; ++i) {
            assert(dppd_control_rule_status(&service, rule.id,
                                            DPPD_RULE_GENERATION_ANY, &queried) == 0);
            assert(memcmp(&queried, &original, sizeof(queried)) == 0);
        }
        socket_status(&service, &original);
        assert(driver_calls == calls && service.next_transaction_id == next_transaction);
        assert(dppd_rule_repository_generation(&service.rules) == 1);
        assert(dppd_control_rule_status(&service, rule.id, 2, &queried) == -ESTALE);
        assert(dppd_control_rule_status(&service, 999, DPPD_RULE_GENERATION_ANY,
                                        &queried) == -ENOENT);
        assert(dppd_control_rule_status(&service, rule.id, 0, &queried) == -EINVAL);
        assert(dppd_control_rule_status(&service, 0, 1, &queried) == -EINVAL);
        assert(dppd_control_rule_status(&service, rule.id, 1, NULL) == -EINVAL);

        /** 完全相同的 apply 不重新安装，原始安装记录应完整保留 */
        assert(dppd_control_apply(&service, 5, &rule, 1, &applied) == 0);
        assert(applied.status == DPPD_RULE_UNCHANGED);
        assert(dppd_control_rule_status(&service, rule.id, 1, &queried) == 0);
        assert(memcmp(&queried, &original, sizeof(queried)) == 0);
        assert(dppd_control_fini(&service) == 0);
    }
    validation_unsupported = false;
}

/** 时钟开始或结束采样失败时，规则仍安装成功，状态明确标记耗时不可用 */
static void test_clock_failures(void)
{
    for (unsigned int software = 0; software < 2; ++software) {
        for (int failure = 0; failure < 2; ++failure) {
            struct dppd_control_service service;
            struct dppd_control_apply_result applied;
            struct dppd_control_rule_status status;
            struct dppd_rule rule = make_rule(200,
                software ? DPPD_FALLBACK_SOFTWARE_ONLY : DPPD_FALLBACK_REQUIRE_HARDWARE,
                false);

            assert(dppd_control_init(&service, &topology, 4, &api) == 0);
            set_clock(failure);
            assert(dppd_control_apply(&service, 5, &rule, 0, &applied) == 0);
            assert(clock_index == clock_limit);
            assert(dppd_control_rule_status(&service, 200, 1, &status) == 0);
            assert(!status.installation.timing_available);
            assert(status.installation.install_duration_ns == 0);
            assert(dppd_control_fini(&service) == 0);
        }
    }
}

/** 时钟不可用的整批更新仍全部生效，两个版本都应显示未知的整批耗时 */
static void test_batch_clock_failure(void)
{
    for (int failure = 0; failure < 2; ++failure) {
        struct dppd_control_service service;
        struct dppd_control_batch_update_request requests[2] = {0};
        struct dppd_control_apply_result results[2];
        struct dppd_control_rule_status status;

        assert(dppd_control_init(&service, &topology, 2, &api) == 0);
        for (unsigned int i = 0; i < 2; ++i) {
            requests[i].rule = make_rule(250 + i, DPPD_FALLBACK_SOFTWARE_ONLY, false);
            requests[i].install_port_id = 5;
            assert(dppd_control_apply(&service, 5, &requests[i].rule, 0, &results[i]) == 0);
            requests[i].expected_generation = results[i].generation;
            requests[i].rule.priority = 20;
        }
        set_clock(failure);
        assert(dppd_control_update_batch(&service, requests, 2, results) == 0);
        for (unsigned int i = 0; i < 2; ++i) {
            assert(dppd_control_rule_status(&service, 250 + i, results[i].generation,
                                            &status) == 0);
            assert(!status.installation.timing_available);
            assert(status.installation.install_duration_ns == 0);
            assert(status.installation.commit_rule_count == 2);
        }
        assert(clock_index == clock_limit);
        assert(dppd_control_fini(&service) == 0);
    }
}

/** 准备占位、重复安装、后端缺失和错误端口分别构造，不能混成成功状态 */
static void test_inconsistency(void)
{
    struct dppd_control_service service;
    struct dppd_control_apply_result applied;
    struct dppd_control_rule_status status;
    struct dppd_rule_install_info info;
    struct dppd_flow_error error;
    struct dppd_transaction_item item = {0};
    struct dppd_transaction_backend hardware, software;
    struct dppd_flow_handle *handle;
    struct dppd_flow_handle saved;
    uintptr_t token;

    item.rule = make_rule(300, DPPD_FALLBACK_PREFER_HARDWARE, true);
    assert(dppd_control_init(&service, &topology, 4, &api) == 0);
    assert(dppd_control_apply(&service, 5, &item.rule, 0, &applied) == 0);
    assert(dppd_rule_repository_get(&service.rules, 300, &item.rule) == 0);
    hardware = dppd_rte_flow_transaction_backend(&service.rte_flow);
    software = dppd_software_transaction_backend(&service.software);
    item.plan.backend = DPPD_PLAN_BACKEND_SOFTWARE;
    item.plan.install_port_id = 5;
    assert(software.prepare(software.context, &item, &token) == 0);
    assert(dppd_software_backend_install_info(&service.software, 300, 1, &info) == -ENOENT);
    assert(dppd_control_rule_status(&service, 300, 1, &status) == 0);
    assert(software.commit(software.context, &item, token) == 0);
    software.finalize(software.context, &item, token);
    assert(dppd_control_rule_status(&service, 300, 1, &status) == -EUCLEAN);
    assert(dppd_software_backend_remove_version(&service.software, 300, 1) == 0);

    /** 逐一破坏并恢复 handle 字段，确认状态查询不会把错误身份当作有效安装 */
    handle = (struct dppd_flow_handle *)dppd_rte_flow_backend_find_version(
        &service.rte_flow, 300, 1);
    assert(handle != NULL);
    saved = *handle;
    handle->rule_id++;
    assert(dppd_control_rule_status(&service, 300, 1, &status) == -EUCLEAN);
    *handle = saved;
    handle->rule_generation++;
    assert(dppd_control_rule_status(&service, 300, 1, &status) == -EUCLEAN);
    *handle = saved;
    handle->port_id++;
    assert(dppd_control_rule_status(&service, 300, 1, &status) == -EUCLEAN);
    *handle = saved;
    handle->has_count = false;
    assert(dppd_control_rule_status(&service, 300, 1, &status) == -EUCLEAN);
    *handle = saved;
    handle->flow = NULL;
    assert(dppd_control_rule_status(&service, 300, 1, &status) == -EUCLEAN);
    *handle = saved;
    assert(dppd_rte_flow_backend_remove_version(&service.rte_flow, 300, 1, &error) == 0);
    assert(dppd_control_rule_status(&service, 300, 1, &status) == -EUCLEAN);

    /** 后端自身可以自洽，但安装端口与规则账本不同，控制层仍必须拒绝 */
    item.plan.backend = DPPD_PLAN_BACKEND_RTE_FLOW;
    item.plan.install_port_id = 6;
    assert(hardware.prepare(hardware.context, &item, &token) == 0);
    assert(dppd_rte_flow_backend_install_info(&service.rte_flow, 300, 1, &info) == -ENOENT);
    assert(hardware.commit(hardware.context, &item, token) == 0);
    assert(dppd_rte_flow_backend_install_info(&service.rte_flow, 300, 1, &info) == 0);
    assert(info.install_port_id == 6);
    assert(dppd_control_rule_status(&service, 300, 1, &status) == -EUCLEAN);
    assert(dppd_control_fini(&service) == 0);
}

/** 同秒倒退和巨大时间跨度都不能变成有效的无符号耗时 */
static void test_clock_edges(void)
{
    struct dppd_install_timer timer;
    uint64_t duration;

    set_clock(-1);
    clock_samples[1] = (struct timespec){.tv_sec = 2, .tv_nsec = 1};
    dppd_install_timer_start(&timer);
    assert(!dppd_install_timer_finish(&timer, &duration) && duration == 0);
    set_clock(-1);
    clock_samples[0] = (struct timespec){.tv_sec = 0, .tv_nsec = 0};
    clock_samples[1] = (struct timespec){.tv_sec = INT64_MAX, .tv_nsec = 0};
    dppd_install_timer_start(&timer);
    assert(!dppd_install_timer_finish(&timer, &duration) && duration == 0);
}

/** 所有场景都不需要初始化 EAL 或真实网卡，软件发布和管理 socket 则使用正式实现 */
int main(void)
{
    test_backends();
    test_clock_failures();
    test_batch_clock_failure();
    test_inconsistency();
    test_clock_edges();
    return 0;
}
