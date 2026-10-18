#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "dppd/admission.h"

static unsigned int fail_allocation;

void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size);

/** 只在指定一次分配处失败，检查创建失败不会覆盖输出或留下半个配额对象 */
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_allocation != 0 && --fail_allocation == 0)
        return NULL;
    return __real_calloc(count, size);
}

static struct dppd_admission_config config(uint32_t tickets, uint64_t limit)
{
    struct dppd_admission_config value = {.ticket_capacity = tickets, .port_count = 2};
    value.ports[0].port_id = 5;
    value.ports[1].port_id = 11;
    for (unsigned int kind = 0; kind < DPPD_RESOURCE_KIND_COUNT; ++kind) {
        value.limits[kind] = limit;
        value.ports[0].limits[kind] = limit;
        value.ports[1].limits[kind] = limit;
    }
    return value;
}

static struct dppd_resource_demand demand(uint16_t port, uint64_t amount)
{
    return (struct dppd_resource_demand){.kind = DPPD_RESOURCE_HARDWARE_FLOW, .port_id = port, .amount = amount};
}

static struct dppd_admission_snapshot snapshot(struct dppd_admission *admission)
{
    struct dppd_admission_snapshot value;
    assert(dppd_admission_snapshot(admission, &value) == 0);
    for (unsigned int kind = 0; kind < DPPD_RESOURCE_KIND_COUNT; ++kind) {
        assert(value.global.committed[kind] <= value.global.limits[kind]);
        assert(value.global.reserved[kind] <= value.global.limits[kind] - value.global.committed[kind]);
        for (uint32_t port = 0; port < value.port_count; ++port) {
            struct dppd_quota_usage *usage = &value.ports[port].usage;
            assert(usage->committed[kind] <= usage->limits[kind]);
            assert(usage->reserved[kind] <= usage->limits[kind] - usage->committed[kind]);
        }
    }
    return value;
}

static void no_usage(struct dppd_admission *admission)
{
    struct dppd_admission_snapshot value = snapshot(admission);
    assert(value.held_tickets == 0 && value.residual_tickets == 0);
    for (unsigned int kind = 0; kind < DPPD_RESOURCE_KIND_COUNT; ++kind) {
        assert(value.global.reserved[kind] == 0 && value.global.committed[kind] == 0);
        for (uint32_t port = 0; port < value.port_count; ++port)
            assert(value.ports[port].usage.reserved[kind] == 0 && value.ports[port].usage.committed[kind] == 0);
    }
}

/** 重复键、跨端口需求和多种资源必须作为一份预留整体接受，提交不释放额度 */
static void test_lifecycle(void)
{
    struct dppd_admission *admission = NULL;
    struct dppd_admission_config limits = config(8, 4);
    struct dppd_resource_demand requests[] = {demand(5, 1), demand(5, 2), demand(11, 1),
        {.kind = DPPD_RESOURCE_COUNTER, .port_id = DPPD_ADMISSION_GLOBAL_PORT, .amount = 2}};
    struct dppd_resource_ticket ticket, untouched = {.owner = 91, .serial = 92, .slot = 93};
    assert(dppd_admission_create(&limits, &admission) == 0);
    assert(dppd_admission_reserve(admission, requests, 4, &ticket) == 0);
    struct dppd_admission_snapshot value = snapshot(admission);
    assert(value.global.reserved[DPPD_RESOURCE_HARDWARE_FLOW] == 4);
    assert(value.ports[0].usage.reserved[DPPD_RESOURCE_HARDWARE_FLOW] == 3);
    assert(value.ports[1].usage.reserved[DPPD_RESOURCE_HARDWARE_FLOW] == 1);
    assert(value.global.reserved[DPPD_RESOURCE_COUNTER] == 2);
    assert(value.ports[0].usage.reserved[DPPD_RESOURCE_COUNTER] == 0);
    assert(dppd_admission_close(&admission) == -EBUSY && admission != NULL);
    assert(dppd_admission_reserve(admission, requests, 1, &untouched) == -ENOSPC);
    assert(untouched.owner == 91 && untouched.serial == 92 && untouched.slot == 93);
    assert(dppd_admission_commit(admission, &ticket) == 0);
    assert(dppd_admission_commit(admission, &ticket) == -EALREADY);
    value = snapshot(admission);
    assert(value.global.reserved[DPPD_RESOURCE_HARDWARE_FLOW] == 0);
    assert(value.global.committed[DPPD_RESOURCE_HARDWARE_FLOW] == 4 && value.held_tickets == 1);
    assert(dppd_admission_reserve(admission, requests, 1, &untouched) == -ENOSPC);
    assert(dppd_admission_release_recovered(admission, &ticket) == -EINVAL);
    assert(dppd_admission_release(admission, &ticket) == 0);
    assert(dppd_admission_release(admission, &ticket) == -ESTALE);
    no_usage(admission);
    assert(dppd_admission_close(&admission) == 0 && admission == NULL);
}

/** 后一项不合法或端口超限时，前一项也不能占额度，整数溢出更不能被当成小数值放行 */
static void test_rejections(void)
{
    struct dppd_admission *admission = NULL;
    struct dppd_admission_config limits = config(2, UINT64_MAX);
    limits.ports[0].limits[DPPD_RESOURCE_HARDWARE_FLOW] = 1;
    struct dppd_resource_ticket ticket;
    assert(dppd_admission_create(&limits, &admission) == 0);
    struct dppd_resource_demand requests[] = {demand(11, 1), demand(5, 2)};
    assert(dppd_admission_reserve(admission, requests, 2, &ticket) == -EDQUOT);
    no_usage(admission);
    requests[1] = demand(99, 1);
    assert(dppd_admission_reserve(admission, requests, 2, &ticket) == -ENOENT);
    requests[1] = demand(5, 0);
    assert(dppd_admission_reserve(admission, requests, 2, &ticket) == -EINVAL);
    requests[1] = demand(5, 1);
    requests[1].kind = DPPD_RESOURCE_KIND_COUNT;
    assert(dppd_admission_reserve(admission, requests, 2, &ticket) == -EINVAL);
    requests[0] = demand(DPPD_ADMISSION_GLOBAL_PORT, UINT64_MAX);
    requests[1] = demand(DPPD_ADMISSION_GLOBAL_PORT, 1);
    assert(dppd_admission_reserve(admission, requests, 2, &ticket) == -EOVERFLOW);
    no_usage(admission);
    assert(dppd_admission_reserve(admission, requests, 1, &ticket) == 0);
    assert(dppd_admission_commit(admission, &ticket) == 0);
    struct dppd_admission_snapshot value = snapshot(admission);
    assert(value.global.committed[DPPD_RESOURCE_HARDWARE_FLOW] == UINT64_MAX);
    struct dppd_resource_ticket other;
    assert(dppd_admission_reserve(admission, &requests[1], 1, &other) == -ENOSPC);
    assert(dppd_admission_release(admission, &ticket) == 0);
    no_usage(admission);
    assert(dppd_admission_close(&admission) == 0);
}

/** 残留凭据即使尚在预留阶段也继续计费，只有精确恢复入口才能在确认清理后释放 */
static void test_residuals_and_stale_tickets(void)
{
    struct dppd_admission *admission = NULL, *other = NULL;
    struct dppd_admission_config limits = config(1, 1);
    struct dppd_resource_demand request = demand(5, 1);
    struct dppd_resource_ticket ticket, next;
    assert(dppd_admission_create(&limits, &admission) == 0 && dppd_admission_create(&limits, &other) == 0);
    for (unsigned int committed = 0; committed < 2; ++committed) {
        assert(dppd_admission_reserve(admission, &request, 1, &ticket) == 0);
        assert(dppd_admission_release(other, &ticket) == -ESTALE);
        if (committed)
            assert(dppd_admission_commit(admission, &ticket) == 0);
        assert(dppd_admission_mark_residual(admission, &ticket) == 0);
        assert(dppd_admission_mark_residual(admission, &ticket) == 0);
        struct dppd_admission_snapshot value = snapshot(admission);
        assert(value.held_tickets == 1 && value.residual_tickets == 1);
        assert(value.global.reserved[DPPD_RESOURCE_HARDWARE_FLOW] == (committed ? 0 : 1));
        assert(value.global.committed[DPPD_RESOURCE_HARDWARE_FLOW] == committed);
        assert(dppd_admission_commit(admission, &ticket) == -EUCLEAN);
        assert(dppd_admission_release(admission, &ticket) == -EUCLEAN);
        assert(dppd_admission_reserve(admission, &request, 1, &next) == -ENOSPC);
        assert(dppd_admission_release_recovered(admission, &ticket) == 0);
        assert(dppd_admission_reserve(admission, &request, 1, &next) == 0);
        assert(next.slot == ticket.slot && next.serial != ticket.serial);
        assert(dppd_admission_commit(admission, &ticket) == -ESTALE);
        assert(dppd_admission_mark_residual(admission, &ticket) == -ESTALE);
        assert(dppd_admission_release(admission, &next) == 0);
    }
    no_usage(admission);
    assert(dppd_admission_close(&admission) == 0 && dppd_admission_close(&other) == 0);
}

static void test_limit_changes(void)
{
    struct dppd_admission *admission = NULL;
    struct dppd_admission_config limits = config(4, 4), changed;
    struct dppd_resource_demand request = demand(5, 2);
    struct dppd_resource_ticket active, prepared;
    assert(dppd_admission_create(&limits, &admission) == 0);
    assert(dppd_admission_reserve(admission, &request, 1, &active) == 0);
    assert(dppd_admission_commit(admission, &active) == 0);
    assert(dppd_admission_reserve(admission, &request, 1, &prepared) == 0);
    changed = limits;
    changed.limits[DPPD_RESOURCE_HARDWARE_FLOW] = 3;
    assert(dppd_admission_set_limits(admission, &changed, 1) == -EBUSY);
    assert(snapshot(admission).revision == 1);
    changed = limits;
    changed.ports[0].limits[DPPD_RESOURCE_HARDWARE_FLOW] = 3;
    assert(dppd_admission_set_limits(admission, &changed, 1) == -EBUSY);
    assert(dppd_admission_set_limits(admission, &limits, 0) == -ESTALE);
    changed = limits;
    changed.ports[0].port_id = 7;
    assert(dppd_admission_set_limits(admission, &changed, 1) == -EINVAL);
    changed = limits;
    changed.limits[DPPD_RESOURCE_HARDWARE_FLOW] = 8;
    assert(dppd_admission_set_limits(admission, &changed, 1) == 0);
    assert(snapshot(admission).revision == 2);
    assert(dppd_admission_set_limits(admission, &limits, 1) == -ESTALE);
    assert(dppd_admission_release(admission, &prepared) == 0);
    assert(dppd_admission_release(admission, &active) == 0);
    assert(dppd_admission_set_limits(admission, &limits, 2) == 0);
    no_usage(admission);
    assert(dppd_admission_close(&admission) == 0);
}

struct concurrency {
    struct dppd_admission *admission;
    pthread_barrier_t start, held, release;
    atomic_uint successes;
};

/** 所有线程先同时争抢四份额度，再停在屏障处核对总占用，不能只检查最后是否归零 */
static void *producer(void *context)
{
    struct concurrency *fixture = context;
    struct dppd_resource_demand request = demand(5, 1);
    struct dppd_resource_ticket ticket;
    pthread_barrier_wait(&fixture->start);
    int rc = dppd_admission_reserve(fixture->admission, &request, 1, &ticket);
    assert(rc == 0 || rc == -ENOSPC);
    pthread_barrier_wait(&fixture->held);
    pthread_barrier_wait(&fixture->release);
    if (rc == 0) {
        assert(dppd_admission_commit(fixture->admission, &ticket) == 0);
        assert(dppd_admission_release(fixture->admission, &ticket) == 0);
        atomic_fetch_add_explicit(&fixture->successes, 1, memory_order_relaxed);
    }
    for (unsigned int i = 0; i < 1000; ++i) {
        rc = dppd_admission_reserve(fixture->admission, &request, 1, &ticket);
        if (rc == -ENOSPC)
            continue;
        assert(rc == 0 && dppd_admission_commit(fixture->admission, &ticket) == 0);
        snapshot(fixture->admission);
        assert(dppd_admission_release(fixture->admission, &ticket) == 0);
        atomic_fetch_add_explicit(&fixture->successes, 1, memory_order_relaxed);
    }
    return NULL;
}

static void test_concurrency(void)
{
    struct concurrency fixture = {0};
    struct dppd_admission_config limits = config(8, 4);
    pthread_t threads[8];
    assert(dppd_admission_create(&limits, &fixture.admission) == 0);
    assert(pthread_barrier_init(&fixture.start, NULL, 9) == 0);
    assert(pthread_barrier_init(&fixture.held, NULL, 9) == 0);
    assert(pthread_barrier_init(&fixture.release, NULL, 9) == 0);
    for (unsigned int i = 0; i < 8; ++i)
        assert(pthread_create(&threads[i], NULL, producer, &fixture) == 0);
    pthread_barrier_wait(&fixture.start);
    pthread_barrier_wait(&fixture.held);
    struct dppd_admission_snapshot value = snapshot(fixture.admission);
    assert(value.held_tickets == 4 && value.global.reserved[DPPD_RESOURCE_HARDWARE_FLOW] == 4);
    assert(value.rejections == 4 && value.global.committed[DPPD_RESOURCE_HARDWARE_FLOW] == 0);
    pthread_barrier_wait(&fixture.release);
    for (unsigned int i = 0; i < 8; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    assert(atomic_load_explicit(&fixture.successes, memory_order_relaxed) >= 4);
    no_usage(fixture.admission);
    assert(dppd_admission_close(&fixture.admission) == 0);
    assert(pthread_barrier_destroy(&fixture.start) == 0 && pthread_barrier_destroy(&fixture.held) == 0);
    assert(pthread_barrier_destroy(&fixture.release) == 0);
}

static void test_invalid_inputs_and_allocation(void)
{
    struct dppd_admission *admission = NULL;
    struct dppd_admission_config limits = config(4, 4), invalid = limits;
    assert(dppd_admission_create(NULL, &admission) == -EINVAL);
    invalid.ticket_capacity = 0;
    assert(dppd_admission_create(&invalid, &admission) == -EINVAL && admission == NULL);
    invalid = limits;
    invalid.ports[1].port_id = invalid.ports[0].port_id;
    assert(dppd_admission_create(&invalid, &admission) == -EINVAL);
    for (unsigned int i = 1; i <= 2; ++i) {
        fail_allocation = i;
        assert(dppd_admission_create(&limits, &admission) == -ENOMEM && admission == NULL);
    }
    assert(dppd_admission_create(&limits, &admission) == 0);
    assert(dppd_admission_create(&limits, &admission) == -EALREADY);
    struct dppd_resource_demand request = demand(5, 1);
    struct dppd_resource_ticket ticket;
    assert(dppd_admission_reserve(admission, &request, 0, &ticket) == -EINVAL);
    assert(dppd_admission_reserve(admission, &request, DPPD_ADMISSION_MAX_DEMANDS + 1U, &ticket) == -EINVAL);
    no_usage(admission);
    assert(dppd_admission_close(&admission) == 0);
}

/** 凭据池先满时也要拒绝，最大需求数组则逐一核对八个端口的全部资源 */
static void test_ticket_capacity_and_maximum_vector(void)
{
    struct dppd_admission *admission = NULL;
    struct dppd_admission_config limits = config(1, 16);
    struct dppd_resource_demand request = demand(5, 1);
    struct dppd_resource_ticket ticket, other;
    assert(dppd_admission_create(&limits, &admission) == 0);
    assert(dppd_admission_reserve(admission, &request, 1, &ticket) == 0);
    assert(dppd_admission_reserve(admission, &request, 1, &other) == -ENOSPC);
    assert(snapshot(admission).global.reserved[DPPD_RESOURCE_HARDWARE_FLOW] == 1);
    assert(dppd_admission_release(admission, &ticket) == 0 && dppd_admission_close(&admission) == 0);

    limits.port_count = 8;
    struct dppd_resource_demand requests[DPPD_ADMISSION_MAX_DEMANDS];
    uint32_t count = 0;
    for (uint32_t port = 0; port < limits.port_count; ++port) {
        limits.ports[port].port_id = (uint16_t)(port + 1U);
        for (unsigned int kind = 0; kind < DPPD_RESOURCE_KIND_COUNT; ++kind) {
            limits.ports[port].limits[kind] = 2;
            requests[count++] = (struct dppd_resource_demand){.kind = (enum dppd_resource_kind)kind,
                .port_id = limits.ports[port].port_id,
                .amount = 1};
        }
    }
    assert(count == DPPD_ADMISSION_MAX_DEMANDS && dppd_admission_create(&limits, &admission) == 0);
    assert(dppd_admission_reserve(admission, requests, count, &ticket) == 0);
    assert(dppd_admission_commit(admission, &ticket) == 0);
    struct dppd_admission_snapshot value = snapshot(admission);
    for (unsigned int kind = 0; kind < DPPD_RESOURCE_KIND_COUNT; ++kind) {
        assert(value.global.committed[kind] == 8);
        for (uint32_t port = 0; port < value.port_count; ++port)
            assert(value.ports[port].usage.committed[kind] == 1);
    }
    assert(dppd_admission_release(admission, &ticket) == 0);
    no_usage(admission);
    assert(dppd_admission_close(&admission) == 0);
}

int main(void)
{
    test_invalid_inputs_and_allocation();
    test_lifecycle();
    test_rejections();
    test_residuals_and_stale_tickets();
    test_limit_changes();
    test_ticket_capacity_and_maximum_vector();
    test_concurrency();
    return 0;
}
