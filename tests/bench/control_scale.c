#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "dppd/control.h"

/** 基准依赖断言核对实际安装，关闭断言会把没有执行操作的空循环误当成性能提升 */
#ifdef NDEBUG
#error "benchmark requires active assertions"
#endif

#define BATCH_SIZE 64U

static uint32_t create_calls, fail_create_at, fail_removes;

/** 内存驱动不访问网卡，仅为正式控制面的规划、事务、账本和补偿提供确定结果 */
static int memory_validate(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_error *error)
{
    (void)port;
    (void)rule;
    (void)error;
    return 0;
}

static int memory_create(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_handle *handle,
    struct dppd_flow_error *error)
{
    (void)error;
    if (++create_calls == fail_create_at)
        return -EIO;
    *handle = (struct dppd_flow_handle){.rule_id = rule->id,
        .rule_generation = rule->generation,
        .port_id = port,
        .flow = (struct rte_flow *)(uintptr_t)(rule->id + 1U)};
    return 0;
}

static int memory_remove(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    if (fail_removes != 0) {
        --fail_removes;
        return -EIO;
    }
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static const struct dppd_flow_api api = {.validate = memory_validate, .create = memory_create, .remove = memory_remove};
static const struct dppd_topology topology = {.nb_endpoints = 1, .endpoints = {{.ethdev_port_id = 5}}};

/** 所有样本使用同一单调时钟，采集失败必须终止，不能把零耗时写成通过结果 */
static uint64_t now_ns(void)
{
    struct timespec time;
    assert(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
}

static int compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static struct dppd_rule make_rule(uint64_t id, bool software)
{
    return (struct dppd_rule){.id = id,
        .install_port_id = 5,
        .fallback = software ? DPPD_FALLBACK_SOFTWARE_ONLY : DPPD_FALLBACK_REQUIRE_HARDWARE,
        .priority = (uint32_t)id,
        .nb_matches = 1,
        .matches = {{.type = DPPD_MATCH_ETH}},
        .nb_actions = 1,
        .actions = {{.type = DPPD_ACTION_DROP}}};
}

/** 端到端样本包含正式 API 的规划、事务与发布，不能用纯循环次数替代安装结果 */
static void measure(uint32_t count, uint32_t batch, bool software)
{
    struct dppd_control_service service;
    struct dppd_control_batch_create_request requests[BATCH_SIZE];
    struct dppd_control_apply_result results[BATCH_SIZE];
    uint64_t *samples = calloc(count / batch, sizeof(*samples));
    assert(samples != NULL && dppd_control_init(&service, &topology, count, &api) == 0);
    uint64_t start = now_ns();
    for (uint32_t offset = 0; offset < count; offset += batch) {
        for (uint32_t i = 0; i < batch; ++i)
            requests[i] = (struct dppd_control_batch_create_request){.install_port_id = 5,
                .rule = make_rule(offset + i + 1U, software)};
        uint64_t before = now_ns();
        if (batch == 1)
            assert(dppd_control_apply(&service, 5, &requests[0].rule, 0, &results[0]) == 0);
        else
            assert(dppd_control_create_batch(&service, requests, batch, results) == 0);
        samples[offset / batch] = now_ns() - before;
        for (uint32_t i = 0; i < batch; ++i)
            assert(results[i].status == DPPD_RULE_CREATED && results[i].generation == offset + i + 1U);
    }
    uint64_t elapsed = now_ns() - start;
    assert(elapsed != 0 && service.rules.count == count && service.rules.generation == count);
    for (uint32_t i = 0; i < count; ++i) {
        struct dppd_rule rule;
        assert(dppd_rule_repository_get(&service.rules, i + 1U, &rule) == 0 && rule.generation == i + 1U);
    }
    qsort(samples, count / batch, sizeof(*samples), compare_u64);
    uint64_t p99 = samples[((uint64_t)(count / batch) * 99U + 99U) / 100U - 1U];
    start = now_ns();
    for (uint32_t i = 0; i < count; ++i) {
        bool removed;
        uint64_t generation;
        assert(dppd_control_remove(&service, i + 1U, i + 1U, &removed, &generation) == 0 && removed);
    }
    uint64_t remove_ns = now_ns() - start;
    assert(service.rules.count == 0 && dppd_control_fini(&service) == 0);
    printf("{\"path\":\"%s\",\"rules\":%u,\"batch\":%u,\"install_ns\":%" PRIu64
           ",\"rules_per_second\":%.3f,\"request_p99_ns\":%" PRIu64 ",\"remove_ns\":%" PRIu64 "}",
        software ? "software" : "memory_driver", count, batch, elapsed, (double)count * 1e9 / (double)elapsed, p99,
        remove_ns);
    free(samples);
}

/** 注入创建及补偿失败，核对隔离到精确残留清理的时间，不冒充设备重启恢复时间 */
static uint64_t measure_recovery(void)
{
    struct dppd_control_service service;
    struct dppd_control_batch_create_request requests[BATCH_SIZE];
    struct dppd_control_apply_result results[BATCH_SIZE];
    assert(dppd_control_init(&service, &topology, BATCH_SIZE, &api) == 0);
    for (uint32_t i = 0; i < BATCH_SIZE; ++i)
        requests[i] =
            (struct dppd_control_batch_create_request){.install_port_id = 5, .rule = make_rule(i + 1U, false)};
    create_calls = 0;
    fail_create_at = 32;
    fail_removes = 1;
    assert(dppd_control_create_batch(&service, requests, BATCH_SIZE, results) == -EUCLEAN);
    assert(service.rules.count == 0 && service.recovery_state == DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED);
    assert(dppd_rte_flow_backend_count(&service.rte_flow) == 1);
    uint64_t start = now_ns();
    assert(dppd_control_reconciliation_retry(&service) == 0);
    uint64_t elapsed = now_ns() - start;
    assert(dppd_rte_flow_backend_count(&service.rte_flow) == 0);
    assert(service.recovery_state == DPPD_CONTROL_RECOVERY_RESTART_REQUIRED && dppd_control_fini(&service) == 0);
    return elapsed;
}

/** 使用真实快照保存与重放，计时包含新服务初始化、文件读取和内存驱动安装 */
static uint64_t measure_replay(uint32_t count)
{
    struct dppd_control_service service;
    struct dppd_control_apply_result result;
    char directory[] = "/tmp/dppd-m4-benchmark-XXXXXX", path[128];
    assert(mkdtemp(directory) != NULL);
    assert(snprintf(path, sizeof(path), "%s/state.bin", directory) > 0);
    create_calls = fail_create_at = fail_removes = 0;
    assert(dppd_control_init(&service, &topology, count, &api) == 0);
    for (uint32_t i = 0; i < count; ++i) {
        struct dppd_rule rule = make_rule(i + 1U, false);
        assert(dppd_control_apply(&service, 5, &rule, 0, &result) == 0);
    }
    assert(dppd_control_persistence_attach(&service, path) == 0);
    assert(dppd_control_persistence_flush(&service) == 0 && dppd_control_fini(&service) == 0);
    create_calls = 0;
    uint64_t start = now_ns();
    assert(dppd_control_init(&service, &topology, count, &api) == 0);
    assert(dppd_control_persistence_restore(&service, path) == 0);
    uint64_t elapsed = now_ns() - start;
    assert(service.rules.count == count && service.rules.generation == count && create_calls == count);
    for (uint32_t i = 0; i < count; ++i) {
        struct dppd_rule stored;
        assert(dppd_rule_repository_get(&service.rules, i + 1U, &stored) == 0);
        assert(stored.id == i + 1U && stored.generation == i + 1U && stored.install_port_id == 5);
    }
    assert(dppd_control_fini(&service) == 0 && unlink(path) == 0 && rmdir(directory) == 0);
    return elapsed;
}

int main(int argc, char **argv)
{
    char *end;
    errno = 0;
    if (argc != 2)
        return 2;
    unsigned long parsed = strtoul(argv[1], &end, 10);
    if (errno != 0 || *argv[1] == '\0' || *end != '\0' || parsed < BATCH_SIZE || parsed > 65536 ||
        parsed % BATCH_SIZE != 0)
        return 2;
    uint32_t count = (uint32_t)parsed;
    printf("{\"schema_version\":1,\"hardware_acceptance\":\"not_run\",\"cases\":[");
    measure(count, 1, false);
    printf(",");
    measure(count, BATCH_SIZE, false);
    printf(",");
    measure(count > 1024 ? 1024 : count, 1, true);
    uint64_t replay_ns = measure_replay(count);
    printf("],\"inprocess_residual_cleanup_ns\":%" PRIu64
           ",\"snapshot_replay_rules\":%u,\"snapshot_replay_ns\":%" PRIu64 "}\n",
        measure_recovery(), count, replay_ns);
    return 0;
}
