#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "dppd/async_flow.h"

struct driver_state {
    struct dppd_async_driver_result results[256];
    unsigned int count, creates, destroys, releases, create_error_at, create_fail_at;
    int destroy_error, destroy_failure, push_error, pull_error;
    bool withhold, pushed, foreign, duplicate;
    uintptr_t last_tag;
    _Atomic unsigned int active;
};

struct fixture {
    struct dppd_admission *admission;
    struct dppd_flow_templates *templates;
    struct dppd_flow_template_id table;
    struct dppd_async_flow_queue *queue;
    struct dppd_async_flow_config config;
    struct driver_state driver;
};

static unsigned int fail_allocation;
void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_allocation != 0 && --fail_allocation == 0)
        return NULL;
    return __real_calloc(count, size);
}

static int create(void *context, uint16_t port, uint32_t queue, void *table, const struct dppd_rule *rule,
    uintptr_t tag, void **object)
{
    struct driver_state *state = context;
    (void)table;
    assert(port == 7 && queue == 0 && rule->generation != 0 && tag != 0);
    assert(atomic_fetch_add(&state->active, 1) == 0);
    ++state->creates;
    if (state->create_error_at == state->creates) {
        atomic_fetch_sub(&state->active, 1);
        return -ENOSPC;
    }
    *object = malloc(1);
    assert(*object != NULL);
    state->results[state->count++] =
        (struct dppd_async_driver_result){tag, state->create_fail_at == state->creates ? -EIO : 0};
    state->pushed = false;
    atomic_fetch_sub(&state->active, 1);
    return 0;
}

static int destroy(void *context, uint16_t port, uint32_t queue, void *object, uintptr_t tag)
{
    struct driver_state *state = context;
    assert(port == 7 && queue == 0 && object != NULL && tag != 0);
    assert(atomic_fetch_add(&state->active, 1) == 0);
    ++state->destroys;
    if (state->destroy_error != 0) {
        atomic_fetch_sub(&state->active, 1);
        return state->destroy_error;
    }
    state->results[state->count++] = (struct dppd_async_driver_result){tag, state->destroy_failure};
    state->pushed = false;
    atomic_fetch_sub(&state->active, 1);
    return 0;
}

static int push(void *context, uint16_t port, uint32_t queue)
{
    struct driver_state *state = context;
    assert(port == 7 && queue == 0 && atomic_fetch_add(&state->active, 1) == 0);
    if (state->push_error == 0)
        state->pushed = true;
    atomic_fetch_sub(&state->active, 1);
    return state->push_error;
}

static int pull(void *context, uint16_t port, uint32_t queue, struct dppd_async_driver_result *results, uint16_t count)
{
    struct driver_state *state = context;
    unsigned int output = 0;
    assert(port == 7 && queue == 0 && count <= 64);
    assert(atomic_fetch_add(&state->active, 1) == 0);
    if (state->pull_error != 0) {
        atomic_fetch_sub(&state->active, 1);
        return state->pull_error;
    }
    if (state->foreign || state->duplicate) {
        results[output++] = (struct dppd_async_driver_result){state->foreign ? UINTPTR_MAX : state->last_tag, 0};
        state->foreign = state->duplicate = false;
    } else if (!state->withhold && state->pushed) {
        /** 故意倒序完成，队列必须用独立身份关联，不能按提交顺序猜测 */
        while (state->count != 0 && output < count) {
            results[output++] = state->results[--state->count];
            state->last_tag = results[output - 1].tag;
        }
    }
    atomic_fetch_sub(&state->active, 1);
    return (int)output;
}

static void release(void *context, void *object)
{
    struct driver_state *state = context;
    assert(object != NULL);
    ++state->releases;
    free(object);
}

static struct dppd_async_flow_create_request request(struct fixture *fixture, uint64_t id)
{
    struct dppd_async_flow_create_request value = {.table = fixture->table};
    value.rule = (struct dppd_rule){.id = id,
        .generation = id + 10,
        .install_port_id = 7,
        .fallback = DPPD_FALLBACK_SOFTWARE_ONLY,
        .nb_matches = 1,
        .nb_actions = 1};
    value.rule.matches[0].type = DPPD_MATCH_ETH;
    value.rule.actions[0].type = DPPD_ACTION_DROP;
    return value;
}

static void setup(struct fixture *fixture, uint32_t depth)
{
    struct dppd_admission_config quota = {.ticket_capacity = 128, .port_count = 1};
    struct dppd_flow_template_spec shape = {.capacity = 64};
    quota.ports[0].port_id = 7;
    for (unsigned int kind = 0; kind < DPPD_RESOURCE_KIND_COUNT; ++kind)
        quota.limits[kind] = quota.ports[0].limits[kind] = 64;
    assert(dppd_admission_create(&quota, &fixture->admission) == 0);
    assert(dppd_flow_templates_create(fixture->admission, 64, NULL, &fixture->templates) == 0);
    shape.example = request(fixture, 1).rule;
    assert(dppd_flow_template_create(fixture->templates, &shape, &fixture->table) == 0);
    fixture->config = (struct dppd_async_flow_config){.admission = fixture->admission,
        .templates = fixture->templates,
        .driver = {.context = &fixture->driver,
            .create = create,
            .destroy = destroy,
            .push = push,
            .pull = pull,
            .release = release},
        .flow_resource = DPPD_RESOURCE_SOFTWARE_RULE,
        .port_id = 7,
        .depth = depth,
        .flow_capacity = 64};
    assert(dppd_async_flow_queue_create(&fixture->config, &fixture->queue) == 0);
}

static void teardown(struct fixture *fixture)
{
    assert(dppd_async_flow_queue_close(&fixture->queue) == 0);
    assert(dppd_flow_template_destroy(fixture->templates, &fixture->table) == 0);
    assert(dppd_flow_templates_close(&fixture->templates) == 0);
    assert(dppd_admission_close(&fixture->admission) == 0);
}

static struct dppd_async_flow_status status(struct fixture *fixture)
{
    struct dppd_async_flow_status value;
    assert(dppd_async_flow_status(fixture->queue, &value) == 0);
    assert(value.queued + value.pending + value.completed <= value.depth);
    return value;
}

static void process(struct fixture *fixture)
{
    assert(dppd_async_flow_process(fixture->queue, 2, 1000, 64) == 0);
}

static void test_batch_and_backpressure(void)
{
    struct fixture fixture = {0};
    struct dppd_async_flow_create_request requests[4];
    struct dppd_async_flow_id flows[4], untouched = {.owner = 99};
    struct dppd_async_flow_result results[4];
    struct dppd_flow_template_info info;
    struct dppd_admission_snapshot usage;
    enum dppd_async_flow_state state;
    unsigned int i;
    setup(&fixture, 4);
    struct dppd_async_flow_queue *duplicate = NULL;
    assert(dppd_async_flow_queue_create(&fixture.config, &duplicate) == -EADDRINUSE && duplicate == NULL);
    for (i = 0; i < 4; ++i)
        requests[i] = request(&fixture, i + 1);
    requests[1].rule.group = 3;
    assert(dppd_async_flow_submit(fixture.queue, requests, 2, 1, &untouched) == -EINVAL && untouched.owner == 99);
    assert(status(&fixture).flows == 0 && fixture.driver.creates == 0);
    assert(dppd_flow_template_get(fixture.templates, &fixture.table, &info) == 0 && info.references == 0);
    assert(dppd_admission_snapshot(fixture.admission, &usage) == 0 && usage.held_tickets == 2);
    requests[1] = request(&fixture, 2);
    assert(dppd_async_flow_submit(fixture.queue, requests, 4, 1, flows) == 0 && fixture.driver.creates == 0);
    assert(dppd_async_flow_destroy(fixture.queue, &flows[0], 1) == -EBUSY);
    assert(dppd_async_flow_submit(fixture.queue, requests, 1, 1, &untouched) == -EAGAIN && untouched.owner == 99);
    assert(dppd_flow_template_destroy(fixture.templates, &fixture.table) == -EBUSY);
    process(&fixture);
    assert(status(&fixture).completed == 4 && fixture.driver.creates == 4);
    assert(dppd_async_flow_submit(fixture.queue, requests, 1, 1, &untouched) == -EAGAIN);
    assert(dppd_async_flow_queue_close(&fixture.queue) == -EBUSY);
    assert(dppd_async_flow_collect(fixture.queue, results, 4) == 4);
    for (i = 0; i < 4; ++i) {
        assert(results[i].status == 0 && results[i].object_exists && !results[i].destroy);
        assert(results[i].rule_id == results[i].flow.slot + 1U);
        assert(dppd_async_flow_get(fixture.queue, &flows[i], &state) == 0 && state == DPPD_ASYNC_FLOW_READY);
        assert(dppd_async_flow_destroy(fixture.queue, &flows[i], 1) == 0);
    }
    process(&fixture);
    assert(dppd_async_flow_collect(fixture.queue, results, 4) == 4);
    assert(status(&fixture).flows == 0 && fixture.driver.releases == 4);
    for (i = 0; i < 4; ++i)
        assert(results[i].destroy && !results[i].object_exists && results[i].status == 0);
    assert(dppd_async_flow_submit(fixture.queue, requests, 1, 1, &untouched) == 0);
    assert(dppd_async_flow_destroy(fixture.queue, &flows[0], 1) == -ESTALE);
    assert(dppd_async_flow_cancel(fixture.queue, &untouched) == 0);
    assert(dppd_async_flow_collect(fixture.queue, results, 1) == 1 && results[0].status == -ECANCELED);
    teardown(&fixture);
}

static void test_failed_create_and_destroy(void)
{
    struct fixture fixture = {0};
    struct dppd_async_flow_id flows[3];
    struct dppd_async_flow_create_request requests[3];
    struct dppd_async_flow_result results[3];
    struct dppd_admission_snapshot usage;
    unsigned int i;
    setup(&fixture, 4);
    fixture.driver.create_error_at = 2;
    fixture.driver.create_fail_at = 3;
    for (i = 0; i < 3; ++i)
        requests[i] = request(&fixture, i + 1);
    assert(dppd_async_flow_submit(fixture.queue, requests, 3, 1, flows) == 0);
    process(&fixture);
    assert(dppd_async_flow_collect(fixture.queue, results, 3) == 3);
    assert(results[0].status == 0 && results[0].object_exists);
    assert(results[1].status == -ENOSPC && !results[1].object_exists);
    assert(results[2].status == -EIO && results[2].object_exists);
    assert(dppd_admission_snapshot(fixture.admission, &usage) == 0);
    assert(usage.global.committed[DPPD_RESOURCE_SOFTWARE_RULE] == 2 && fixture.driver.releases == 0);
    assert(dppd_async_flow_destroy(fixture.queue, &flows[2], 1) == 0);
    fixture.driver.destroy_failure = -EIO;
    process(&fixture);
    assert(dppd_async_flow_collect(fixture.queue, results, 1) == 1 && results[0].object_exists);
    assert(status(&fixture).flows == 2 && fixture.driver.releases == 0);
    fixture.driver.destroy_failure = 0;
    fixture.driver.destroy_error = -ENOSPC;
    assert(dppd_async_flow_destroy(fixture.queue, &flows[2], 1) == 0);
    process(&fixture);
    assert(dppd_async_flow_collect(fixture.queue, results, 1) == 1 && results[0].status == -ENOSPC);
    fixture.driver.destroy_error = 0;
    assert(dppd_async_flow_destroy(fixture.queue, &flows[2], 1) == 0);
    assert(dppd_async_flow_destroy(fixture.queue, &flows[0], 1) == 0);
    process(&fixture);
    assert(dppd_async_flow_collect(fixture.queue, results, 3) == 2);
    assert(status(&fixture).flows == 0 && fixture.driver.releases == 2);
    teardown(&fixture);
}

static void test_timeout_and_transport_recovery(void)
{
    struct fixture fixture = {0};
    struct dppd_async_flow_id flow;
    struct dppd_async_flow_result result;
    struct dppd_async_flow_create_request req;
    setup(&fixture, 4);
    req = request(&fixture, 1);
    assert(dppd_async_flow_submit(fixture.queue, &req, 1, 1, &flow) == 0);
    fixture.driver.withhold = true;
    fixture.driver.push_error = -EIO;
    assert(dppd_async_flow_process(fixture.queue, 10, 20, 64) == -EUCLEAN);
    assert(status(&fixture).pending == 1 && status(&fixture).flows == 1);
    assert(dppd_async_flow_cancel(fixture.queue, &flow) == -EBUSY);
    assert(dppd_async_flow_destroy(fixture.queue, &flow, 10) == -EBUSY);
    assert(dppd_async_flow_process(fixture.queue, 30, 20, 64) == -EUCLEAN);
    assert(status(&fixture).timeouts == 1);
    assert(dppd_async_flow_process(fixture.queue, 40, 20, 64) == -EUCLEAN && status(&fixture).timeouts == 1);
    assert(dppd_async_flow_collect(fixture.queue, &result, 1) == 0 && fixture.driver.releases == 0);
    assert(dppd_async_flow_resume(fixture.queue) == -EUCLEAN);
    fixture.driver.push_error = 0;
    fixture.driver.withhold = false;
    fixture.driver.pull_error = -EAGAIN;
    assert(dppd_async_flow_process(fixture.queue, 40, 20, 64) == -EUCLEAN && status(&fixture).pending == 1);
    fixture.driver.pull_error = 0;
    assert(dppd_async_flow_process(fixture.queue, 40, 20, 64) == -EUCLEAN);
    assert(dppd_async_flow_collect(fixture.queue, &result, 1) == 1 && result.object_exists);
    assert(dppd_async_flow_destroy(fixture.queue, &flow, 40) == 0);
    assert(dppd_async_flow_process(fixture.queue, 40, 20, 64) == -EUCLEAN);
    assert(dppd_async_flow_collect(fixture.queue, &result, 1) == 1 && !result.object_exists);
    assert(dppd_async_flow_resume(fixture.queue) == -EUCLEAN);
    assert(dppd_async_flow_process(fixture.queue, 40, 20, 64) == -EUCLEAN);
    assert(dppd_async_flow_resume(fixture.queue) == 0 && !status(&fixture).fault);
    teardown(&fixture);
}

struct concurrent_submit {
    struct fixture *fixture;
    pthread_barrier_t *barrier;
    uint64_t id;
    struct dppd_async_flow_id flow;
    int result;
};

static void *submit_thread(void *context)
{
    struct concurrent_submit *job = context;
    struct dppd_async_flow_create_request req = request(job->fixture, job->id);
    pthread_barrier_wait(job->barrier);
    job->result = dppd_async_flow_submit(job->fixture->queue, &req, 1, 1, &job->flow);
    return NULL;
}

static void test_concurrent_producers_and_init_failures(void)
{
    struct fixture fixture = {0};
    struct concurrent_submit jobs[8];
    struct dppd_async_flow_result results[4];
    pthread_t threads[8];
    pthread_barrier_t barrier;
    unsigned int accepted = 0, rejected = 0;
    setup(&fixture, 4);
    struct dppd_async_flow_config config = fixture.config;
    config.queue_id = 1;
    for (unsigned int i = 1; i <= 3; ++i) {
        struct dppd_async_flow_queue *queue = NULL;
        fail_allocation = i;
        assert(dppd_async_flow_queue_create(&config, &queue) == -ENOMEM && queue == NULL);
    }
    assert(pthread_barrier_init(&barrier, NULL, 8) == 0);
    for (unsigned int i = 0; i < 8; ++i) {
        jobs[i] = (struct concurrent_submit){.fixture = &fixture, .barrier = &barrier, .id = i + 1};
        assert(pthread_create(&threads[i], NULL, submit_thread, &jobs[i]) == 0);
    }
    for (unsigned int i = 0; i < 8; ++i) {
        assert(pthread_join(threads[i], NULL) == 0);
        accepted += jobs[i].result == 0;
        rejected += jobs[i].result == -EAGAIN;
    }
    assert(accepted == 4 && rejected == 4 && fixture.driver.creates == 0);
    process(&fixture);
    assert(dppd_async_flow_collect(fixture.queue, results, 4) == 4);
    for (unsigned int i = 0; i < 8; ++i)
        if (jobs[i].result == 0)
            assert(dppd_async_flow_destroy(fixture.queue, &jobs[i].flow, 1) == 0);
    process(&fixture);
    assert(dppd_async_flow_collect(fixture.queue, results, 4) == 4);
    pthread_barrier_destroy(&barrier);
    teardown(&fixture);
}

/** 不明完成属于外部实际状态，子进程退出前保留资源，不用伪清理让测试通过 */
static void fatal_completion_child(bool duplicate)
{
    struct fixture fixture = {0};
    struct dppd_async_flow_result result;
    struct dppd_async_flow_id flow;
    setup(&fixture, 4);
    struct dppd_async_flow_create_request req = request(&fixture, 1);
    assert(dppd_async_flow_submit(fixture.queue, &req, 1, 1, &flow) == 0);
    if (duplicate) {
        process(&fixture);
        assert(dppd_async_flow_collect(fixture.queue, &result, 1) == 1);
        fixture.driver.duplicate = true;
    } else {
        fixture.driver.foreign = true;
    }
    assert(dppd_async_flow_process(fixture.queue, 2, 1000, 64) == -EUCLEAN);
    assert(status(&fixture).fatal && status(&fixture).flows == 1 && fixture.driver.releases == 0);
    assert(dppd_async_flow_queue_close(&fixture.queue) == -EBUSY);
    assert(dppd_async_flow_resume(fixture.queue) == -EUCLEAN);
    _exit(0);
}

int main(void)
{
    test_batch_and_backpressure();
    test_failed_create_and_destroy();
    test_timeout_and_transport_recovery();
    test_concurrent_producers_and_init_failures();
    for (unsigned int i = 0; i < 2; ++i) {
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0)
            fatal_completion_child(i != 0);
        int result;
        assert(waitpid(child, &result, 0) == child && WIFEXITED(result) && WEXITSTATUS(result) == 0);
    }
    return 0;
}
