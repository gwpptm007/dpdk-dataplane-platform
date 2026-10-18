#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "dppd/flow_service.h"

struct model_queue {
    struct dppd_async_driver_result completed[64];
    uintptr_t aged[64];
    uint32_t count, ready, aged_count;
};

struct model {
    struct model_queue queues[2];
    uintptr_t tags[64];
    unsigned int objects, creates, fail_deletes;
    bool withhold;
    int query_error;
    int pattern, actions, table;
};

struct fixture {
    struct model model;
    struct dppd_admission_config limits;
    struct dppd_admission *admission;
    struct dppd_flow_templates *templates;
    struct dppd_flow_template_id table;
    struct dppd_async_flow_queue *queues[2];
    struct dppd_flow_service *service;
};

/** 只模拟驱动通知和完成，正式模板、队列、额度、版本与补偿由生产代码负责 */
static int create(void *context, uint16_t port, uint32_t queue, void *table, const struct dppd_rule *rule,
    uintptr_t tag, void **object)
{
    struct model *model = context;
    assert(port == 7 && queue < 2 && table == &model->table && rule->id < 64);
    *object = malloc(1);
    assert(*object != NULL);
    ++model->objects;
    ++model->creates;
    model->tags[rule->id] = tag;
    struct model_queue *pending = &model->queues[queue];
    assert(pending->count < 64);
    pending->completed[pending->count++] = (struct dppd_async_driver_result){tag, 0};
    return 0;
}

static int destroy(void *context, uint16_t port, uint32_t queue, void *object, uintptr_t tag)
{
    struct model *model = context;
    assert(port == 7 && queue < 2 && object != NULL);
    struct model_queue *pending = &model->queues[queue];
    int status = 0;
    if (model->fail_deletes != 0) {
        --model->fail_deletes;
        status = -EIO;
    }
    pending->completed[pending->count++] = (struct dppd_async_driver_result){tag, status};
    return 0;
}

static int push(void *context, uint16_t port, uint32_t queue)
{
    struct model *model = context;
    assert(port == 7 && queue < 2);
    model->queues[queue].ready = model->queues[queue].count;
    return 0;
}

static int pull(void *context, uint16_t port, uint32_t queue, struct dppd_async_driver_result *results, uint16_t count)
{
    struct model *model = context;
    struct model_queue *pending = &model->queues[queue];
    assert(port == 7 && queue < 2);
    if (model->withhold)
        return 0;
    uint32_t copied = 0;
    while (pending->ready != 0 && copied < count) {
        results[copied++] = pending->completed[--pending->count];
        --pending->ready;
    }
    return (int)copied;
}

static void release(void *context, void *object)
{
    struct model *model = context;
    assert(model->objects != 0);
    --model->objects;
    free(object);
}

static int aged(void *context, uint16_t port, uint32_t queue, uintptr_t *tags, uint32_t count)
{
    struct model *model = context;
    struct model_queue *pending = &model->queues[queue];
    assert(port == 7 && queue < 2);
    if (model->query_error != 0)
        return model->query_error;
    uint32_t copied = count < pending->aged_count ? count : pending->aged_count;
    memcpy(tags, pending->aged, copied * sizeof(*tags));
    pending->aged_count -= copied;
    memmove(pending->aged, pending->aged + copied, pending->aged_count * sizeof(*tags));
    return (int)copied;
}

static int template_create(void *context, const struct dppd_flow_template_spec *spec,
    struct dppd_flow_template_parts *parts)
{
    struct model *model = context;
    assert(spec->example.install_port_id == 7);
    *parts = (struct dppd_flow_template_parts){&model->pattern, &model->actions, &model->table};
    return 0;
}

static int template_destroy(void *context, uint16_t port, struct dppd_flow_template_parts *parts)
{
    (void)context;
    assert(port == 7);
    *parts = (struct dppd_flow_template_parts){0};
    return 0;
}

static struct dppd_async_flow_create_request request(struct fixture *fixture, uint64_t id)
{
    struct dppd_async_flow_create_request value = {.table = fixture->table};
    value.rule = (struct dppd_rule){.id = id, .install_port_id = 7, .nb_matches = 1, .nb_actions = 3};
    value.rule.matches[0].type = DPPD_MATCH_ETH;
    value.rule.actions[0].type = DPPD_ACTION_COUNT;
    value.rule.actions[1] = (struct dppd_action){.type = DPPD_ACTION_AGE, .conf.age_timeout_seconds = 1};
    value.rule.actions[2].type = DPPD_ACTION_DROP;
    return value;
}

static void setup(struct fixture *fixture)
{
    fixture->limits = (struct dppd_admission_config){.ticket_capacity = 64, .port_count = 1};
    fixture->limits.ports[0].port_id = 7;
    for (uint32_t kind = 0; kind < DPPD_RESOURCE_KIND_COUNT; ++kind)
        fixture->limits.limits[kind] = fixture->limits.ports[0].limits[kind] = 4;
    assert(dppd_admission_create(&fixture->limits, &fixture->admission) == 0);
    struct dppd_flow_template_driver templates = {&fixture->model, template_create, template_destroy};
    assert(dppd_flow_templates_create(fixture->admission, 8, &templates, &fixture->templates) == 0);
    struct dppd_flow_template_spec shape = {.example = request(fixture, 1).rule, .capacity = 4};
    assert(dppd_flow_template_create(fixture->templates, &shape, &fixture->table) == 0);
    struct dppd_async_flow_driver driver = {.context = &fixture->model,
        .create = create,
        .destroy = destroy,
        .push = push,
        .pull = pull,
        .release = release,
        .aged = aged};
    for (unsigned int q = 0; q < 2; ++q) {
        struct dppd_async_flow_config queue = {.admission = fixture->admission,
            .templates = fixture->templates,
            .driver = driver,
            .flow_resource = DPPD_RESOURCE_HARDWARE_FLOW,
            .port_id = 7,
            .queue_id = q,
            .depth = 2,
            .flow_capacity = 4};
        assert(dppd_async_flow_queue_create(&queue, &fixture->queues[q]) == 0);
    }
    struct dppd_flow_service_config service = {.admission = fixture->admission,
        .templates = fixture->templates,
        .queues = {fixture->queues[0], fixture->queues[1]},
        .rule_capacity = 4,
        .queue_count = 2,
        .depth = 2,
        .timeout_ns = UINT64_C(10000000000),
        .port_id = 7};
    assert(dppd_flow_service_create(&service, &fixture->service) == 0);
}

static struct dppd_flow_service_status status(struct fixture *fixture)
{
    struct dppd_flow_service_status value;
    assert(dppd_flow_service_status(fixture->service, &value) == 0);
    return value;
}

static struct dppd_flow_job_result finish(struct fixture *fixture, const struct dppd_flow_job_id *job, uint64_t now_ns)
{
    struct dppd_flow_job_result result;
    for (unsigned int i = 0; i < 40; ++i) {
        assert(dppd_flow_service_process(fixture->service, now_ns, 64) == 0);
        int rc = dppd_flow_service_collect(fixture->service, job, &result);
        if (rc == 0)
            return result;
        assert(rc == -EAGAIN);
    }
    assert(!"incomplete task");
    return result;
}

static void notify(struct fixture *fixture, uint32_t q, uintptr_t tag)
{
    struct model_queue *queue = &fixture->model.queues[q];
    assert(queue->aged_count < 64);
    queue->aged[queue->aged_count++] = tag;
}

static void drain(struct fixture *fixture, uint64_t now_ns)
{
    for (unsigned int i = 0; i < 16; ++i)
        assert(dppd_flow_service_process(fixture->service, now_ns, 64) == 0);
}

static void teardown(struct fixture *fixture)
{
    struct dppd_admission_snapshot usage;
    assert(fixture->model.objects == 0);
    assert(dppd_admission_snapshot(fixture->admission, &usage) == 0);
    assert(usage.global.committed[DPPD_RESOURCE_AGING_OBJECT] == 0 &&
        usage.global.reserved[DPPD_RESOURCE_AGING_OBJECT] == 0);
    assert(dppd_flow_service_close(&fixture->service) == 0);
    for (unsigned int q = 0; q < 2; ++q)
        assert(dppd_async_flow_queue_close(&fixture->queues[q]) == 0);
    assert(dppd_flow_template_destroy(fixture->templates, &fixture->table) == 0);
    assert(dppd_flow_templates_close(&fixture->templates) == 0);
    assert(dppd_admission_close(&fixture->admission) == 0);
}

/** 用户完成回执占满应用深度时，独立且有界的维护任务仍能回收实际流及全部引用 */
static void test_completed_backpressure_and_stale_identity(void)
{
    struct fixture fixture = {0};
    struct dppd_flow_job_id job;
    struct dppd_rule rule;
    setup(&fixture);
    struct dppd_async_flow_create_request requests[2] = {request(&fixture, 1), request(&fixture, 2)};
    assert(dppd_flow_service_submit(fixture.service, 0, requests, 2, 1, &job) == 0);
    assert(dppd_flow_service_process(fixture.service, 2, 64) == 0);
    assert(status(&fixture).completed == 1 && status(&fixture).rules == 2);
    uintptr_t old = fixture.model.tags[1];
    notify(&fixture, 0, old);
    notify(&fixture, 0, old);
    notify(&fixture, 0, fixture.model.tags[2]);
    notify(&fixture, 0, UINTPTR_MAX);
    drain(&fixture, UINT64_C(200000000));
    drain(&fixture, UINT64_C(400000000));
    assert(status(&fixture).rules == 0 && status(&fixture).aged_deleted == 2 && status(&fixture).age_jobs == 0);
    assert(status(&fixture).completed == 1 && fixture.model.objects == 0);
    struct dppd_flow_job_result result;
    assert(dppd_flow_service_collect(fixture.service, &job, &result) == 0 && result.applied);
    assert(dppd_flow_service_submit(fixture.service, 0, requests, 1, UINT64_C(400000001), &job) == 0);
    assert(finish(&fixture, &job, UINT64_C(400000002)).applied);
    notify(&fixture, 0, old);
    drain(&fixture, UINT64_C(600000000));
    assert(dppd_flow_service_get(fixture.service, 1, &rule) == 0 && rule.generation == 5);
    struct dppd_async_flow_status queue;
    assert(dppd_async_flow_status(fixture.queues[0], &queue) == 0 && queue.aged_stale == 2);
    notify(&fixture, 0, fixture.model.tags[1]);
    drain(&fixture, UINT64_C(800000000));
    assert(status(&fixture).rules == 0 && status(&fixture).aged_deleted == 3);
    teardown(&fixture);
}

static void test_early_notification_and_query_failure(void)
{
    struct fixture fixture = {0};
    struct dppd_flow_job_id job;
    struct dppd_async_aged_flow events[2];
    setup(&fixture);
    struct dppd_async_flow_create_request req = request(&fixture, 1);
    fixture.model.withhold = true;
    assert(dppd_flow_service_submit(fixture.service, 0, &req, 1, 1, &job) == 0);
    assert(dppd_flow_service_process(fixture.service, 2, 64) == 0);
    notify(&fixture, 0, fixture.model.tags[1]);
    assert(dppd_async_flow_aged(fixture.queues[0], events, 2) == 0);
    assert(fixture.model.objects == 1 && status(&fixture).rules == 0);
    fixture.model.withhold = false;
    assert(finish(&fixture, &job, 3).applied);
    fixture.model.query_error = -EIO;
    drain(&fixture, UINT64_C(200000000));
    assert(status(&fixture).rules == 1 && status(&fixture).age_query_errors == 2 &&
        status(&fixture).age_query_error == -EIO);
    fixture.model.query_error = 0;
    drain(&fixture, UINT64_C(400000000));
    assert(status(&fixture).rules == 0 && status(&fixture).aged_deleted == 1 && status(&fixture).age_query_error == 0);
    teardown(&fixture);
}

static void test_query_budget_fairness_and_retry(void)
{
    struct fixture fixture = {0};
    struct dppd_flow_job_id job;
    setup(&fixture);
    for (unsigned int q = 0; q < 2; ++q) {
        struct dppd_async_flow_create_request req = request(&fixture, q + 1U);
        assert(dppd_flow_service_submit(fixture.service, q, &req, 1, 1, &job) == 0);
        assert(finish(&fixture, &job, 2).applied);
    }
    for (unsigned int q = 0; q < 2; ++q)
        notify(&fixture, q, fixture.model.tags[q + 1U]);
    fixture.model.fail_deletes = 1;
    assert(dppd_flow_service_age_poll(fixture.service, UINT64_C(200000000), 1) == 1);
    drain(&fixture, UINT64_C(200000000));
    assert(status(&fixture).rules == 2 && status(&fixture).age_delete_errors == 1 && status(&fixture).age_jobs == 0);
    assert(dppd_flow_service_age_poll(fixture.service, UINT64_C(400000000), 1) == 0);
    assert(dppd_flow_service_age_poll(fixture.service, UINT64_C(1400000000), 1) == 1);
    drain(&fixture, UINT64_C(1400000000));
    assert(status(&fixture).rules == 1);
    assert(dppd_flow_service_age_poll(fixture.service, UINT64_C(1600000000), 1) == 1);
    drain(&fixture, UINT64_C(1600000000));
    assert(status(&fixture).rules == 0 && status(&fixture).aged_deleted == 2);
    teardown(&fixture);
}

static void test_aging_object_exhaustion(void)
{
    struct fixture fixture = {0};
    struct dppd_flow_job_id job;
    struct dppd_admission_snapshot usage;
    setup(&fixture);
    fixture.limits.limits[DPPD_RESOURCE_AGING_OBJECT] = 0;
    assert(dppd_admission_snapshot(fixture.admission, &usage) == 0);
    assert(dppd_admission_set_limits(fixture.admission, &fixture.limits, usage.revision) == 0);
    struct dppd_async_flow_create_request req = request(&fixture, 1);
    assert(dppd_flow_service_submit(fixture.service, 0, &req, 1, 1, &job) == 0);
    struct dppd_flow_job_result result = finish(&fixture, &job, 2);
    assert(!result.applied && result.status == -ENOSPC && fixture.model.creates == 0 && status(&fixture).rules == 0);
    teardown(&fixture);
}

/** 补偿可能重建同一 generation，旧 flow 的迟到通知也不能删除计时已经重置的新句柄 */
static void test_partial_delete_same_version_restoration(void)
{
    struct fixture fixture = {0};
    struct dppd_flow_job_id job;
    struct dppd_rule rule;
    setup(&fixture);
    struct dppd_async_flow_create_request requests[2] = {request(&fixture, 1), request(&fixture, 2)};
    assert(dppd_flow_service_submit(fixture.service, 0, requests, 2, 1, &job) == 0);
    assert(finish(&fixture, &job, 2).applied);
    uintptr_t old_second = fixture.model.tags[2];
    notify(&fixture, 0, fixture.model.tags[1]);
    notify(&fixture, 0, old_second);
    fixture.model.fail_deletes = 1;
    drain(&fixture, UINT64_C(200000000));
    assert(status(&fixture).rules == 2 && status(&fixture).age_delete_errors == 1);
    assert(status(&fixture).age_jobs == 0 && fixture.model.tags[2] != old_second);
    assert(dppd_flow_service_get(fixture.service, 2, &rule) == 0 && rule.generation == 2);
    notify(&fixture, 0, old_second);
    drain(&fixture, UINT64_C(1400000000));
    assert(status(&fixture).rules == 1 && status(&fixture).aged_deleted == 1);
    assert(dppd_flow_service_get(fixture.service, 2, &rule) == 0 && rule.generation == 2);
    notify(&fixture, 0, fixture.model.tags[2]);
    drain(&fixture, UINT64_C(1600000000));
    assert(status(&fixture).rules == 0 && status(&fixture).aged_deleted == 2);
    teardown(&fixture);
}

int main(void)
{
    test_completed_backpressure_and_stale_identity();
    test_early_notification_and_query_failure();
    test_query_budget_fairness_and_retry();
    test_aging_object_exhaustion();
    test_partial_delete_same_version_restoration();
    return 0;
}
