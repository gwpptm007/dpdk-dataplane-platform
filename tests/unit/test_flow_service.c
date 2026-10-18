#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include "dppd/flow_service.h"
#include "dppd/rule_repository.h"

static unsigned int fail_allocation;
void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_allocation != 0 && --fail_allocation == 0)
        return NULL;
    return __real_calloc(count, size);
}

struct faults {
    struct dppd_async_flow_driver base;
    unsigned int creates, destroys, fail_create_at, fail_destroy_ack_at;
    uintptr_t failed_destroy_tag;
    bool withhold;
};

struct fixture {
    struct dppd_admission *admission;
    struct dppd_flow_templates *templates;
    struct dppd_flow_template_id table;
    struct dppd_software_async_flows *driver;
    struct dppd_software_backend software;
    struct dppd_flow_service *service;
    struct dppd_flow_service_config config;
    struct faults faults;
};

static int create(void *context, uint16_t port, uint32_t queue, void *table, const struct dppd_rule *rule,
    const struct dppd_flow_binding *binding, uintptr_t tag, void **object)
{
    struct faults *faults = context;
    if (++faults->creates == faults->fail_create_at)
        return -ENOMEM;
    return faults->base.create(faults->base.context, port, queue, table, rule, binding, tag, object);
}

static int destroy(void *context, uint16_t port, uint32_t queue, void *object, uintptr_t tag)
{
    struct faults *faults = context;
    if (++faults->destroys == faults->fail_destroy_ack_at)
        faults->failed_destroy_tag = tag;
    return faults->base.destroy(faults->base.context, port, queue, object, tag);
}

static int push(void *context, uint16_t port, uint32_t queue)
{
    struct faults *faults = context;
    return faults->base.push(faults->base.context, port, queue);
}

static int pull(void *context, uint16_t port, uint32_t queue, struct dppd_async_driver_result *results, uint16_t count)
{
    struct faults *faults = context;
    if (faults->withhold)
        return 0;
    int received = faults->base.pull(faults->base.context, port, queue, results, count);
    for (int i = 0; i < received; ++i)
        if (results[i].tag == faults->failed_destroy_tag) {
            results[i].status = -EIO;
            faults->failed_destroy_tag = 0;
        }
    return received;
}

static void release(void *context, void *object)
{
    struct faults *faults = context;
    faults->base.release(faults->base.context, object);
}

static struct dppd_async_flow_create_request request(struct fixture *fixture, uint64_t id)
{
    struct dppd_async_flow_create_request value = {.table = fixture->table};
    value.rule = (struct dppd_rule){.id = id,
        .install_port_id = 7,
        .fallback = DPPD_FALLBACK_SOFTWARE_ONLY,
        .nb_matches = 1,
        .nb_actions = 3};
    value.rule.matches[0].type = DPPD_MATCH_ETH;
    value.rule.actions[0] = (struct dppd_action){.type = DPPD_ACTION_MARK, .conf.mark_id = (uint32_t)id};
    value.rule.actions[1].type = DPPD_ACTION_COUNT;
    value.rule.actions[2].type = DPPD_ACTION_DROP;
    return value;
}

static void setup(struct fixture *fixture, uint32_t capacity, uint32_t depth, uint32_t queues)
{
    struct dppd_admission_config quota = {.ticket_capacity = capacity * 4U + queues + 1U, .port_count = 1};
    struct dppd_flow_template_spec shape = {.capacity = capacity, .dynamic_actions = 1};
    struct dppd_async_flow_driver driver = {.context = &fixture->faults,
        .create = create,
        .destroy = destroy,
        .push = push,
        .pull = pull,
        .release = release};
    quota.ports[0].port_id = 7;
    for (uint32_t k = 0; k < DPPD_RESOURCE_KIND_COUNT; ++k)
        quota.limits[k] = quota.ports[0].limits[k] = capacity > queues * depth ? capacity : queues * depth;
    assert(dppd_admission_create(&quota, &fixture->admission) == 0);
    assert(dppd_flow_templates_create(fixture->admission, capacity * 2U, NULL, &fixture->templates) == 0);
    shape.example = request(fixture, 1).rule;
    assert(dppd_flow_template_create(fixture->templates, &shape, &fixture->table) == 0);
    assert(dppd_software_backend_init(&fixture->software, capacity) == 0);
    assert(dppd_software_async_flows_create(&fixture->driver, &fixture->faults.base) == 0);
    fixture->config = (struct dppd_flow_service_config){.admission = fixture->admission,
        .templates = fixture->templates,
        .software = &fixture->software,
        .rule_capacity = capacity,
        .queue_count = queues,
        .depth = depth,
        .timeout_ns = 1000,
        .port_id = 7};
    for (uint32_t q = 0; q < queues; ++q) {
        struct dppd_async_flow_config config = {.admission = fixture->admission,
            .templates = fixture->templates,
            .driver = driver,
            .flow_resource = DPPD_RESOURCE_SOFTWARE_RULE,
            .port_id = 7,
            .queue_id = (uint16_t)q,
            .depth = depth,
            .flow_capacity = capacity};
        assert(dppd_async_flow_queue_create(&config, &fixture->config.queues[q]) == 0);
    }
    assert(dppd_flow_service_create(&fixture->config, &fixture->service) == 0);
}

static struct dppd_flow_job_result finish(struct fixture *fixture, const struct dppd_flow_job_id *id)
{
    struct dppd_flow_job_result result;
    for (unsigned int i = 0; i < 100; ++i) {
        int rc = dppd_flow_service_collect(fixture->service, id, &result);
        if (rc == 0)
            return result;
        assert(rc == -EAGAIN || rc == -EUCLEAN);
        assert(dppd_flow_service_process(fixture->service, 2, 64) <= 0);
    }
    assert(!"job did not reach a complete state");
    return result;
}

static void remove_all(struct fixture *fixture, uint32_t count)
{
    struct dppd_flow_remove_request requests[64];
    struct dppd_flow_job_id job;
    struct dppd_rule rule;
    for (uint32_t i = 0; i < count; ++i) {
        assert(dppd_flow_service_get(fixture->service, i + 1U, &rule) == 0);
        requests[i] = (struct dppd_flow_remove_request){rule.id, rule.generation};
    }
    assert(dppd_flow_service_remove(fixture->service, 0, requests, count, 1, &job) == 0);
    struct dppd_flow_job_result result = finish(fixture, &job);
    assert(result.status == 0 && result.applied && result.remove);
}

static void teardown(struct fixture *fixture)
{
    assert(dppd_flow_service_close(&fixture->service) == 0);
    for (uint32_t q = 0; q < fixture->config.queue_count; ++q)
        assert(dppd_async_flow_queue_close(&fixture->config.queues[q]) == 0);
    assert(dppd_software_async_flows_close(&fixture->driver) == 0);
    dppd_software_backend_fini(&fixture->software);
    assert(dppd_flow_template_destroy(fixture->templates, &fixture->table) == 0);
    assert(dppd_flow_templates_close(&fixture->templates) == 0);
    assert(dppd_admission_close(&fixture->admission) == 0);
}

static void test_batch_packet_and_backpressure(void)
{
    struct fixture fixture = {0};
    struct dppd_async_flow_create_request requests[64];
    struct dppd_flow_job_id job, duplicate = {.owner = 999};
    struct dppd_flow_job_result result;
    struct dppd_flow_service_status status;
    struct dppd_admission_snapshot usage;
    struct dppd_packet packet = {.packet_len = 100};
    struct dppd_software_decision decision;
    struct dppd_rule rule;
    setup(&fixture, 64, 64, 8);
    struct dppd_flow_service *other = NULL;
    assert(dppd_flow_service_create(&fixture.config, &other) == -EBUSY && other == NULL);
    assert(dppd_async_flow_queue_close(&fixture.config.queues[0]) == -EBUSY);
    for (uint32_t i = 0; i < 64; ++i)
        requests[i] = request(&fixture, i + 1U);
    assert(dppd_flow_service_submit(fixture.service, 0, requests, 64, 1, &job) == 0);
    assert(
        dppd_flow_service_submit(fixture.service, 0, requests, 1, 1, &duplicate) == -EAGAIN && duplicate.owner == 999);
    assert(dppd_flow_service_submit(fixture.service, 1, requests, 1, 1, &duplicate) == -EEXIST);
    assert(dppd_flow_service_get(fixture.service, 1, &rule) == -ENOENT);
    assert(dppd_software_backend_count(&fixture.software) == 0 && fixture.faults.creates == 0);
    assert(dppd_flow_template_destroy(fixture.templates, &fixture.table) == -EBUSY);
    assert(dppd_flow_service_process(fixture.service, 2, 1) == 0);
    assert(dppd_flow_service_get(fixture.service, 1, &rule) == -ENOENT);
    assert(dppd_software_backend_count(&fixture.software) == 0);
    assert(dppd_flow_service_process(fixture.service, 2, 64) == 0);
    assert(dppd_flow_service_submit(fixture.service, 0, requests, 1, 1, &duplicate) == -EAGAIN);
    result = finish(&fixture, &job);
    assert(result.status == 0 && result.applied && result.count == 64);
    assert(dppd_flow_service_collect(fixture.service, &job, &result) == -ESTALE);
    assert(dppd_flow_service_status(fixture.service, &status) == 0 && status.rules == 64 && status.revision == 64);
    assert(dppd_software_backend_count(&fixture.software) == 64);
    assert(dppd_admission_snapshot(fixture.admission, &usage) == 0);
    assert(
        usage.global.committed[DPPD_RESOURCE_RULE] == 64 && usage.global.committed[DPPD_RESOURCE_SOFTWARE_RULE] == 64);
    assert(dppd_software_backend_worker_register(&fixture.software, 0) == 0);
    dppd_software_backend_decide(&fixture.software, 7, &packet, &decision);
    assert(decision.matched && decision.drop && decision.has_mark && decision.mark_id == 1);
    dppd_software_backend_worker_quiescent(&fixture.software, 0);
    dppd_software_backend_worker_unregister(&fixture.software, 0);
    for (uint32_t i = 0; i < 64; ++i) {
        assert(dppd_flow_service_get(fixture.service, i + 1U, &rule) == 0);
        assert(rule.generation == i + 1U && result.generations[i] == i + 1U);
    }
    remove_all(&fixture, 64);
    assert(dppd_software_backend_count(&fixture.software) == 0);
    teardown(&fixture);
}

static void test_failures_and_residual_retry(void)
{
    for (unsigned int failure = 0; failure < 3; ++failure) {
        struct fixture fixture = {0};
        struct dppd_async_flow_create_request requests[3];
        struct dppd_flow_job_id job, untouched = {.owner = 999};
        struct dppd_flow_job_result result;
        struct dppd_rule rule;
        setup(&fixture, 3, 4, 1);
        for (unsigned int i = 0; i < 3; ++i)
            requests[i] = request(&fixture, i + 1U);
        assert(dppd_flow_service_submit(fixture.service, 0, requests, 3, 1, &job) == 0);
        if (failure == 0)
            fail_allocation = 4;
        else {
            fixture.faults.fail_create_at = 2;
            if (failure == 2)
                fixture.faults.fail_destroy_ack_at = 1;
        }
        if (failure == 2) {
            int rc = -EAGAIN;
            for (unsigned int attempt = 0; attempt < 10 && rc == -EAGAIN; ++attempt) {
                dppd_flow_service_process(fixture.service, 2, 64);
                rc = dppd_flow_service_collect(fixture.service, &job, &result);
            }
            assert(rc == -EUCLEAN);
            assert(dppd_flow_service_submit(fixture.service, 0, requests, 1, 1, &untouched) == -EUCLEAN);
            assert(untouched.owner == 999);
            assert(dppd_flow_service_retry(fixture.service) == 0);
        }
        result = finish(&fixture, &job);
        assert(!result.applied && result.status == -ENOMEM);
        assert(dppd_flow_service_get(fixture.service, 1, &rule) == -ENOENT);
        assert(dppd_software_backend_count(&fixture.software) == 0);
        assert(dppd_flow_service_retry(fixture.service) == 0);
        assert(dppd_flow_service_submit(fixture.service, 0, requests, 3, 1, &job) == 0);
        result = finish(&fixture, &job);
        assert(result.status == 0 && result.applied);
        remove_all(&fixture, 3);
        teardown(&fixture);
    }
}

static void test_delete_restoration(void)
{
    for (unsigned int failure = 0; failure < 3; ++failure) {
        struct fixture fixture = {0};
        struct dppd_async_flow_create_request requests[3];
        struct dppd_flow_remove_request removals[3];
        struct dppd_flow_job_id job;
        struct dppd_flow_job_result result;
        struct dppd_rule rule;
        setup(&fixture, 3, 4, 2);
        for (unsigned int i = 0; i < 3; ++i)
            requests[i] = request(&fixture, i + 1U);
        assert(dppd_flow_service_submit(fixture.service, 1, requests, 3, 1, &job) == 0);
        result = finish(&fixture, &job);
        assert(result.applied);
        for (unsigned int i = 0; i < 3; ++i)
            removals[i] = (struct dppd_flow_remove_request){i + 1U, i + 1U};
        assert(dppd_flow_service_remove(fixture.service, 0, removals, 3, 1, &job) == 0);
        if (failure == 0)
            fail_allocation = 1;
        else {
            fixture.faults.fail_destroy_ack_at = 1;
            if (failure == 2)
                fixture.faults.fail_create_at = fixture.faults.creates + 1U;
        }
        if (failure == 2) {
            int rc = -EAGAIN;
            for (unsigned int attempt = 0; attempt < 15 && rc == -EAGAIN; ++attempt) {
                dppd_flow_service_process(fixture.service, 2, 64);
                rc = dppd_flow_service_collect(fixture.service, &job, &result);
            }
            assert(rc == -EUCLEAN);
            assert(dppd_flow_service_retry(fixture.service) == 0);
        }
        result = finish(&fixture, &job);
        assert(!result.applied && result.status == (failure == 0 ? -ENOMEM : -EIO));
        assert(dppd_software_backend_count(&fixture.software) == 3);
        for (unsigned int i = 0; i < 3; ++i) {
            assert(dppd_flow_service_get(fixture.service, i + 1U, &rule) == 0);
            assert(rule.generation == i + 1U);
        }
        assert(dppd_flow_service_retry(fixture.service) == 0);
        remove_all(&fixture, 3);
        teardown(&fixture);
    }
}

static void test_cancellation_timeout_and_allocations(void)
{
    struct fixture fixture = {0};
    struct dppd_flow_job_id job;
    struct dppd_flow_job_result result;
    setup(&fixture, 4, 4, 1);
    struct dppd_async_flow_create_request req = request(&fixture, 1);
    assert(dppd_flow_service_submit(fixture.service, 0, &req, 1, 1, &job) == 0);
    assert(dppd_flow_service_cancel(fixture.service, &job) == 0);
    assert(dppd_flow_service_collect(fixture.service, &job, &result) == 0 && result.status == -ECANCELED);
    assert(dppd_flow_service_submit(fixture.service, 0, &req, 1, 1, &job) == 0);
    assert(dppd_flow_service_process(fixture.service, 2000, 64) == 0);
    result = finish(&fixture, &job);
    assert(!result.applied && result.status == -ETIMEDOUT && fixture.faults.creates == 0);
    assert(dppd_flow_service_close(&fixture.service) == 0);
    for (unsigned int i = 1; i <= 6; ++i) {
        fail_allocation = i;
        assert(dppd_flow_service_create(&fixture.config, &fixture.service) == -ENOMEM && fixture.service == NULL);
    }
    assert(dppd_flow_service_create(&fixture.config, &fixture.service) == 0);
    teardown(&fixture);
}

struct producer {
    struct fixture *fixture;
    pthread_barrier_t *barrier;
    struct dppd_flow_job_id job;
    uint64_t id;
    int status;
};

static void *produce(void *context)
{
    struct producer *producer = context;
    struct dppd_async_flow_create_request req = request(producer->fixture, producer->id);
    pthread_barrier_wait(producer->barrier);
    producer->status = dppd_flow_service_submit(producer->fixture->service, 0, &req, 1, 1, &producer->job);
    return NULL;
}

static void test_concurrent_admission(void)
{
    struct fixture fixture = {0};
    struct producer producers[8];
    struct dppd_flow_remove_request removals[4];
    pthread_t threads[8];
    pthread_barrier_t barrier;
    unsigned int accepted = 0, rejected = 0;
    setup(&fixture, 8, 4, 1);
    assert(pthread_barrier_init(&barrier, NULL, 8) == 0);
    for (unsigned int i = 0; i < 8; ++i) {
        producers[i] = (struct producer){.fixture = &fixture, .barrier = &barrier, .id = i + 1U};
        assert(pthread_create(&threads[i], NULL, produce, &producers[i]) == 0);
    }
    for (unsigned int i = 0; i < 8; ++i) {
        assert(pthread_join(threads[i], NULL) == 0);
        rejected += producers[i].status == -EAGAIN;
        accepted += producers[i].status == 0;
    }
    assert(accepted == 4 && rejected == 4 && fixture.faults.creates == 0);
    unsigned int count = 0;
    for (unsigned int i = 0; i < 8; ++i)
        if (producers[i].status == 0) {
            struct dppd_flow_job_result result = finish(&fixture, &producers[i].job);
            assert(result.applied && result.status == 0);
            removals[count++] = (struct dppd_flow_remove_request){producers[i].id, result.generations[0]};
        }
    struct dppd_flow_job_id job;
    assert(dppd_flow_service_remove(fixture.service, 0, removals, 4, 1, &job) == 0);
    assert(finish(&fixture, &job).applied);
    pthread_barrier_destroy(&barrier);
    teardown(&fixture);
}

static void test_resource_exhaustion(void)
{
    struct fixture fixture = {0};
    struct dppd_admission_config limits = {.ticket_capacity = 14, .port_count = 1};
    struct dppd_admission_snapshot usage;
    struct dppd_async_flow_create_request requests[3];
    struct dppd_flow_job_id job = {.owner = 999};
    setup(&fixture, 3, 4, 1);
    limits.ports[0].port_id = 7;
    for (unsigned int k = 0; k < DPPD_RESOURCE_KIND_COUNT; ++k)
        limits.limits[k] = limits.ports[0].limits[k] = 4;
    for (unsigned int i = 0; i < 3; ++i)
        requests[i] = request(&fixture, i + 1U);
    limits.limits[DPPD_RESOURCE_RULE] = 2;
    assert(dppd_admission_snapshot(fixture.admission, &usage) == 0);
    assert(dppd_admission_set_limits(fixture.admission, &limits, usage.revision) == 0);
    assert(dppd_flow_service_submit(fixture.service, 0, requests, 3, 1, &job) == -ENOSPC && job.owner == 999);
    assert(dppd_admission_snapshot(fixture.admission, &usage) == 0);
    assert(usage.global.reserved[DPPD_RESOURCE_RULE] == 0 && usage.held_tickets == 2);
    limits.limits[DPPD_RESOURCE_RULE] = 4;
    limits.limits[DPPD_RESOURCE_SOFTWARE_RULE] = 2;
    assert(dppd_admission_set_limits(fixture.admission, &limits, usage.revision) == 0);
    assert(dppd_flow_service_submit(fixture.service, 0, requests, 3, 1, &job) == 0);
    struct dppd_flow_job_result result = finish(&fixture, &job);
    assert(!result.applied && result.status == -ENOSPC && fixture.faults.creates == 0);
    assert(dppd_software_backend_count(&fixture.software) == 0);
    teardown(&fixture);
}

static void test_active_timeout_preserves_existing_rules(void)
{
    struct fixture fixture = {0};
    struct dppd_async_flow_create_request requests[2];
    struct dppd_flow_job_id job;
    struct dppd_flow_job_result result;
    struct dppd_admission_snapshot usage;
    setup(&fixture, 4, 4, 1);
    requests[0] = request(&fixture, 1);
    assert(dppd_flow_service_submit(fixture.service, 0, requests, 1, 1, &job) == 0);
    assert(finish(&fixture, &job).applied);
    requests[0] = request(&fixture, 2);
    requests[1] = request(&fixture, 3);
    fixture.faults.withhold = true;
    assert(dppd_flow_service_submit(fixture.service, 0, requests, 2, 1, &job) == 0);
    assert(dppd_flow_service_process(fixture.service, 2, 64) == 0);
    assert(dppd_flow_service_process(fixture.service, 2000, 64) == -EUCLEAN);
    assert(dppd_flow_service_cancel(fixture.service, &job) == -EBUSY);
    assert(dppd_flow_service_collect(fixture.service, &job, &result) == -EAGAIN);
    assert(dppd_admission_snapshot(fixture.admission, &usage) == 0);
    assert(usage.global.committed[DPPD_RESOURCE_RULE] + usage.global.reserved[DPPD_RESOURCE_RULE] == 3);
    assert(dppd_software_backend_count(&fixture.software) == 1);
    fixture.faults.withhold = false;
    result = finish(&fixture, &job);
    assert(!result.applied && result.status == -EUCLEAN);
    dppd_flow_service_process(fixture.service, 2, 64);
    assert(dppd_flow_service_retry(fixture.service) == 0);
    assert(dppd_software_backend_count(&fixture.software) == 1);
    remove_all(&fixture, 1);
    teardown(&fixture);
}

static void test_repository_batch_boundaries(void)
{
    struct dppd_rule_repository repository;
    struct fixture unused = {0};
    struct dppd_rule rules[2] = {request(&unused, 1).rule, request(&unused, 2).rule};
    struct dppd_rule read;
    rules[0].generation = 1;
    rules[1].generation = 99;
    assert(dppd_rule_repository_init(&repository, 2) == 0);
    assert(dppd_rule_repository_create_batch(&repository, rules, 2) == -EINVAL);
    assert(repository.count == 0 && repository.index.count == 0 && repository.generation == 0);
    rules[1].generation = 2;
    assert(dppd_rule_repository_create_batch(&repository, rules, 2) == 0 && repository.generation == 2);
    rules[1].generation = 99;
    assert(dppd_rule_repository_remove_batch(&repository, rules, 2) == -ESTALE && repository.count == 2);
    assert(dppd_rule_repository_get(&repository, 1, &read) == 0 && read.generation == 1);
    rules[1].generation = 2;
    assert(dppd_rule_repository_remove_batch(&repository, rules, 2) == 0 && repository.generation == 4);
    repository.generation = UINT64_MAX - 2U;
    rules[0].generation = UINT64_MAX - 1U;
    rules[1].generation = UINT64_MAX;
    assert(dppd_rule_repository_create_batch(&repository, rules, 2) == -EOVERFLOW && repository.count == 0);
    dppd_rule_repository_destroy(&repository);
}

int main(void)
{
    test_batch_packet_and_backpressure();
    test_failures_and_residual_retry();
    test_delete_restoration();
    test_cancellation_timeout_and_allocations();
    test_concurrent_admission();
    test_resource_exhaustion();
    test_active_timeout_preserves_existing_rules();
    test_repository_batch_boundaries();
    return 0;
}
