#define _GNU_SOURCE
#include "flow_state_io.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/** 本实例独占配置、classifier 和异步服务，文件只保存期望状态，关闭仅回收实际对象 */
struct dppd_flow_store {
    pthread_mutex_t lock;
    char *path;
    int guard;
    struct dppd_flow_state *state;
    struct dppd_flow_service_entry *scratch;
    struct dppd_admission *admission;
    struct dppd_flow_templates *templates;
    struct dppd_flow_shared_counts *counts;
    struct dppd_flow_template_id tables[64];
    bool shared_created[64];
    struct dppd_async_flow_queue *queues[8];
    struct dppd_software_backend software;
    struct dppd_software_async_flows *software_flows;
    struct dppd_flow_service *service;
    struct dppd_flow_store_status status;
    struct dppd_flow_job_id internal_job;
    bool pending;
};

static uint64_t now_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static bool same_table(const struct dppd_flow_template_id *a, const struct dppd_flow_template_id *b)
{
    return a->owner == b->owner && a->serial == b->serial && a->slot == b->slot;
}

static int table_slot(struct dppd_flow_store *s, uint64_t key)
{
    for (uint32_t i = 0; i < s->state->table_count; ++i)
        if (s->state->tables[i].key == key)
            return (int)i;
    return -1;
}

static int available(struct dppd_flow_store *s)
{
    return s->status.closing ? -ESHUTDOWN : !s->status.ready || s->status.dirty || s->status.residual ? -EUCLEAN : 0;
}

/** 创建和删除都要收齐实际完成，超时只保留任务，不能据此推断驱动没有留下对象 */
static int wait_internal(struct dppd_flow_store *s, bool retry, struct dppd_flow_job_result *result)
{
    uint64_t start = now_ns();
    bool retried = false;
    if (start == 0)
        return -EIO;
    for (;;) {
        int rc = dppd_flow_service_collect(s->service, &s->internal_job, result);
        if (rc == 0) {
            s->pending = false;
            return 0;
        }
        if (rc == -EUCLEAN && retry && !retried) {
            retried = true;
            if (dppd_flow_service_retry(s->service) != 0)
                return -EUCLEAN;
        } else if (rc != -EAGAIN) {
            return rc;
        }
        uint64_t now = now_ns();
        if (now == 0 || now < start)
            return -EIO;
        if (now - start >= s->state->config.timeout_ns)
            return -ETIMEDOUT;
        dppd_flow_service_process(s->service, now, 64);
        /** 给真实驱动和调度器留出运行机会，避免恢复循环一直占满控制 CPU */
        struct timespec pause = {.tv_nsec = 100000};
        nanosleep(&pause, NULL);
    }
}

static int snapshot(struct dppd_flow_store *s)
{
    uint32_t count;
    uint64_t revision;
    int rc = dppd_flow_service_snapshot(s->service, s->scratch, s->state->config.rule_capacity, &count, &revision);
    if (rc != 0)
        return rc;
    for (uint32_t i = 0; i < count; ++i) {
        int table = -1;
        for (uint32_t t = 0; t < s->state->table_count; ++t)
            if (same_table(&s->tables[t], &s->scratch[i].request.table))
                table = (int)t;
        if (table < 0)
            return -EUCLEAN;
        s->state->rules[i] =
            (struct dppd_flow_state_rule){s->state->tables[table].key, s->scratch[i].queue, s->scratch[i].request.rule};
    }
    s->state->rule_count = count;
    s->state->revision = revision;
    return 0;
}

static int flush(struct dppd_flow_store *s)
{
    struct dppd_flow_service_status status;
    dppd_flow_service_status(s->service, &status);
    if (status.isolated) {
        /** 修订号已经变化却仍处于隔离，不能把不确定的配置覆盖到最后一份可靠快照 */
        if (status.revision != s->status.durable_revision) {
            s->status.residual = true;
            s->status.last_error = -EUCLEAN;
        }
        return -EUCLEAN;
    }
    int rc = snapshot(s);
    if (rc == 0)
        rc = dppd_flow_state_write(s->path, s->guard, s->state);
    s->status.dirty = rc != 0;
    s->status.last_error = rc;
    if (rc == 0)
        s->status.durable_revision = s->state->revision;
    return rc;
}

/** 逆序回收服务、队列、模板和共享对象，任何失败都保留实例和写锁供下一次关闭重试 */
static int teardown(struct dppd_flow_store *s)
{
    struct dppd_flow_job_result result;
    int rc;
    s->status.ready = false;
    s->status.closing = true;
    if (s->service != NULL) {
        if (s->pending && wait_internal(s, true, &result) != 0)
            return -EUCLEAN;
        if (dppd_flow_service_retry(s->service) != 0 || dppd_flow_service_shutdown_begin(s->service) != 0)
            return -EUCLEAN;
        for (;;) {
            uint32_t count;
            uint64_t revision;
            rc = dppd_flow_service_snapshot(s->service, s->scratch, s->state->config.rule_capacity, &count, &revision);
            if (rc != 0)
                return rc;
            if (count == 0)
                break;
            struct dppd_flow_remove_request requests[64];
            if (count > 64)
                count = 64;
            if (count > s->state->config.depth)
                count = s->state->config.depth;
            for (uint32_t i = 0; i < count; ++i)
                requests[i] = (struct dppd_flow_remove_request){s->scratch[i].request.rule.id,
                    s->scratch[i].request.rule.generation};
            rc = dppd_flow_service_remove(s->service, 0, requests, count, now_ns(), &s->internal_job);
            if (rc != 0)
                return rc;
            s->pending = true;
            rc = wait_internal(s, true, &result);
            if (rc != 0 || !result.applied)
                return rc != 0 ? rc : result.status;
        }
        if (dppd_flow_service_close(&s->service) != 0)
            return -EUCLEAN;
    }
    for (uint32_t q = s->state->config.queue_count; q != 0; --q)
        if (s->queues[q - 1U] != NULL && dppd_async_flow_queue_close(&s->queues[q - 1U]) != 0)
            return -EUCLEAN;
    if (s->software_flows != NULL && dppd_software_async_flows_close(&s->software_flows) != 0)
        return -EUCLEAN;
    if (s->templates != NULL) {
        for (uint32_t t = s->state->table_count; t != 0; --t)
            if (s->tables[t - 1U].owner != 0) {
                if (dppd_flow_template_destroy(s->templates, &s->tables[t - 1U]) != 0)
                    return -EUCLEAN;
                s->tables[t - 1U] = (struct dppd_flow_template_id){0};
            }
        if (dppd_flow_templates_close(&s->templates) != 0)
            return -EUCLEAN;
    }
    if (s->counts != NULL) {
        for (uint32_t i = 0; i < s->state->shared_count; ++i)
            if (s->shared_created[i]) {
                if (dppd_flow_shared_counts_remove(s->counts, s->state->counts[i].id, s->state->counts[i].generation) !=
                    0)
                    return -EUCLEAN;
                s->shared_created[i] = false;
            }
        if (dppd_flow_shared_counts_close(&s->counts) != 0)
            return -EUCLEAN;
    }
    if (s->admission != NULL && dppd_admission_close(&s->admission) != 0)
        return -EUCLEAN;
    if (s->software.lock_initialized)
        dppd_software_backend_fini(&s->software);
    return 0;
}

static void destroy(struct dppd_flow_store *s)
{
    close(s->guard);
    pthread_mutex_destroy(&s->lock);
    free(s->scratch);
    free(s->path);
    dppd_flow_state_free(&s->state);
    free(s);
}

static int build(struct dppd_flow_store *s, const struct dppd_flow_store_runtime *runtime)
{
    const struct dppd_flow_state_config *c = &s->state->config;
    struct dppd_async_flow_driver flow_driver;
    struct dppd_flow_template_driver template_driver;
    int rc;
    if (c->mode == DPPD_FLOW_STATE_NATIVE) {
        if (runtime == NULL || runtime->verify_empty == NULL)
            return -ENOTSUP;
        rc = runtime->verify_empty(runtime->context, c->port_id);
        if (rc != 0)
            return rc;
        rc = runtime->configure != NULL ? runtime->configure(runtime->context, c)
                                        : dppd_native_flow_configure(c->port_id, (uint16_t)c->queue_count, c->depth,
                                              c->native_counters, c->native_aging);
        if (rc != 0)
            return rc;
    }
    rc = dppd_admission_create(&c->admission, &s->admission);
    if (rc != 0)
        return rc;
    if (c->mode == DPPD_FLOW_STATE_NATIVE) {
        struct dppd_flow_shared_count_driver driver =
            runtime->counts.create != NULL ? runtime->counts : dppd_native_flow_shared_count_driver();
        rc = dppd_flow_shared_counts_create(s->admission, c->shared_leases, &driver, &s->counts);
        if (rc != 0)
            return rc;
        /** 文件按 ID 排序，创建按旧代次排序，才能保留中间删除留下的版本空洞 */
        uint64_t previous = 0;
        for (uint32_t n = 0; n < s->state->shared_count; ++n) {
            uint32_t next = UINT32_MAX;
            for (uint32_t i = 0; i < s->state->shared_count; ++i)
                if (s->state->counts[i].generation > previous &&
                    (next == UINT32_MAX || s->state->counts[i].generation < s->state->counts[next].generation))
                    next = i;
            if (next == UINT32_MAX)
                return -EUCLEAN;
            struct dppd_flow_shared_count_info info = {0};
            rc = dppd_flow_shared_counts_replay_add(s->counts, &s->state->counts[next], &info);
            if (info.spec.id != 0)
                s->shared_created[next] = true;
            if (rc != 0)
                return rc;
            previous = info.spec.generation;
        }
        rc = dppd_flow_shared_counts_seed(s->counts, s->state->shared_next_generation);
        if (rc != 0)
            return rc;
        template_driver = runtime->templates != NULL ? runtime->templates(runtime->context, s->counts)
                                                     : dppd_native_flow_template_driver_shared(s->counts);
        flow_driver = runtime->flows.create != NULL ? runtime->flows : dppd_native_async_flow_driver();
    } else {
        if (runtime != NULL && runtime->flows.create != NULL)
            flow_driver = runtime->flows;
        else {
            rc = dppd_software_async_flows_create(&s->software_flows, &flow_driver);
            if (rc != 0)
                return rc;
        }
        rc = dppd_software_backend_init(&s->software, c->rule_capacity);
        if (rc != 0)
            return rc;
    }
    rc = dppd_flow_templates_create(s->admission, c->template_leases,
        c->mode == DPPD_FLOW_STATE_NATIVE ? &template_driver : NULL, &s->templates);
    if (rc != 0)
        return rc;
    for (uint32_t t = 0; t < s->state->table_count; ++t) {
        rc = dppd_flow_template_create(s->templates, &s->state->tables[t].spec, &s->tables[t]);
        if (rc != 0)
            return rc;
    }
    struct dppd_flow_service_config service = {.admission = s->admission,
        .templates = s->templates,
        .software = c->mode == DPPD_FLOW_STATE_SOFTWARE ? &s->software : NULL,
        .rule_capacity = c->rule_capacity,
        .queue_count = c->queue_count,
        .depth = c->depth,
        .timeout_ns = c->timeout_ns,
        .port_id = c->port_id};
    for (uint32_t q = 0; q < c->queue_count; ++q) {
        struct dppd_async_flow_config queue = {.admission = s->admission,
            .templates = s->templates,
            .driver = flow_driver,
            .flow_resource =
                c->mode == DPPD_FLOW_STATE_SOFTWARE ? DPPD_RESOURCE_SOFTWARE_RULE : DPPD_RESOURCE_HARDWARE_FLOW,
            .port_id = c->port_id,
            .queue_id = (uint16_t)q,
            .depth = c->depth,
            .flow_capacity = c->queue_capacity[q]};
        rc = dppd_async_flow_queue_create(&queue, &s->queues[q]);
        if (rc != 0)
            return rc;
        service.queues[q] = s->queues[q];
    }
    rc = dppd_flow_service_create(&service, &s->service);
    if (rc == 0)
        rc = dppd_flow_service_replay_begin(s->service, s->state->revision);
    if (rc != 0)
        return rc;
    for (uint32_t q = 0; q < c->queue_count; ++q) {
        uint32_t offset = 0;
        while (offset < s->state->rule_count) {
            struct dppd_async_flow_create_request requests[64];
            uint32_t count = 0;
            for (; offset < s->state->rule_count && count < 64 && count < c->depth; ++offset) {
                const struct dppd_flow_state_rule *entry = &s->state->rules[offset];
                if (entry->queue != q)
                    continue;
                int t = table_slot(s, entry->table_key);
                requests[count++] = (struct dppd_async_flow_create_request){s->tables[t], entry->rule};
            }
            if (count == 0)
                continue;
            rc = dppd_flow_service_replay_submit(s->service, q, requests, count, now_ns(), &s->internal_job);
            if (rc != 0)
                return rc;
            s->pending = true;
            struct dppd_flow_job_result result;
            rc = wait_internal(s, false, &result);
            if (rc != 0 || !result.applied)
                return rc != 0 ? rc : result.status;
        }
    }
    return dppd_flow_service_replay_end(s->service);
}

int dppd_flow_store_open(const char *path, const struct dppd_flow_store_runtime *runtime,
    struct dppd_flow_store **output)
{
    if (path == NULL || output == NULL || *output != NULL)
        return -EINVAL;
    char *canonical = NULL;
    int rc = dppd_flow_state_path(path, &canonical);
    if (rc != 0)
        return rc;
    int guard = dppd_flow_state_lock(canonical);
    if (guard < 0) {
        free(canonical);
        return guard;
    }
    struct dppd_flow_state *state = NULL;
    rc = dppd_flow_state_load(canonical, &state);
    if (rc != 0) {
        close(guard);
        free(canonical);
        return rc;
    }
    struct dppd_flow_store *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        close(guard);
        free(canonical);
        dppd_flow_state_free(&state);
        return -ENOMEM;
    }
    s->guard = guard;
    s->state = state;
    s->path = canonical;
    s->scratch = calloc(state->config.rule_capacity, sizeof(*s->scratch));
    struct dppd_flow_state_rule *rules = calloc(state->config.rule_capacity, sizeof(*rules));
    if (s->path == NULL || s->scratch == NULL || rules == NULL) {
        free(rules);
        free(s->path);
        free(s->scratch);
        free(s);
        close(guard);
        dppd_flow_state_free(&state);
        return -ENOMEM;
    }
    if (state->rule_count != 0)
        memcpy(rules, state->rules, state->rule_count * sizeof(*rules));
    free(state->rules);
    state->rules = rules;
    rc = pthread_mutex_init(&s->lock, NULL);
    if (rc != 0) {
        free(s->path);
        free(s->scratch);
        free(s);
        close(guard);
        dppd_flow_state_free(&state);
        return -rc;
    }
    rc = build(s, runtime);
    if (rc != 0) {
        int cleanup = teardown(s);
        if (cleanup == 0) {
            destroy(s);
            return rc;
        }
        s->status.residual = true;
        s->status.last_error = rc;
        *output = s;
        return -EUCLEAN;
    }
    s->status.ready = true;
    s->status.durable_revision = state->revision;
    *output = s;
    return 0;
}

int dppd_flow_store_close(struct dppd_flow_store **output)
{
    if (output == NULL || *output == NULL)
        return -EINVAL;
    struct dppd_flow_store *s = *output;
    pthread_mutex_lock(&s->lock);
    struct dppd_flow_service_status status;
    if (s->status.dirty) {
        pthread_mutex_unlock(&s->lock);
        return -EUCLEAN;
    }
    if (s->status.ready && s->service != NULL && dppd_flow_service_status(s->service, &status) == 0 &&
        status.jobs != 0) {
        pthread_mutex_unlock(&s->lock);
        return -EBUSY;
    }
    int rc = teardown(s);
    if (rc != 0) {
        s->status.residual = true;
        s->status.last_error = rc;
    }
    pthread_mutex_unlock(&s->lock);
    if (rc == 0) {
        destroy(s);
        *output = NULL;
    }
    return rc;
}

int dppd_flow_store_status(struct dppd_flow_store *s, struct dppd_flow_store_status *status)
{
    if (s == NULL || status == NULL)
        return -EINVAL;
    pthread_mutex_lock(&s->lock);
    *status = s->status;
    status->ready &= !status->dirty && !status->residual && !status->closing;
    if (s->service != NULL) {
        dppd_flow_service_status(s->service, &status->service);
        status->ready &= !status->service.isolated;
    }
    pthread_mutex_unlock(&s->lock);
    return 0;
}

struct dppd_software_backend *dppd_flow_store_software(struct dppd_flow_store *s)
{
    if (s == NULL)
        return NULL;
    pthread_mutex_lock(&s->lock);
    struct dppd_software_backend *result = available(s) == 0 && s->software.lock_initialized ? &s->software : NULL;
    if (s->service != NULL) {
        struct dppd_flow_service_status status;
        dppd_flow_service_status(s->service, &status);
        if (status.isolated)
            result = NULL;
    }
    pthread_mutex_unlock(&s->lock);
    return result;
}

int dppd_flow_store_get(struct dppd_flow_store *s, uint64_t id, struct dppd_rule *rule)
{
    if (s == NULL || rule == NULL)
        return -EINVAL;
    pthread_mutex_lock(&s->lock);
    int rc = !s->status.ready || s->service == NULL || s->status.closing || s->status.residual
        ? -EUCLEAN
        : dppd_flow_service_get(s->service, id, rule);
    pthread_mutex_unlock(&s->lock);
    return rc;
}

static int resolve(struct dppd_flow_store *s, uint32_t queue, const struct dppd_flow_state_rule *rules, uint32_t count,
    struct dppd_async_flow_create_request *requests)
{
    if (rules == NULL || count == 0 || count > 64)
        return -EINVAL;
    for (uint32_t i = 0; i < count; ++i) {
        int t = table_slot(s, rules[i].table_key);
        if (t < 0 || rules[i].queue != queue)
            return -EINVAL;
        requests[i] = (struct dppd_async_flow_create_request){s->tables[t], rules[i].rule};
    }
    return 0;
}

int dppd_flow_store_submit(struct dppd_flow_store *s, uint32_t queue, const struct dppd_flow_state_rule *rules,
    uint32_t count, uint64_t now, struct dppd_flow_job_id *job)
{
    struct dppd_async_flow_create_request requests[64];
    if (s == NULL || job == NULL)
        return -EINVAL;
    pthread_mutex_lock(&s->lock);
    int rc = available(s);
    if (rc == 0)
        rc = resolve(s, queue, rules, count, requests);
    if (rc == 0)
        rc = dppd_flow_service_submit(s->service, queue, requests, count, now, job);
    pthread_mutex_unlock(&s->lock);
    return rc;
}

int dppd_flow_store_update(struct dppd_flow_store *s, uint32_t queue, const struct dppd_flow_state_rule *rules,
    const uint64_t *expected, uint32_t count, uint64_t now, struct dppd_flow_job_id *job)
{
    struct dppd_async_flow_create_request requests[64];
    struct dppd_flow_update_request updates[64];
    if (s == NULL || job == NULL || expected == NULL)
        return -EINVAL;
    pthread_mutex_lock(&s->lock);
    int rc = available(s);
    if (rc == 0)
        rc = resolve(s, queue, rules, count, requests);
    if (rc == 0) {
        for (uint32_t i = 0; i < count; ++i)
            updates[i] = (struct dppd_flow_update_request){expected[i], requests[i]};
        rc = dppd_flow_service_update(s->service, queue, updates, count, now, job);
    }
    pthread_mutex_unlock(&s->lock);
    return rc;
}

int dppd_flow_store_remove(struct dppd_flow_store *s, uint32_t queue, const struct dppd_flow_remove_request *rules,
    uint32_t count, uint64_t now, struct dppd_flow_job_id *job)
{
    if (s == NULL || job == NULL)
        return -EINVAL;
    pthread_mutex_lock(&s->lock);
    int rc = available(s);
    if (rc == 0)
        rc = dppd_flow_service_remove(s->service, queue, rules, count, now, job);
    pthread_mutex_unlock(&s->lock);
    return rc;
}

int dppd_flow_store_process(struct dppd_flow_store *s, uint64_t now, uint32_t budget)
{
    if (s == NULL)
        return -EINVAL;
    pthread_mutex_lock(&s->lock);
    int rc = available(s);
    if (rc == 0) {
        rc = dppd_flow_service_process(s->service, now, budget);
        struct dppd_flow_service_status status;
        dppd_flow_service_status(s->service, &status);
        if (status.revision != s->status.durable_revision) {
            int saved = flush(s);
            if (saved != 0)
                rc = saved;
        }
    }
    pthread_mutex_unlock(&s->lock);
    return rc;
}

int dppd_flow_store_collect(struct dppd_flow_store *s, const struct dppd_flow_job_id *job,
    struct dppd_flow_job_result *result)
{
    if (s == NULL)
        return -EINVAL;
    pthread_mutex_lock(&s->lock);
    int rc = available(s);
    if (rc == 0)
        rc = dppd_flow_service_collect(s->service, job, result);
    pthread_mutex_unlock(&s->lock);
    return rc;
}

int dppd_flow_store_cancel(struct dppd_flow_store *s, const struct dppd_flow_job_id *job)
{
    if (s == NULL)
        return -EINVAL;
    pthread_mutex_lock(&s->lock);
    int rc = available(s);
    if (rc == 0)
        rc = dppd_flow_service_cancel(s->service, job);
    pthread_mutex_unlock(&s->lock);
    return rc;
}

int dppd_flow_store_retry(struct dppd_flow_store *s)
{
    if (s == NULL)
        return -EINVAL;
    pthread_mutex_lock(&s->lock);
    int rc = available(s);
    if (rc == 0)
        rc = dppd_flow_service_retry(s->service);
    pthread_mutex_unlock(&s->lock);
    return rc;
}

int dppd_flow_store_flush(struct dppd_flow_store *s)
{
    if (s == NULL)
        return -EINVAL;
    pthread_mutex_lock(&s->lock);
    int rc = !s->status.ready || s->status.closing || s->status.residual ? -EUCLEAN : flush(s);
    pthread_mutex_unlock(&s->lock);
    return rc;
}

int dppd_flow_store_query_shared(struct dppd_flow_store *s, uint64_t id, uint64_t generation, uint64_t *hits,
    uint64_t *bytes)
{
    if (s == NULL)
        return -EINVAL;
    pthread_mutex_lock(&s->lock);
    int rc = available(s);
    if (rc == 0)
        rc = s->counts == NULL ? -ENOTSUP : dppd_flow_shared_counts_query(s->counts, id, generation, hits, bytes);
    pthread_mutex_unlock(&s->lock);
    return rc;
}
