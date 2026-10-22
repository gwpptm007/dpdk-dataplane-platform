#define _GNU_SOURCE
#include "flow_state_io.h"
#include "flow_request_io.h"
#include "dppd/software_shared.h"

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
    /** 准备失败的新对象或提交后待删除的旧对象，都保留精确凭据供重试 */
    struct dppd_flow_template_id retired_tables[64];
    struct dppd_shared_count_spec retired_counts[64];
    bool configuration_residual;
    struct dppd_flow_job_id request_jobs[DPPD_REQUEST_RECEIPTS];
    bool requests_dirty;
};

/** 在保存配置前收齐持久任务的完整结果，避免策略已经落盘而回执仍停留在受理态 */
static bool collect_requests(struct dppd_flow_store *s)
{
    if (s->state->requests == NULL)
        return false;
    for (uint32_t i = 0; i < DPPD_REQUEST_RECEIPTS; ++i) {
        if (s->request_jobs[i].owner == 0)
            continue;
        struct dppd_flow_job_result result;
        if (dppd_flow_service_collect(s->service, &s->request_jobs[i], &result) != 0)
            continue;
        struct dppd_flow_request_result *receipt = &s->state->requests->receipts[i].result;
        receipt->phase = DPPD_FLOW_REQUEST_DONE;
        receipt->status = result.status;
        receipt->applied = result.applied;
        receipt->revision = result.revision;
        for (uint32_t j = 0; j < result.count && j < 8; ++j) {
            receipt->ids[j] = result.ids[j];
            receipt->generations[j] = result.generations[j];
        }
        memset(&s->request_jobs[i], 0, sizeof(s->request_jobs[i]));
        s->requests_dirty = true;
    }
    return s->requests_dirty;
}

static int cleanup_configuration(struct dppd_flow_store *s)
{
    bool remaining = false;
    if (s->software.lock_initialized)
        dppd_software_backend_reclaim(&s->software);
    for (uint32_t i = 0; i < 64; ++i)
        if (s->retired_tables[i].owner != 0) {
            if (dppd_flow_template_destroy(s->templates, &s->retired_tables[i]) == 0)
                memset(&s->retired_tables[i], 0, sizeof(s->retired_tables[i]));
            else
                remaining = true;
        }
    /** 模板可能持有共享对象的借用，必须等全部待删除模板清除后再尝试共享回收 */
    if (!remaining)
        for (uint32_t i = 0; i < 64; ++i)
            if (s->retired_counts[i].id != 0) {
                if (dppd_flow_shared_counts_remove(s->counts, s->retired_counts[i].id,
                        s->retired_counts[i].generation) == 0)
                    memset(&s->retired_counts[i], 0, sizeof(s->retired_counts[i]));
                else
                    remaining = true;
            }
    if (remaining) {
        s->configuration_residual = true;
        s->status.residual = true;
        s->status.last_error = -EUCLEAN;
        return -EUCLEAN;
    }
    if (s->configuration_residual) {
        s->configuration_residual = false;
        s->status.residual = false;
        s->status.last_error = 0;
    }
    return 0;
}

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

/** 回执尚未保存时也封闭新写入，process 和显式修复仍可完成已经受理的工作 */
static int writable(struct dppd_flow_store *s)
{
    int rc = available(s);
    return rc != 0 ? rc : s->requests_dirty ? -EUCLEAN : 0;
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
    (void)collect_requests(s);
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
    if (rc == 0) {
        s->status.durable_revision = s->state->revision;
        s->requests_dirty = false;
    }
    if (rc == 0)
        rc = cleanup_configuration(s);
    return rc;
}

/** 逆序回收服务、队列、模板和共享对象，任何失败都保留实例和写锁供下一次关闭重试 */
static int teardown(struct dppd_flow_store *s)
{
    struct dppd_flow_job_result result;
    int rc;
    s->status.ready = false;
    s->status.closing = true;
    if (cleanup_configuration(s) != 0)
        return -EUCLEAN;
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
    /** 先释放退役快照的共享借用，再删除模板及共享库，避免旧读者引用被提前释放 */
    if (s->software.lock_initialized)
        dppd_software_backend_fini(&s->software);
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

static int software_bind(void *context, const struct dppd_rule *rule, struct dppd_flow_binding *binding,
    struct dppd_flow_shared_count_lease *lease)
{
    return dppd_flow_shared_counts_bind(context, rule, binding, lease);
}

static int software_unbind(void *context, const struct dppd_flow_shared_count_lease *lease)
{
    return lease->owner == 0 ? 0 : dppd_flow_shared_counts_unbind(context, lease);
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
    {
        struct dppd_flow_shared_count_driver driver = c->mode == DPPD_FLOW_STATE_SOFTWARE
            ? dppd_software_shared_count_driver()
            : runtime->counts.create != NULL ? runtime->counts
                                             : dppd_native_flow_shared_count_driver();
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
    }
    if (c->mode == DPPD_FLOW_STATE_NATIVE) {
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
        s->software.shared_counts = s->counts;
        template_driver =
            (struct dppd_flow_template_driver){.context = s->counts, .bind = software_bind, .unbind = software_unbind};
    }
    rc = dppd_flow_templates_create(s->admission, c->template_leases, &template_driver, &s->templates);
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
    if (rc == 0 && state->requests != NULL) {
        /** 文件中未完成的请求没有已提交策略结果，重启后明确中止，旧键不能重新执行 */
        bool changed = false;
        for (uint32_t i = 0; i < DPPD_REQUEST_RECEIPTS; ++i) {
            struct dppd_flow_request_result *receipt = &state->requests->receipts[i].result;
            if (receipt->key.client != 0 && receipt->phase == DPPD_FLOW_REQUEST_PENDING) {
                receipt->phase = DPPD_FLOW_REQUEST_DONE;
                receipt->status = -ECANCELED;
                receipt->revision = state->revision;
                changed = true;
            }
        }
        if (changed)
            rc = dppd_flow_state_write(s->path, s->guard, s->state);
    }
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
    if (s->status.dirty || s->requests_dirty) {
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
    status->dirty |= s->requests_dirty;
    status->ready &= !status->dirty && !status->residual && !status->closing;
    if (s->service != NULL) {
        dppd_flow_service_status(s->service, &status->service);
        status->ready &= !status->service.isolated;
    }
    pthread_mutex_unlock(&s->lock);
    return 0;
}

int dppd_flow_store_config(struct dppd_flow_store *s, struct dppd_flow_state_config *config)
{
    if (s == NULL || config == NULL)
        return -EINVAL;
    pthread_mutex_lock(&s->lock);
    *config = s->state->config;
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

int dppd_flow_store_list(struct dppd_flow_store *s, uint64_t after, uint64_t expected,
    struct dppd_flow_state_rule *rules, uint32_t capacity, uint32_t *count, uint64_t *revision, bool *more)
{
    if (s == NULL || rules == NULL || capacity == 0 || count == NULL || revision == NULL || more == NULL)
        return -EINVAL;
    pthread_mutex_lock(&s->lock);
    int rc = available(s);
    if (rc == 0 && expected != UINT64_MAX && expected != s->state->revision)
        rc = -ESTALE;
    if (rc == 0) {
        *count = 0;
        *more = false;
        *revision = s->state->revision;
        for (uint32_t i = 0; i < s->state->rule_count; ++i) {
            if (s->state->rules[i].rule.id <= after)
                continue;
            if (*count == capacity) {
                *more = true;
                break;
            }
            rules[(*count)++] = s->state->rules[i];
        }
    }
    pthread_mutex_unlock(&s->lock);
    return rc;
}

int dppd_flow_store_query_count(struct dppd_flow_store *s, uint64_t id, uint64_t generation, uint64_t *hits,
    uint64_t *bytes)
{
    if (s == NULL)
        return -EINVAL;
    pthread_mutex_lock(&s->lock);
    int rc = available(s);
    if (rc == 0)
        rc = s->software.lock_initialized ? dppd_software_backend_query_count(&s->software, id, generation, hits, bytes)
                                          : -ENOTSUP;
    pthread_mutex_unlock(&s->lock);
    return rc;
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
    int rc = writable(s);
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
    int rc = writable(s);
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
    int rc = writable(s);
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
        (void)collect_requests(s);
        struct dppd_flow_service_status status;
        dppd_flow_service_status(s->service, &status);
        if (status.revision != s->status.durable_revision || s->requests_dirty) {
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
    int rc = s->configuration_residual && !s->status.dirty ? cleanup_configuration(s) : available(s);
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

int dppd_flow_store_configuration(struct dppd_flow_store *s, uint64_t expected, uint64_t after_table,
    uint64_t after_count, struct dppd_flow_config_page *page)
{
    if (s == NULL || page == NULL)
        return -EINVAL;
    pthread_mutex_lock(&s->lock);
    int rc = expected != UINT64_MAX && expected != s->state->revision ? -ESTALE : 0;
    if (rc == 0) {
        memset(page, 0, sizeof(*page));
        page->revision = s->state->revision;
        page->durable_revision = s->status.durable_revision;
        page->shared_next_generation = s->state->shared_next_generation;
        page->dirty = s->status.dirty;
        page->residual = s->status.residual;
        page->table_count = s->state->table_count;
        page->shared_count = s->state->shared_count;
        page->next_table = after_table;
        page->next_count = after_count;
        for (uint32_t i = 0; i < s->state->table_count; ++i) {
            if (s->state->tables[i].key <= after_table)
                continue;
            if (page->tables_returned == 2) {
                page->more_tables = true;
                break;
            }
            page->tables[page->tables_returned++] = s->state->tables[i];
            page->next_table = s->state->tables[i].key;
        }
        for (uint32_t i = 0; i < s->state->shared_count; ++i) {
            if (s->state->counts[i].id <= after_count)
                continue;
            if (page->counts_returned == 4) {
                page->more_counts = true;
                break;
            }
            page->counts[page->counts_returned++] = s->state->counts[i];
            page->next_count = s->state->counts[i].id;
        }
    }
    pthread_mutex_unlock(&s->lock);
    return rc;
}

static int count_slot(const struct dppd_flow_state *state, uint64_t id)
{
    for (uint32_t i = 0; i < state->shared_count; ++i)
        if (state->counts[i].id == id)
            return (int)i;
    return -1;
}

static int compare_tables(const void *left, const void *right)
{
    const struct dppd_flow_state_table *a = left, *b = right;
    return (a->key > b->key) - (a->key < b->key);
}

static int compare_counts(const void *left, const void *right)
{
    const struct dppd_shared_count_spec *a = left, *b = right;
    return (a->id > b->id) - (a->id < b->id);
}

static bool uses_count(const struct dppd_rule *rule, uint64_t id)
{
    for (uint16_t a = 0; a < rule->nb_actions; ++a)
        if (rule->actions[a].type == DPPD_ACTION_SHARED_COUNT && rule->actions[a].conf.shared_count.id == id)
            return true;
    return false;
}

/** 候选配置只修改定义，规则不在本事务中迁移，重复键和被引用删除都在驱动前拒绝 */
static int prepare_configuration(struct dppd_flow_store *s, struct dppd_flow_state *next,
    const struct dppd_flow_config_change *changes, uint32_t count, uint64_t generations[8])
{
    for (uint32_t i = 0; i < count; ++i) {
        const struct dppd_flow_config_change *change = &changes[i];
        bool table =
            change->operation == DPPD_FLOW_CONFIG_TEMPLATE_ADD || change->operation == DPPD_FLOW_CONFIG_TEMPLATE_DELETE;
        if (change->operation < DPPD_FLOW_CONFIG_TEMPLATE_ADD || change->operation > DPPD_FLOW_CONFIG_COUNT_DELETE ||
            change->key == 0 || change->key == UINT64_MAX)
            return -EINVAL;
        for (uint32_t j = 0; j < i; ++j) {
            bool previous_table = changes[j].operation <= DPPD_FLOW_CONFIG_TEMPLATE_DELETE;
            if (changes[j].key == change->key && previous_table == table)
                return -EINVAL;
        }
        int slot = table ? table_slot(s, change->key) : count_slot(s->state, change->key);
        if (change->operation == DPPD_FLOW_CONFIG_TEMPLATE_ADD || change->operation == DPPD_FLOW_CONFIG_COUNT_ADD) {
            if (slot >= 0)
                return -EEXIST;
            if (change->expected_generation != 0)
                return -EINVAL;
            if (table) {
                if (next->table_count == 64)
                    return -ENOSPC;
                next->tables[next->table_count++] = (struct dppd_flow_state_table){change->key, change->spec.table};
            } else {
                if (next->shared_count == 64 || next->shared_next_generation == UINT64_MAX)
                    return -ENOSPC;
                generations[i] = next->shared_next_generation++;
                next->counts[next->shared_count++] = (struct dppd_shared_count_spec){change->key, generations[i],
                    change->spec.count.port_id, change->spec.count.domain};
            }
        } else {
            if (slot < 0)
                return -ENOENT;
            if (table) {
                if (change->expected_generation != 0)
                    return -EINVAL;
                for (uint32_t r = 0; r < s->state->rule_count; ++r)
                    if (s->state->rules[r].table_key == change->key)
                        return -EBUSY;
                uint32_t n = 0;
                while (next->tables[n].key != change->key)
                    ++n;
                memmove(next->tables + n, next->tables + n + 1, (--next->table_count - n) * sizeof(next->tables[0]));
            } else {
                if (change->expected_generation != s->state->counts[slot].generation)
                    return -ESTALE;
                int n = count_slot(next, change->key);
                memmove(next->counts + n, next->counts + n + 1,
                    (--next->shared_count - (uint32_t)n) * sizeof(next->counts[0]));
            }
        }
    }
    /** 同批新建的共享引用可填零代次，只有本批实际分配的身份才允许这样解析 */
    for (uint32_t t = 0; t < next->table_count; ++t)
        for (uint16_t a = 0; a < next->tables[t].spec.example.nb_actions && a < DPPD_RULE_MAX_ACTIONS; ++a) {
            struct dppd_action *action = &next->tables[t].spec.example.actions[a];
            if (action->type == DPPD_ACTION_SHARED_COUNT && action->conf.shared_count.generation == 0)
                for (uint32_t i = 0; i < count; ++i)
                    if (changes[i].operation == DPPD_FLOW_CONFIG_COUNT_ADD &&
                        changes[i].key == action->conf.shared_count.id)
                        action->conf.shared_count.generation = generations[i];
        }
    for (uint32_t i = 0; i < count; ++i)
        if (changes[i].operation == DPPD_FLOW_CONFIG_COUNT_DELETE) {
            for (uint32_t t = 0; t < next->table_count; ++t)
                if (uses_count(&next->tables[t].spec.example, changes[i].key))
                    return -EBUSY;
            for (uint32_t r = 0; r < next->rule_count; ++r)
                if (uses_count(&next->rules[r].rule, changes[i].key))
                    return -EBUSY;
        }
    qsort(next->tables, next->table_count, sizeof(next->tables[0]), compare_tables);
    qsort(next->counts, next->shared_count, sizeof(next->counts[0]), compare_counts);
    return dppd_flow_state_validate(next);
}

static int configure_locked(struct dppd_flow_store *s, uint64_t expected, const struct dppd_flow_config_change *changes,
    uint32_t count, struct dppd_flow_config_result *result, struct dppd_flow_request_record *receipt)
{
    if (s == NULL || changes == NULL || result == NULL || count == 0 || count > DPPD_FLOW_CONFIG_OPERATIONS)
        return -EINVAL;
    memset(result, 0, sizeof(*result));
    struct dppd_flow_service_status status;
    dppd_flow_service_status(s->service, &status);
    int rc = writable(s);
    struct dppd_flow_state *next = NULL;
    struct dppd_flow_template_id prepared[64] = {{0}};
    struct dppd_shared_count_spec created[64] = {{0}};
    if (rc == 0 && (status.jobs != 0 || status.isolated))
        rc = -EBUSY;
    if (rc == 0 && expected != status.revision)
        rc = -ESTALE;
    if (rc == 0 && expected >= UINT64_MAX - 1U)
        rc = -EOVERFLOW;
    if (rc != 0)
        goto done;
    next = malloc(sizeof(*next));
    if (next == NULL) {
        rc = -ENOMEM;
        goto done;
    }
    *next = *s->state;
    next->revision = expected + 1U;
    rc = prepare_configuration(s, next, changes, count, result->generations);
    if (rc != 0)
        goto done;
    /** 先可靠保存代次水位，创建失败或突然退出也不能在重启后复用已分配的身份 */
    if (next->shared_next_generation != s->state->shared_next_generation) {
        s->state->shared_next_generation = next->shared_next_generation;
        rc = dppd_flow_state_write(s->path, s->guard, s->state);
        if (rc != 0) {
            s->status.dirty = true;
            s->status.last_error = rc;
            rc = -EUCLEAN;
            goto done;
        }
    }
    for (uint32_t i = 0; i < count && rc == 0; ++i)
        if (changes[i].operation == DPPD_FLOW_CONFIG_COUNT_ADD) {
            int n = count_slot(next, changes[i].key);
            struct dppd_flow_shared_count_info info = {0};
            rc = dppd_flow_shared_counts_replay_add(s->counts, &next->counts[n], &info);
            if (info.spec.id != 0)
                created[n] = info.spec;
        }
    (void)dppd_flow_shared_counts_seed(s->counts, s->state->shared_next_generation);
    for (uint32_t t = 0; t < next->table_count && rc == 0; ++t) {
        int old = table_slot(s, next->tables[t].key);
        if (old >= 0)
            prepared[t] = s->tables[old];
        else
            rc = dppd_flow_template_create(s->templates, &next->tables[t].spec, &prepared[t]);
    }
    if (rc == 0)
        rc = dppd_flow_service_config_revision(s->service, expected);
    if (rc != 0) {
        for (uint32_t t = 0; t < next->table_count; ++t)
            if (prepared[t].owner != 0 && table_slot(s, next->tables[t].key) < 0)
                s->retired_tables[t] = prepared[t];
        memcpy(s->retired_counts, created, sizeof(created));
        int cleaned = cleanup_configuration(s);
        if (cleaned != 0)
            rc = cleaned;
        goto done;
    }
    for (uint32_t t = 0; t < s->state->table_count; ++t) {
        bool found = false;
        for (uint32_t n = 0; n < next->table_count; ++n)
            found |= s->state->tables[t].key == next->tables[n].key;
        if (!found)
            s->retired_tables[t] = s->tables[t];
    }
    for (uint32_t i = 0; i < s->state->shared_count; ++i)
        if (count_slot(next, s->state->counts[i].id) < 0)
            s->retired_counts[i] = s->state->counts[i];
    memcpy(s->tables, prepared, sizeof(prepared));
    memset(s->shared_created, 0, sizeof(s->shared_created));
    for (uint32_t i = 0; i < next->shared_count; ++i)
        s->shared_created[i] = true;
    *s->state = *next;
    result->applied = true;
    if (receipt != NULL) {
        receipt->result.phase = DPPD_FLOW_REQUEST_DONE;
        receipt->result.applied = true;
        receipt->result.revision = s->state->revision;
        memcpy(receipt->result.generations, result->generations, sizeof(result->generations));
        s->requests_dirty = true;
    }
    /** 失败不撤回已经采用的期望配置，保留旧对象与新对象，显式 flush 成功后再回收 */
    rc = flush(s);
    if (rc != 0)
        rc = -EUCLEAN;
done:
    if (receipt != NULL && receipt->result.phase == DPPD_FLOW_REQUEST_PENDING) {
        receipt->result.phase = DPPD_FLOW_REQUEST_DONE;
        receipt->result.status = rc;
        receipt->result.revision = s->state->revision;
        memcpy(receipt->result.generations, result->generations, sizeof(result->generations));
        s->requests_dirty = true;
        if (s->status.dirty || flush(s) != 0)
            rc = -EUCLEAN;
    }
    result->revision = s->state->revision;
    result->durable_revision = s->status.durable_revision;
    result->durable = result->applied && !s->status.dirty && result->revision == result->durable_revision;
    free(next);
    return rc;
}

int dppd_flow_store_configure(struct dppd_flow_store *s, uint64_t expected,
    const struct dppd_flow_config_change *changes, uint32_t count, struct dppd_flow_config_result *result)
{
    if (s == NULL)
        return -EINVAL;
    pthread_mutex_lock(&s->lock);
    int rc = configure_locked(s, expected, changes, count, result, NULL);
    pthread_mutex_unlock(&s->lock);
    return rc;
}

static int request_slot(struct dppd_flow_store *s, struct dppd_flow_request_key key)
{
    if (s->state->requests != NULL)
        for (uint32_t i = 0; i < DPPD_REQUEST_RECEIPTS; ++i) {
            struct dppd_flow_request_key saved = s->state->requests->receipts[i].result.key;
            if (saved.client == key.client && saved.sequence == key.sequence)
                return (int)i;
        }
    return -1;
}

static int request_result(struct dppd_flow_store *s, uint32_t slot, struct dppd_flow_request_result *result)
{
    *result = s->state->requests->receipts[slot].result;
    result->durable = !s->status.dirty && !s->requests_dirty;
    struct dppd_flow_service_status status;
    dppd_flow_service_status(s->service, &status);
    return s->status.dirty || s->requests_dirty || s->status.residual || status.isolated ? -EUCLEAN : 0;
}

int dppd_flow_store_request_get(struct dppd_flow_store *s, struct dppd_flow_request_key key,
    struct dppd_flow_request_result *result)
{
    if (s == NULL || result == NULL || key.client == 0 || key.sequence == 0)
        return -EINVAL;
    memset(result, 0, sizeof(*result));
    pthread_mutex_lock(&s->lock);
    int slot = request_slot(s, key), rc = -ENOENT;
    if (slot >= 0)
        rc = request_result(s, (uint32_t)slot, result);
    else if (s->state->requests != NULL)
        for (uint32_t c = 0; c < DPPD_REQUEST_CLIENTS; ++c)
            if (s->state->requests->clients[c].client == key.client &&
                s->state->requests->clients[c].sequence >= key.sequence)
                rc = -ESTALE;
    pthread_mutex_unlock(&s->lock);
    return rc;
}

/** 持久受理在进入异步队列前完成，掉电后最多保留明确中止，不能重复执行原键 */
int dppd_flow_store_request(struct dppd_flow_store *s, const struct dppd_flow_request *request, uint64_t now,
    struct dppd_flow_request_result *result)
{
    uint8_t contents[DPPD_REQUEST_BYTES];
    uint32_t length;
    if (s == NULL || result == NULL)
        return -EINVAL;
    memset(result, 0, sizeof(*result));
    int rc = dppd_flow_request_encode(request, contents, &length);
    if (rc != 0)
        return rc;
    pthread_mutex_lock(&s->lock);
    int slot = request_slot(s, request->key);
    if (slot >= 0) {
        struct dppd_flow_request_record *old = &s->state->requests->receipts[slot];
        rc = old->length != length || memcmp(old->contents, contents, length) != 0
            ? -EEXIST
            : request_result(s, (uint32_t)slot, result);
        goto done;
    }
    rc = writable(s);
    if (rc == 0 && s->state->config.mode != DPPD_FLOW_STATE_SOFTWARE)
        rc = -ENOTSUP;
    if (rc != 0)
        goto done;
    if (s->state->requests == NULL) {
        s->state->requests = calloc(1, sizeof(*s->state->requests));
        if (s->state->requests == NULL) {
            rc = -ENOMEM;
            goto done;
        }
    }
    struct dppd_flow_request_ledger *ledger = s->state->requests;
    int client = -1, empty = -1;
    for (uint32_t c = 0; c < DPPD_REQUEST_CLIENTS; ++c) {
        if (ledger->clients[c].client == request->key.client)
            client = (int)c;
        if (ledger->clients[c].client == 0 && empty < 0)
            empty = (int)c;
    }
    if (client >= 0 && request->key.sequence <= ledger->clients[client].sequence) {
        rc = -ESTALE;
        goto done;
    }
    if (client < 0)
        client = empty;
    if (client < 0) {
        rc = -ENOSPC;
        goto done;
    }
    for (uint32_t i = 0; i < DPPD_REQUEST_RECEIPTS; ++i) {
        uint32_t candidate = (ledger->cursor + i) % DPPD_REQUEST_RECEIPTS;
        if (ledger->receipts[candidate].result.key.client == 0 ||
            ledger->receipts[candidate].result.phase == DPPD_FLOW_REQUEST_DONE) {
            slot = (int)candidate;
            break;
        }
    }
    if (slot < 0) {
        rc = -ENOSPC;
        goto done;
    }
    struct dppd_flow_request_record *record = &ledger->receipts[slot];
    memset(record, 0, sizeof(*record));
    record->length = length;
    memcpy(record->contents, contents, length);
    record->result = (struct dppd_flow_request_result){.key = request->key,
        .operation = request->operation,
        .phase = DPPD_FLOW_REQUEST_PENDING,
        .count = request->count,
        .revision = s->state->revision};
    ledger->clients[client] = request->key;
    ledger->cursor = ((uint32_t)slot + 1U) % DPPD_REQUEST_RECEIPTS;
    s->requests_dirty = true;
    rc = flush(s);
    if (rc != 0) {
        /** 受理写盘失败时绝不入队，修复保存后也只保留失败结果 */
        record->result.phase = DPPD_FLOW_REQUEST_DONE;
        record->result.status = rc;
        s->requests_dirty = true;
        rc = request_result(s, (uint32_t)slot, result);
        goto done;
    }
    if (request->operation == DPPD_FLOW_REQUEST_CONFIGURE) {
        for (uint32_t i = 0; i < request->count; ++i)
            record->result.ids[i] = request->payload.configuration[i].key;
        struct dppd_flow_config_result configured;
        (void)configure_locked(s, request->expected_revision, request->payload.configuration, request->count,
            &configured, record);
    } else {
        struct dppd_async_flow_create_request creates[4];
        struct dppd_flow_update_request updates[4];
        if (request->operation == DPPD_FLOW_REQUEST_REMOVE) {
            rc = dppd_flow_service_remove(s->service, request->queue, request->payload.remove, request->count, now,
                &s->request_jobs[slot]);
        } else {
            rc = resolve(s, request->queue, request->payload.change.rules, request->count, creates);
            for (uint32_t i = 0; i < request->count && rc == 0; ++i) {
                updates[i] = (struct dppd_flow_update_request){request->payload.change.expected[i], creates[i]};
                if (request->operation == DPPD_FLOW_REQUEST_CREATE && request->payload.change.expected[i] != 0)
                    rc = -EINVAL;
            }
            if (rc == 0)
                rc = request->operation == DPPD_FLOW_REQUEST_CREATE
                    ? dppd_flow_service_submit(s->service, request->queue, creates, request->count, now,
                          &s->request_jobs[slot])
                    : dppd_flow_service_update(s->service, request->queue, updates, request->count, now,
                          &s->request_jobs[slot]);
        }
        if (rc != 0) {
            record->result.phase = DPPD_FLOW_REQUEST_DONE;
            record->result.status = rc;
            s->requests_dirty = true;
            (void)flush(s);
        }
    }
    rc = request_result(s, (uint32_t)slot, result);
done:
    pthread_mutex_unlock(&s->lock);
    return rc;
}

int dppd_flow_store_requests_cancel(struct dppd_flow_store *s)
{
    if (s == NULL)
        return -EINVAL;
    pthread_mutex_lock(&s->lock);
    for (uint32_t i = 0; i < DPPD_REQUEST_RECEIPTS; ++i)
        if (s->request_jobs[i].owner != 0)
            (void)dppd_flow_service_cancel(s->service, &s->request_jobs[i]);
    pthread_mutex_unlock(&s->lock);
    return 0;
}

int dppd_flow_store_shared_get(struct dppd_flow_store *s, uint64_t id, uint64_t generation,
    struct dppd_flow_shared_count_info *info)
{
    if (s == NULL || info == NULL)
        return -EINVAL;
    pthread_mutex_lock(&s->lock);
    int rc = available(s);
    if (rc == 0)
        rc = dppd_flow_shared_counts_get(s->counts, id, generation, info);
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
