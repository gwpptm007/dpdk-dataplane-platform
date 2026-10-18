#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dppd/flow_service.h"

#ifdef NDEBUG
#error "benchmark requires active assertions"
#endif

struct memory_queue {
    struct dppd_async_driver_result results[256];
    uint32_t count, ready;
};

struct memory_driver {
    struct memory_queue queues[8];
    uint32_t objects;
    int pattern, actions, table;
};

/** 仅替换驱动，不替换正式队列、配额、模板、批次协调或期望账本，不作为 ASIC 吞吐 */
static int create(void *context, uint16_t port, uint32_t queue, void *table, const struct dppd_rule *rule,
    uintptr_t tag, void **output)
{
    struct memory_driver *driver = context;
    assert(port == 5 && queue < 8 && table == &driver->table && rule->generation != 0);
    struct memory_queue *pending = &driver->queues[queue];
    assert(pending->count < 256);
    uint64_t *object = malloc(sizeof(*object));
    if (object == NULL)
        return -ENOMEM;
    *object = rule->id;
    *output = object;
    ++driver->objects;
    pending->results[pending->count++] = (struct dppd_async_driver_result){tag, 0};
    return 0;
}

static int destroy(void *context, uint16_t port, uint32_t queue, void *object, uintptr_t tag)
{
    struct memory_driver *driver = context;
    struct memory_queue *pending = &driver->queues[queue];
    assert(port == 5 && queue < 8 && object != NULL && pending->count < 256);
    pending->results[pending->count++] = (struct dppd_async_driver_result){tag, 0};
    return 0;
}

static int push(void *context, uint16_t port, uint32_t queue)
{
    struct memory_driver *driver = context;
    assert(port == 5 && queue < 8);
    driver->queues[queue].ready = driver->queues[queue].count;
    return 0;
}

static int pull(void *context, uint16_t port, uint32_t queue, struct dppd_async_driver_result *results, uint16_t count)
{
    struct memory_driver *driver = context;
    struct memory_queue *pending = &driver->queues[queue];
    unsigned int copied = 0;
    assert(port == 5 && queue < 8);
    while (pending->ready != 0 && copied < count) {
        results[copied++] = pending->results[--pending->count];
        --pending->ready;
    }
    return (int)copied;
}

static void release(void *context, void *object)
{
    struct memory_driver *driver = context;
    assert(driver->objects != 0);
    --driver->objects;
    free(object);
}

static int template_create(void *context, const struct dppd_flow_template_spec *spec,
    struct dppd_flow_template_parts *parts)
{
    struct memory_driver *driver = context;
    assert(spec->example.install_port_id == 5);
    *parts = (struct dppd_flow_template_parts){&driver->pattern, &driver->actions, &driver->table};
    return 0;
}

static int template_destroy(void *context, uint16_t port, struct dppd_flow_template_parts *parts)
{
    (void)context;
    assert(port == 5);
    *parts = (struct dppd_flow_template_parts){0};
    return 0;
}

static uint64_t now_ns(void)
{
    struct timespec value;
    assert(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static int compare(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

int main(int argc, char **argv)
{
    uint32_t count = argc == 2 ? (uint32_t)strtoul(argv[1], NULL, 10) : 4096;
    assert(count >= 2048 && count <= 16384 && count % 2048 == 0);
    struct memory_driver memory = {0};
    struct dppd_admission_config quota = {.ticket_capacity = count * 2U + 2048U + 9U, .port_count = 1};
    struct dppd_admission *admission = NULL;
    struct dppd_flow_templates *templates = NULL;
    struct dppd_flow_template_id table;
    struct dppd_flow_template_driver template_driver = {&memory, template_create, template_destroy};
    struct dppd_flow_template_spec shape = {.capacity = count};
    struct dppd_flow_service_config config = {.rule_capacity = count,
        .queue_count = 8,
        .depth = 256,
        .timeout_ns = UINT64_C(10000000000),
        .port_id = 5};
    struct dppd_flow_service *service = NULL;
    struct dppd_async_flow_driver driver =
        {.context = &memory, .create = create, .destroy = destroy, .push = push, .pull = pull, .release = release};
    struct dppd_async_flow_create_request requests[64];
    struct dppd_flow_remove_request removals[64];
    struct dppd_flow_job_id jobs[32];
    uint64_t started[32], samples[256];
    uint32_t sample_count = 0;
    quota.ports[0].port_id = 5;
    for (unsigned int k = 0; k < DPPD_RESOURCE_KIND_COUNT; ++k)
        quota.limits[k] = quota.ports[0].limits[k] = count;
    quota.limits[DPPD_RESOURCE_TEMPLATE_TABLE] = quota.ports[0].limits[DPPD_RESOURCE_TEMPLATE_TABLE] = 64;
    quota.limits[DPPD_RESOURCE_FLOW_QUEUE] = quota.ports[0].limits[DPPD_RESOURCE_FLOW_QUEUE] = 8;
    quota.limits[DPPD_RESOURCE_INFLIGHT_OP] = quota.ports[0].limits[DPPD_RESOURCE_INFLIGHT_OP] = 2048;
    shape.example = (struct dppd_rule){.id = 1, .install_port_id = 5, .nb_matches = 1, .nb_actions = 1};
    shape.example.matches[0].type = DPPD_MATCH_ETH;
    shape.example.actions[0].type = DPPD_ACTION_DROP;
    assert(dppd_admission_create(&quota, &admission) == 0);
    assert(dppd_flow_templates_create(admission, count * 2U, &template_driver, &templates) == 0);
    assert(dppd_flow_template_create(templates, &shape, &table) == 0);
    config.admission = admission;
    config.templates = templates;
    for (unsigned int q = 0; q < 8; ++q) {
        struct dppd_async_flow_config queue = {.admission = admission,
            .templates = templates,
            .driver = driver,
            .flow_resource = DPPD_RESOURCE_HARDWARE_FLOW,
            .port_id = 5,
            .queue_id = q,
            .depth = 256,
            .flow_capacity = count};
        assert(dppd_async_flow_queue_create(&queue, &config.queues[q]) == 0);
    }
    assert(dppd_flow_service_create(&config, &service) == 0);
    uint64_t start = now_ns();
    for (uint32_t offset = 0; offset < count; offset += 2048) {
        /** 32 个批次同时受理，八个逻辑队列均达到 256 深度，计时包含等待前面批次的时间 */
        for (uint32_t batch = 0; batch < 32; ++batch) {
            for (uint32_t i = 0; i < 64; ++i) {
                requests[i] = (struct dppd_async_flow_create_request){.table = table, .rule = shape.example};
                requests[i].rule.id = offset + batch * 64U + i + 1U;
            }
            started[batch] = now_ns();
            assert(dppd_flow_service_submit(service, batch % 8U, requests, 64, started[batch], &jobs[batch]) == 0);
        }
        for (uint32_t batch = 0; batch < 32; ++batch) {
            struct dppd_flow_job_result result;
            uint64_t deadline = started[batch] + UINT64_C(10000000000);
            for (;;) {
                assert(now_ns() < deadline);
                assert(dppd_flow_service_process(service, now_ns(), 64) == 0);
                int rc = dppd_flow_service_collect(service, &jobs[batch], &result);
                if (rc == 0)
                    break;
                assert(rc == -EAGAIN);
            }
            assert(result.status == 0 && result.applied && result.count == 64);
            samples[sample_count++] = now_ns() - started[batch];
            for (uint32_t i = 0; i < 64; ++i)
                assert(result.ids[i] == offset + batch * 64U + i + 1U && result.generations[i] == result.ids[i]);
        }
    }
    uint64_t elapsed = now_ns() - start;
    for (uint32_t i = 0; i < count; ++i) {
        struct dppd_rule rule;
        assert(dppd_flow_service_get(service, i + 1U, &rule) == 0 && rule.generation == i + 1U);
    }
    assert(memory.objects == count);
    qsort(samples, sample_count, sizeof(*samples), compare);
    uint64_t p99 = samples[(sample_count * 99U + 99U) / 100U - 1U];
    for (uint32_t offset = 0; offset < count; offset += 64) {
        struct dppd_flow_job_result result;
        for (uint32_t i = 0; i < 64; ++i)
            removals[i] = (struct dppd_flow_remove_request){offset + i + 1U, offset + i + 1U};
        assert(dppd_flow_service_remove(service, 0, removals, 64, now_ns(), &jobs[0]) == 0);
        uint64_t deadline = now_ns() + UINT64_C(10000000000);
        for (;;) {
            assert(now_ns() < deadline);
            assert(dppd_flow_service_process(service, now_ns(), 64) == 0);
            int rc = dppd_flow_service_collect(service, &jobs[0], &result);
            if (rc == 0)
                break;
            assert(rc == -EAGAIN);
        }
        assert(result.status == 0 && result.applied);
    }
    assert(memory.objects == 0 && dppd_flow_service_close(&service) == 0);
    for (unsigned int q = 0; q < 8; ++q)
        assert(dppd_async_flow_queue_close(&config.queues[q]) == 0);
    assert(dppd_flow_template_destroy(templates, &table) == 0);
    assert(dppd_flow_templates_close(&templates) == 0 && dppd_admission_close(&admission) == 0);
    printf("{\"hardware_acceptance\":\"not_run\",\"path\":\"memory_driver_async_batch_service\","
           "\"rules\":%u,\"batch\":64,\"queues\":8,\"queue_depth\":256,\"window_rules\":2048,"
           "\"samples\":%u,\"install_ns\":%" PRIu64 ",\"completion_p99_ns\":%" PRIu64 "}\n",
        count, sample_count, elapsed, p99);
    return 0;
}
