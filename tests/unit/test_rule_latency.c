#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "dppd/device.h"
#include "dppd/management.h"

static struct timespec clock_samples[2];
static unsigned int clock_index, clock_limit, clock_calls, driver_calls, fail_allocation;
static int clock_failure = -1;
static bool create_failure;

int __real_clock_gettime(clockid_t clock_id, struct timespec *value);
int __wrap_clock_gettime(clockid_t clock_id, struct timespec *value);
void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size);

/** 只替换本测试的时钟引用，精确检查安装复用原有两次采样，查询不能新增采样 */
int __wrap_clock_gettime(clockid_t clock_id, struct timespec *value)
{
    clock_calls++;
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

/** 指定一次分配失败，历史记录本身和历史查询都不应消耗这次注入 */
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_allocation != 0 && --fail_allocation == 0)
        return NULL;
    return __real_calloc(count, size);
}

/** 预设一个有效耗时或开始/结束采样错误，开始失败时结束采样不会发生 */
static void arm_clock(uint64_t duration, int failure)
{
    clock_samples[0] = (struct timespec){.tv_sec = 10};
    clock_samples[1] = (struct timespec){.tv_sec = 10 + (time_t)(duration / 1000000000U),
        .tv_nsec = (long)(duration % 1000000000U)};
    clock_index = 0;
    clock_limit = failure == 0 ? 1U : 2U;
    clock_failure = failure;
}

/** 允许硬件提交，记录驱动访问数以证明历史查询不重新校验 */
static int validate(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_error *error)
{
    (void)port; (void)rule; (void)error;
    driver_calls++;
    return 0;
}

/** 创建错误不构成成功提交样本，成功时填写正式后端需要的对象身份 */
static int create(uint16_t port, const struct dppd_rule *rule,
    struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    driver_calls++;
    memset(handle, 0, sizeof(*handle));
    if (create_failure)
        return -EIO;
    *handle = (struct dppd_flow_handle){.rule_id = rule->id, .rule_generation = rule->generation,
        .port_id = port, .flow = (struct rte_flow *)(uintptr_t)(rule->id + 1U)};
    return 0;
}

/** 清理对象只影响实际规则数量，不能从安装历史中扣除样本 */
static int remove_flow(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    driver_calls++;
    memset(handle, 0, sizeof(*handle));
    return 0;
}

/** 最小 ETH/DROP 规则用于软件与硬件，策略是唯一的后端选择条件 */
static struct dppd_rule rule(uint64_t id, enum dppd_fallback_policy fallback)
{
    struct dppd_rule value = {.id = id, .fallback = fallback, .nb_matches = 1, .nb_actions = 1};

    value.matches[0].type = DPPD_MATCH_ETH;
    value.actions[0].type = DPPD_ACTION_DROP;
    return value;
}

/** 每个边界正好命中当前区间，超过一纳秒进入下一区间，零耗时仍是有效测量 */
static void boundaries_and_percentiles(void)
{
    struct dppd_rule_latency_histogram histogram = {0};
    struct dppd_rule_latency_summary summary;

    dppd_rule_latency_summarize(&histogram, &summary);
    assert(!summary.mean_available && !summary.quantiles_available);
    dppd_rule_latency_record(&histogram, true, 0, 1);
    dppd_rule_latency_summarize(&histogram, &summary);
    assert(summary.mean_available && summary.quantiles_available && summary.mean_ns == 0);
    assert(histogram.samples == 1 && histogram.buckets[0] == 1 && histogram.min_ns == 0);
    for (unsigned int bucket = 0; bucket + 1 < DPPD_RULE_LATENCY_BUCKETS; ++bucket) {
        uint64_t upper = dppd_rule_latency_bucket_upper(bucket);
        struct dppd_rule_latency_histogram exact = {0};

        dppd_rule_latency_record(&exact, true, upper, 1);
        assert(exact.buckets[bucket] == 1);
        dppd_rule_latency_record(&exact, true, upper + 1U, 4);
        assert(exact.buckets[bucket + 1U] == 1 && exact.samples == 2 && exact.rules == 5);
    }
    memset(&histogram, 0, sizeof(histogram));
    for (unsigned int sample = 1; sample <= 100; ++sample)
        dppd_rule_latency_record(&histogram, true, sample * 1000U, 1);
    dppd_rule_latency_record(&histogram, false, UINT64_MAX, 4);
    dppd_rule_latency_summarize(&histogram, &summary);
    assert(histogram.samples == 100 && histogram.unavailable == 1 && histogram.rules == 104);
    assert(histogram.min_ns == 1000 && histogram.max_ns == 100000 && summary.mean_ns == 50500);
    assert(summary.p50_upper_ns == 50000 && summary.p95_upper_ns == 100000 && summary.p99_upper_ns == 100000);
    dppd_rule_latency_summarize(NULL, &summary);
    assert(summary.histogram.samples == 0 && !summary.quantiles_available);
    dppd_rule_latency_summarize(&histogram, NULL);
}

/** 总耗时溢出不损坏区间计数，计数空间用尽则冻结，分位数排名自身也不能乘法溢出 */
static void saturation(void)
{
    struct dppd_rule_latency_histogram histogram = {0}, frozen;
    struct dppd_rule_latency_summary summary;

    dppd_rule_latency_record(&histogram, true, UINT64_MAX, 1);
    dppd_rule_latency_record(&histogram, true, 1, 1);
    dppd_rule_latency_summarize(&histogram, &summary);
    assert(histogram.total_ns == UINT64_MAX && histogram.total_saturated && histogram.samples == 2);
    assert(!summary.mean_available && summary.quantiles_available && summary.p99_upper_ns == UINT64_MAX);
    memset(&histogram, 0, sizeof(histogram));
    histogram.samples = histogram.rules = histogram.buckets[0] = UINT64_MAX;
    histogram.min_ns = histogram.max_ns = 1;
    dppd_rule_latency_summarize(&histogram, &summary);
    assert(summary.quantiles_available && summary.p50_upper_ns == 1 && summary.p99_upper_ns == 1);
    dppd_rule_latency_record(&histogram, true, 10, 1);
    assert(histogram.counters_saturated && histogram.samples == UINT64_MAX && histogram.buckets[0] == UINT64_MAX);
    frozen = histogram;
    dppd_rule_latency_record(&histogram, false, 0, 1);
    assert(memcmp(&histogram, &frozen, sizeof(histogram)) == 0);
    dppd_rule_latency_summarize(&histogram, &summary);
    assert(!summary.mean_available && !summary.quantiles_available);
    memset(&histogram, 0, sizeof(histogram));
    histogram.rules = UINT64_MAX - 1U;
    dppd_rule_latency_record(&histogram, true, 10, 2);
    assert(histogram.counters_saturated && histogram.samples == 0 && histogram.rules == UINT64_MAX - 1U);
    memset(&histogram, 0, sizeof(histogram));
    histogram.unavailable = UINT64_MAX;
    dppd_rule_latency_record(&histogram, false, 0, 1);
    assert(histogram.counters_saturated && histogram.unavailable == UINT64_MAX);
}

/** 真实本机 socket 验证 v13 被拒绝、v14 可读、隔离期间可读且不增加请求指标 */
static void socket_query(struct dppd_control_service *control, const struct dppd_rule_latency_report *expected)
{
    struct dppd_management_server server;
    struct dppd_device_set devices = {0};
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    struct dppd_management_response response;
    uint64_t operations = control->observation.metrics.operations;
    char path[108];

    snprintf(path, sizeof(path), "/tmp/dppd-latency-socket-%ld", (long)getpid());
    memcpy(address.sun_path, path, strlen(path) + 1U);
    assert(dppd_management_start(&server, control, &devices, NULL, path) == 0);
    for (unsigned int attempt = 0; attempt < 3; ++attempt) {
        struct dppd_management_request request = {.version = DPPD_MANAGEMENT_VERSION,
            .size = sizeof(request), .operation = DPPD_MANAGEMENT_RULE_LATENCY, .request_id = 99};
        int client = socket(AF_UNIX, SOCK_SEQPACKET, 0);

        if (attempt == 0)
            request.version--;
        if (attempt == 2)
            control->recovery_state = DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED;
        assert(client >= 0 && connect(client, (const struct sockaddr *)&address, sizeof(address)) == 0);
        assert(send(client, &request, sizeof(request), 0) == (ssize_t)sizeof(request));
        assert(dppd_management_poll(&server) == 0);
        assert(recv(client, &response, sizeof(response), 0) == (ssize_t)sizeof(response));
        assert(response.status == (attempt == 0 ? -EPROTO : 0));
        if (attempt != 0)
            assert(memcmp(&response.payload.rule_latency, expected, sizeof(*expected)) == 0);
        assert(close(client) == 0 && control->observation.metrics.operations == operations);
    }
    control->recovery_state = DPPD_CONTROL_RECOVERY_READY;
    dppd_management_stop(&server);
    assert(access(path, F_OK) != 0);
}

/** 真实控制路径覆盖单条、整批、删除保留、安装失败、时钟失败和无副作用查询 */
static void backend_history(void)
{
    const struct dppd_topology topology = {.nb_endpoints = 1, .endpoints = {{.ethdev_port_id = 5}}};
    const struct dppd_flow_api api = {.validate = validate, .create = create, .remove = remove_flow};
    struct dppd_control_service control;
    struct dppd_control_apply_result applied, results[2];
    struct dppd_control_batch_update_request requests[2] = {0};
    struct dppd_rule value = rule(1, DPPD_FALLBACK_REQUIRE_HARDWARE);
    struct dppd_rule_latency_report report, copied;
    bool removed;
    uint64_t generation;

    assert(dppd_control_init(&control, &topology, 4, &api) == 0);
    arm_clock(1234, -1);
    assert(dppd_control_apply(&control, 5, &value, 0, &applied) == 0 && clock_index == 2);
    assert(dppd_control_apply(&control, 5, &value, 0, &applied) == 0);
    value = rule(2, DPPD_FALLBACK_SOFTWARE_ONLY);
    arm_clock(0, -1);
    assert(dppd_control_apply(&control, 5, &value, 0, &applied) == 0 && clock_index == 2);
    for (unsigned int index = 0; index < 2; ++index) {
        requests[index].rule = rule(3 + index, DPPD_FALLBACK_SOFTWARE_ONLY);
        requests[index].install_port_id = 5;
        arm_clock(100, -1);
        assert(dppd_control_apply(&control, 5, &requests[index].rule, 0, &applied) == 0 && clock_index == 2);
        requests[index].expected_generation = applied.generation;
        requests[index].rule.priority = 9;
    }
    arm_clock(3500, -1);
    assert(dppd_control_update_batch(&control, requests, 2, results) == 0 && clock_index == 2);
    for (unsigned int index = 0; index < 2; ++index)
        requests[index].expected_generation = results[index].generation;
    dppd_control_rule_latency(&control, &report);
    assert(report.scopes[DPPD_RULE_LATENCY_RTE_FLOW].histogram.samples == 1);
    assert(report.scopes[DPPD_RULE_LATENCY_RTE_FLOW].mean_ns == 1234);
    assert(report.scopes[DPPD_RULE_LATENCY_SOFTWARE].histogram.samples == 3);
    assert(report.scopes[DPPD_RULE_LATENCY_SOFTWARE].histogram.total_ns == 200);
    assert(report.scopes[DPPD_RULE_LATENCY_SOFTWARE_BATCH].histogram.samples == 1);
    assert(report.scopes[DPPD_RULE_LATENCY_SOFTWARE_BATCH].histogram.rules == 2);
    assert(report.scopes[DPPD_RULE_LATENCY_SOFTWARE_BATCH].mean_ns == 3500);
    assert(dppd_control_remove(&control, 1, 1, &removed, &generation) == 0 && removed);
    assert(dppd_control_remove(&control, 2, 2, &removed, &generation) == 0 && removed);
    dppd_control_rule_latency(&control, &copied);
    assert(memcmp(&report, &copied, sizeof(report)) == 0);
    create_failure = true;
    value = rule(10, DPPD_FALLBACK_REQUIRE_HARDWARE);
    arm_clock(10, -1);
    assert(dppd_control_apply(&control, 5, &value, 0, &applied) == -EIO && clock_index == 1);
    clock_limit = 0;
    create_failure = false;
    dppd_control_rule_latency(&control, &copied);
    assert(memcmp(&report, &copied, sizeof(report)) == 0);
    fail_allocation = 1;
    assert(dppd_control_update_batch(&control, requests, 2, results) == -ENOMEM && fail_allocation == 0);
    dppd_control_rule_latency(&control, &copied);
    assert(memcmp(&report, &copied, sizeof(report)) == 0);
    for (int failure = 0; failure < 2; ++failure) {
        arm_clock(10, failure);
        assert(dppd_control_update_batch(&control, requests, 2, results) == 0 && clock_index == clock_limit);
        for (unsigned int index = 0; index < 2; ++index)
            requests[index].expected_generation = results[index].generation;
    }
    value = rule(20, DPPD_FALLBACK_SOFTWARE_ONLY);
    arm_clock(10, 0);
    assert(dppd_control_apply(&control, 5, &value, 0, &applied) == 0 && clock_index == 1);
    dppd_control_rule_latency(&control, &report);
    assert(report.scopes[DPPD_RULE_LATENCY_SOFTWARE_BATCH].histogram.unavailable == 2);
    assert(report.scopes[DPPD_RULE_LATENCY_SOFTWARE_BATCH].histogram.samples == 1);
    assert(report.scopes[DPPD_RULE_LATENCY_SOFTWARE_BATCH].histogram.rules == 6);
    assert(report.scopes[DPPD_RULE_LATENCY_SOFTWARE].histogram.unavailable == 1);
    unsigned int clocks = clock_calls, drivers = driver_calls;
    fail_allocation = 1;
    for (unsigned int query = 0; query < 10; ++query) {
        dppd_control_rule_latency(&control, &copied);
        assert(memcmp(&report, &copied, sizeof(report)) == 0);
    }
    assert(fail_allocation == 1 && clock_calls == clocks && driver_calls == drivers);
    fail_allocation = 0;
    socket_query(&control, &report);
    assert(clock_calls == clocks && driver_calls == drivers);
    assert(dppd_control_fini(&control) == 0);
}

/** 直方图数学边界与真实控制路径分开验证，避免只检查实现自己写入的字段 */
int main(void)
{
    boundaries_and_percentiles();
    saturation();
    backend_history();
    return 0;
}
