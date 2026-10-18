#include "dppd/flow_service.h"
#include "dppd/rule_repository.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/** 等完成、等清理和等恢复分别记账，不能把驱动受理或一次重试当作整批成功 */
enum job_state {
    JOB_FREE,
    JOB_NEW,
    JOB_CREATE_WAIT,
    JOB_DELETE_WAIT,
    JOB_CLEAN_START,
    JOB_CLEAN_WAIT,
    JOB_RESTORE_START,
    JOB_RESTORE_WAIT,
    JOB_RESIDUAL,
    JOB_DONE
};

/** 逻辑额度及模板保护跟随期望规则，实际 flow 和计数器额度由下层异步队列管理 */
struct service_rule {
    struct dppd_resource_ticket ticket;
    struct dppd_flow_template_lease pin;
    struct dppd_flow_template_id table;
    struct dppd_async_flow_id flow;
    uint32_t queue, job, item;
    bool committed, busy;
};

struct job_item {
    uint32_t slot;
    bool pending, have_flow, healthy;
};

struct flow_job {
    struct dppd_async_flow_create_request requests[64];
    struct job_item items[64];
    struct dppd_flow_job_result result;
    uint64_t serial, submitted_ns;
    uint32_t queue, waiting;
    enum job_state state;
    bool cleanup_failed, publication_unknown, auto_collect;
};

/** 多个队列可各自积压有界请求，控制消费者按顺序提交一整批，避免版本与账本交叉发布 */
struct dppd_flow_service {
    pthread_mutex_t lock;
    struct dppd_flow_service_config config;
    struct dppd_rule_repository rules;
    struct dppd_slot_index owners;
    struct service_rule *records;
    struct flow_job *jobs;
    struct dppd_flow_service_status status;
    uint32_t job_capacity, user_job_capacity, active, weights[8], age_queue_cursor;
    uint64_t owner, serial;
    uint64_t age_next_poll_ns, process_ns;
};

static _Atomic uint64_t next_owner = 1;

static int allocate_owner(uint64_t *owner)
{
    uint64_t current = atomic_load(&next_owner);
    while (current != UINT64_MAX)
        if (atomic_compare_exchange_weak(&next_owner, &current, current + 1U)) {
            *owner = current;
            return 0;
        }
    return -EOVERFLOW;
}

static void isolate(struct dppd_flow_service *service, int error)
{
    service->status.isolated = true;
    service->status.last_error = error < 0 ? error : -EUCLEAN;
}

static struct flow_job *lookup_job(struct dppd_flow_service *service, const struct dppd_flow_job_id *id)
{
    struct flow_job *job;
    if (id == NULL || id->owner != service->owner || id->slot >= service->job_capacity)
        return NULL;
    job = &service->jobs[id->slot];
    return job->state != JOB_FREE && job->serial == id->serial ? job : NULL;
}

/** 接管前必须为空，并逐队列登记独占身份，第二个服务不能领取本服务的完成消息 */
int dppd_flow_service_create(const struct dppd_flow_service_config *config, struct dppd_flow_service **output)
{
    struct dppd_flow_service *service;
    struct dppd_async_flow_status status;
    uint32_t i, attached = 0;
    int rc;
    if (config == NULL || output == NULL || *output != NULL || config->admission == NULL || config->templates == NULL ||
        config->queue_count == 0 || config->queue_count > 8 || config->depth == 0 || config->depth > 256 ||
        config->rule_capacity == 0 || config->rule_capacity > 16384 || config->timeout_ns == 0)
        return -EINVAL;
    if (config->software != NULL &&
        (!config->software->lock_initialized || config->software->capacity < config->rule_capacity ||
            dppd_software_backend_count(config->software) != 0))
        return -EINVAL;
    for (i = 0; i < config->queue_count; ++i) {
        if (dppd_async_flow_status(config->queues[i], &status) != 0 || status.depth != config->depth ||
            status.port_id != config->port_id || status.flows != 0 || status.queued != 0 || status.pending != 0 ||
            status.completed != 0 || status.fault ||
            status.flow_resource !=
                (config->software == NULL ? DPPD_RESOURCE_HARDWARE_FLOW : DPPD_RESOURCE_SOFTWARE_RULE))
            return -EINVAL;
        for (uint32_t j = 0; j < i; ++j)
            if (config->queues[i] == config->queues[j])
                return -EINVAL;
    }
    service = calloc(1, sizeof(*service));
    if (service == NULL)
        return -ENOMEM;
    service->config = *config;
    service->active = UINT32_MAX;
    service->serial = 1;
    service->user_job_capacity = config->queue_count * config->depth;
    /** 每队列另留一个有界老化任务，应用停止领取回执时也不会挡住维护删除 */
    service->job_capacity = service->user_job_capacity + config->queue_count;
    service->records = calloc(config->rule_capacity, sizeof(*service->records));
    service->jobs = calloc(service->job_capacity, sizeof(*service->jobs));
    rc = service->records == NULL || service->jobs == NULL ? -ENOMEM : 0;
    if (rc == 0)
        rc = dppd_rule_repository_init(&service->rules, config->rule_capacity);
    if (rc == 0)
        rc = dppd_slot_index_init(&service->owners, config->rule_capacity);
    if (rc == 0)
        rc = allocate_owner(&service->owner);
    if (rc == 0)
        for (; attached < config->queue_count; ++attached) {
            rc = dppd_async_flow_attach(config->queues[attached], service, config->admission, config->templates);
            if (rc != 0)
                break;
        }
    if (rc == 0) {
        rc = pthread_mutex_init(&service->lock, NULL);
        if (rc != 0)
            rc = -rc;
    }
    if (rc != 0) {
        while (attached != 0)
            dppd_async_flow_detach(config->queues[--attached], service);
        dppd_rule_repository_destroy(&service->rules);
        dppd_slot_index_fini(&service->owners);
        free(service->records);
        free(service->jobs);
        free(service);
        return rc;
    }
    *output = service;
    return 0;
}

int dppd_flow_service_close(struct dppd_flow_service **output)
{
    struct dppd_flow_service *service;
    if (output == NULL || *output == NULL)
        return -EINVAL;
    service = *output;
    pthread_mutex_lock(&service->lock);
    if (service->owners.count != 0 || service->status.jobs != 0 || service->status.isolated) {
        pthread_mutex_unlock(&service->lock);
        return -EBUSY;
    }
    for (uint32_t q = 0; q < service->config.queue_count; ++q)
        if (dppd_async_flow_detach(service->config.queues[q], service) != 0) {
            isolate(service, -EUCLEAN);
            pthread_mutex_unlock(&service->lock);
            return -EUCLEAN;
        }
    pthread_mutex_unlock(&service->lock);
    pthread_mutex_destroy(&service->lock);
    dppd_rule_repository_destroy(&service->rules);
    dppd_slot_index_fini(&service->owners);
    free(service->records);
    free(service->jobs);
    free(service);
    *output = NULL;
    return 0;
}

/** 深度按规则数计费而非请求数，64 条批次需要 64 个位置，完成未领取仍占全部位置 */
static int new_job(struct dppd_flow_service *service, uint32_t queue, uint32_t count, uint64_t now_ns, bool automatic,
    struct flow_job **output, uint32_t *slot)
{
    if (service->status.isolated)
        return -EUCLEAN;
    if (queue >= service->config.queue_count || count == 0 || count > 64)
        return -EINVAL;
    if (!automatic && count > service->config.depth - service->weights[queue])
        return -EAGAIN;
    if (service->serial == UINT64_MAX)
        return -EOVERFLOW;
    uint32_t begin = automatic ? service->user_job_capacity + queue : 0;
    uint32_t end = automatic ? begin + 1U : service->user_job_capacity;
    for (*slot = begin; *slot < end; ++*slot)
        if (service->jobs[*slot].state == JOB_FREE) {
            *output = &service->jobs[*slot];
            memset(*output, 0, sizeof(**output));
            (*output)->serial = service->serial++;
            (*output)->submitted_ns = now_ns;
            (*output)->queue = queue;
            (*output)->result.count = count;
            (*output)->auto_collect = automatic;
            return 0;
        }
    return -EAGAIN;
}

static int release_record(struct dppd_flow_service *service, uint64_t id, uint32_t slot)
{
    struct service_rule *record = &service->records[slot];
    int rc = dppd_flow_template_release(service->config.templates, &record->pin);
    if (rc == 0)
        rc = dppd_admission_release(service->config.admission, &record->ticket);
    if (rc == 0)
        rc = dppd_slot_index_erase(&service->owners, id, 0);
    if (rc != 0) {
        isolate(service, -EUCLEAN);
        return -EUCLEAN;
    }
    memset(record, 0, sizeof(*record));
    return 0;
}

/** 受理阶段只复制输入并预留逻辑额度和配置保护，真正调用驱动前还有实际额度的整批预检 */
int dppd_flow_service_submit(struct dppd_flow_service *service, uint32_t queue,
    const struct dppd_async_flow_create_request *requests, uint32_t count, uint64_t now_ns, struct dppd_flow_job_id *id)
{
    struct flow_job *job;
    uint32_t job_slot, prepared = 0;
    int rc;
    if (service == NULL || requests == NULL || id == NULL)
        return -EINVAL;
    pthread_mutex_lock(&service->lock);
    rc = new_job(service, queue, count, now_ns, false, &job, &job_slot);
    if (rc != 0)
        goto done;
    for (; prepared < count; ++prepared) {
        struct service_rule *record;
        const struct dppd_rule *rule = &requests[prepared].rule;
        struct dppd_resource_demand demand = {DPPD_RESOURCE_RULE, service->config.port_id, 1};
        uint32_t slot;
        if (rule->id == 0 || rule->id == UINT64_MAX || rule->generation != 0 ||
            rule->install_port_id != service->config.port_id ||
            (service->config.software != NULL && rule->fallback != DPPD_FALLBACK_SOFTWARE_ONLY) ||
            (service->config.software == NULL && rule->fallback == DPPD_FALLBACK_SOFTWARE_ONLY)) {
            rc = -EINVAL;
            break;
        }
        rc = dppd_slot_index_insert(&service->owners, rule->id, 0, &slot);
        if (rc != 0)
            break;
        record = &service->records[slot];
        rc = dppd_flow_template_pin(service->config.templates, &requests[prepared].table, rule, &record->pin);
        if (rc != 0) {
            dppd_slot_index_erase(&service->owners, rule->id, 0);
            break;
        }
        rc = dppd_admission_reserve(service->config.admission, &demand, 1, &record->ticket);
        if (rc != 0) {
            dppd_flow_template_release(service->config.templates, &record->pin);
            dppd_slot_index_erase(&service->owners, rule->id, 0);
            break;
        }
        record->job = job_slot;
        record->table = requests[prepared].table;
        record->item = prepared;
        record->queue = queue;
        record->busy = true;
        job->items[prepared].slot = slot;
        job->requests[prepared] = requests[prepared];
        job->result.ids[prepared] = rule->id;
    }
    if (rc != 0) {
        for (uint32_t i = 0; i < prepared; ++i)
            release_record(service, job->requests[i].rule.id, job->items[i].slot);
        memset(job, 0, sizeof(*job));
        goto done;
    }
    job->state = JOB_NEW;
    service->weights[queue] += count;
    ++service->status.jobs;
    *id = (struct dppd_flow_job_id){service->owner, job->serial, job_slot};
done:
    pthread_mutex_unlock(&service->lock);
    return rc;
}

/** 先锁定每个精确旧版本，暂不删账本，后续相同 ID 的请求不能越过正在执行的批次 */
static int submit_remove(struct dppd_flow_service *service, uint32_t queue,
    const struct dppd_flow_remove_request *requests, uint32_t count, uint64_t now_ns, bool automatic,
    struct dppd_flow_job_id *id)
{
    struct flow_job *job;
    uint32_t job_slot, prepared = 0;
    int rc;
    if (service == NULL || requests == NULL || id == NULL)
        return -EINVAL;
    pthread_mutex_lock(&service->lock);
    rc = new_job(service, queue, count, now_ns, automatic, &job, &job_slot);
    if (rc != 0)
        goto done;
    job->result.remove = true;
    for (; prepared < count; ++prepared) {
        uint32_t slot = dppd_slot_index_find(&service->owners, requests[prepared].id, 0);
        struct service_rule *record;
        if (slot == DPPD_SLOT_NONE || requests[prepared].generation == 0 ||
            requests[prepared].generation == UINT64_MAX) {
            rc = -ESTALE;
            break;
        }
        record = &service->records[slot];
        if (!record->committed || record->busy) {
            rc = -EBUSY;
            break;
        }
        rc = dppd_rule_repository_get(&service->rules, requests[prepared].id, &job->requests[prepared].rule);
        if (rc != 0 || job->requests[prepared].rule.generation != requests[prepared].generation) {
            rc = -ESTALE;
            break;
        }
        /** 原模板身份随逻辑 pin 保留，删除中途失败时可以按旧版本恢复实际对象 */
        job->requests[prepared].table = record->table;
        job->items[prepared] = (struct job_item){.slot = slot, .have_flow = true, .healthy = true};
        job->result.ids[prepared] = requests[prepared].id;
        job->result.generations[prepared] = requests[prepared].generation;
        record->busy = true;
        record->job = job_slot;
        record->item = prepared;
    }
    if (rc != 0) {
        for (uint32_t i = 0; i < prepared; ++i)
            service->records[job->items[i].slot].busy = false;
        memset(job, 0, sizeof(*job));
        goto done;
    }
    job->state = JOB_NEW;
    if (!automatic)
        service->weights[queue] += count;
    else
        ++service->status.age_jobs;
    ++service->status.jobs;
    *id = (struct dppd_flow_job_id){service->owner, job->serial, job_slot};
done:
    pthread_mutex_unlock(&service->lock);
    return rc;
}

int dppd_flow_service_remove(struct dppd_flow_service *service, uint32_t queue,
    const struct dppd_flow_remove_request *requests, uint32_t count, uint64_t now_ns, struct dppd_flow_job_id *id)
{
    return submit_remove(service, queue, requests, count, now_ns, false, id);
}

static bool rule_aging(const struct dppd_rule *rule)
{
    for (uint16_t i = 0; i < rule->nb_actions; ++i)
        if (rule->actions[i].type == DPPD_ACTION_AGE)
            return true;
    return false;
}

static uint64_t after_ns(uint64_t now_ns, uint64_t delay)
{
    return UINT64_MAX - now_ns < delay ? UINT64_MAX : now_ns + delay;
}

/** 自动老化也走完整删除任务，只有终态才领取内部回执，不能无限积压无人领取的结果 */
static void collect_age_job(struct dppd_flow_service *service, struct flow_job *job)
{
    if (job->result.applied) {
        service->status.aged_deleted += job->result.count;
        service->status.age_delete_error = 0;
    } else {
        ++service->status.age_delete_errors;
        service->status.age_delete_error = job->result.status;
        service->age_next_poll_ns = after_ns(service->process_ns, UINT64_C(1000000000));
    }
    --service->status.jobs;
    --service->status.completed;
    --service->status.age_jobs;
    memset(job, 0, sizeof(*job));
}

static void complete_job(struct dppd_flow_service *service, struct flow_job *job, bool applied)
{
    job->result.applied = applied;
    job->state = JOB_DONE;
    ++service->status.completed;
    service->active = UINT32_MAX;
    if (job->auto_collect)
        collect_age_job(service, job);
}

static int software_publish(struct dppd_flow_service *service, struct flow_job *job)
{
    struct dppd_software_change changes[64];
    if (service->config.software == NULL)
        return 0;
    for (uint32_t i = 0; i < job->result.count; ++i)
        changes[i] = (struct dppd_software_change){job->requests[i].rule.id,
            job->result.remove ? job->requests[i].rule.generation : 0,
            job->result.remove ? NULL : &job->requests[i].rule};
    return dppd_software_backend_change_batch(service->config.software, changes, job->result.count);
}

/** 实际结果齐全后先提交软件完整快照，再执行不分配内存的账本发布，失败前不产生成功回执 */
static int publish(struct dppd_flow_service *service, struct flow_job *job)
{
    struct dppd_rule rules[64];
    uint64_t base = service->rules.generation;
    uint32_t count = job->result.count;
    int rc;
    for (uint32_t i = 0; i < count; ++i) {
        rules[i] = job->requests[i].rule;
        if (!job->result.remove) {
            rc = dppd_admission_commit(service->config.admission, &service->records[job->items[i].slot].ticket);
            if (rc != 0)
                return rc;
        }
    }
    rc = software_publish(service, job);
    if (rc != 0)
        return rc;
    rc = job->result.remove ? dppd_rule_repository_remove_batch(&service->rules, rules, count)
                            : dppd_rule_repository_create_batch(&service->rules, rules, count);
    if (rc != 0) {
        isolate(service, -EUCLEAN);
        job->state = JOB_RESIDUAL;
        job->publication_unknown = true;
        job->result.status = -EUCLEAN;
        return -EUCLEAN;
    }
    for (uint32_t i = 0; i < count; ++i) {
        struct service_rule *record = &service->records[job->items[i].slot];
        job->result.generations[i] = base + i + 1U;
        if (job->result.remove) {
            if (release_record(service, rules[i].id, job->items[i].slot) != 0) {
                job->state = JOB_RESIDUAL;
                job->publication_unknown = true;
                job->result.status = -EUCLEAN;
                return -EUCLEAN;
            }
            if (rule_aging(&rules[i]))
                --service->status.aged_rules;
        } else {
            record->committed = true;
            record->busy = false;
            if (rule_aging(&rules[i]))
                ++service->status.aged_rules;
        }
    }
    complete_job(service, job, true);
    return 0;
}

/** 开始处理时才分配连续版本，先检查整批版本空间，过期的未发送请求可以直接撤销 */
static void activate(struct dppd_flow_service *service, struct flow_job *job, uint64_t now_ns)
{
    struct dppd_async_flow_id flows[64];
    uint32_t count = job->result.count;
    int rc;
    if (now_ns >= job->submitted_ns && now_ns - job->submitted_ns >= service->config.timeout_ns) {
        job->result.status = -ETIMEDOUT;
        if (job->result.remove) {
            for (uint32_t i = 0; i < count; ++i)
                service->records[job->items[i].slot].busy = false;
            complete_job(service, job, false);
        } else {
            job->state = JOB_CLEAN_START;
        }
        return;
    }
    if (service->rules.generation >= UINT64_MAX - count) {
        job->result.status = -EOVERFLOW;
        if (job->result.remove) {
            for (uint32_t i = 0; i < count; ++i)
                service->records[job->items[i].slot].busy = false;
            complete_job(service, job, false);
        } else {
            job->state = JOB_CLEAN_START;
        }
        return;
    }
    if (job->result.remove) {
        job->state = JOB_DELETE_WAIT;
        for (uint32_t i = 0; i < count; ++i) {
            struct service_rule *record = &service->records[job->items[i].slot];
            rc = dppd_async_flow_destroy(service->config.queues[record->queue], &record->flow, now_ns);
            if (rc == 0) {
                job->items[i].pending = true;
                ++job->waiting;
            } else if (job->result.status == 0) {
                job->result.status = rc;
            }
        }
        return;
    }
    for (uint32_t i = 0; i < count; ++i)
        job->requests[i].rule.generation = service->rules.generation + i + 1U;
    rc = dppd_async_flow_submit(service->config.queues[job->queue], job->requests, count, now_ns, flows);
    if (rc != 0) {
        job->result.status = rc;
        job->state = JOB_CLEAN_START;
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        service->records[job->items[i].slot].flow = flows[i];
        job->items[i].pending = true;
        job->items[i].have_flow = true;
    }
    job->waiting = count;
    job->state = JOB_CREATE_WAIT;
}

/** 下层已经核对完成标签，这里再次核对队列、规则、任务和 flow 凭据，防止串到其他批次 */
static void receive(struct dppd_flow_service *service, uint32_t queue, const struct dppd_async_flow_result *result)
{
    uint32_t slot = dppd_slot_index_find(&service->owners, result->rule_id, 0);
    struct service_rule *record;
    struct flow_job *job;
    struct job_item *item;
    enum dppd_async_flow_state state;
    if (slot == DPPD_SLOT_NONE || service->active == UINT32_MAX) {
        isolate(service, -EPROTO);
        return;
    }
    record = &service->records[slot];
    job = &service->jobs[service->active];
    if (record->queue != queue || record->job != service->active || record->flow.owner != result->flow.owner ||
        record->flow.serial != result->flow.serial || record->flow.slot != result->flow.slot ||
        record->item >= job->result.count || !job->items[record->item].pending || job->waiting == 0) {
        isolate(service, -EPROTO);
        return;
    }
    item = &job->items[record->item];
    item->pending = false;
    --job->waiting;
    item->have_flow = dppd_async_flow_get(service->config.queues[queue], &record->flow, &state) == 0;
    if (!result->destroy)
        item->healthy = result->status == 0 && item->have_flow && state == DPPD_ASYNC_FLOW_READY;
    else if (!item->have_flow)
        item->healthy = false;
    if (result->status != 0) {
        if (job->result.status == 0)
            job->result.status = result->status;
        if (job->state == JOB_CLEAN_WAIT || job->state == JOB_RESTORE_WAIT)
            job->cleanup_failed = true;
    }
}

/** 所有创建完成都收齐后才能清理句柄，清理失败继续保留任务、逻辑额度及模板保护 */
static void cleanup(struct dppd_flow_service *service, struct flow_job *job, uint64_t now_ns)
{
    uint32_t remaining = 0;
    for (uint32_t i = 0; i < job->result.count; ++i) {
        struct job_item *item = &job->items[i];
        struct service_rule *record = &service->records[item->slot];
        if (!item->have_flow || (job->result.remove && item->healthy))
            continue;
        ++remaining;
        int rc = dppd_async_flow_destroy(service->config.queues[record->queue], &record->flow, now_ns);
        if (rc == 0) {
            item->pending = true;
            ++job->waiting;
        } else if (rc != -EAGAIN) {
            job->cleanup_failed = true;
        }
    }
    if (job->waiting != 0) {
        job->state = JOB_CLEAN_WAIT;
    } else if (job->cleanup_failed) {
        job->state = JOB_RESIDUAL;
        isolate(service, -EUCLEAN);
    } else if (remaining == 0) {
        if (job->result.remove) {
            job->state = JOB_RESTORE_START;
        } else {
            for (uint32_t i = 0; i < job->result.count; ++i)
                if (release_record(service, job->requests[i].rule.id, job->items[i].slot) != 0) {
                    job->state = JOB_RESIDUAL;
                    return;
                }
            complete_job(service, job, false);
        }
    }
}

/** 删除失败时按原队列和精确旧版本补回缺失流，硬件历史计数不承诺恢复 */
static void restore(struct dppd_flow_service *service, struct flow_job *job, uint64_t now_ns)
{
    struct dppd_async_flow_create_request requests[64];
    struct dppd_async_flow_id flows[64];
    uint32_t item_indices[64], healthy = 0;
    int rc;
    for (uint32_t q = 0; q < service->config.queue_count; ++q) {
        uint32_t count = 0;
        for (uint32_t i = 0; i < job->result.count; ++i) {
            struct service_rule *record = &service->records[job->items[i].slot];
            if (record->queue != q)
                continue;
            if (job->items[i].healthy) {
                ++healthy;
            } else if (!job->items[i].have_flow) {
                requests[count] = job->requests[i];
                item_indices[count++] = i;
            }
        }
        if (count == 0)
            continue;
        rc = dppd_async_flow_submit(service->config.queues[q], requests, count, now_ns, flows);
        if (rc != 0) {
            job->cleanup_failed = true;
            continue;
        }
        for (uint32_t j = 0; j < count; ++j) {
            struct job_item *item = &job->items[item_indices[j]];
            service->records[item->slot].flow = flows[j];
            item->pending = item->have_flow = true;
            ++job->waiting;
        }
    }
    if (healthy == job->result.count) {
        for (uint32_t i = 0; i < job->result.count; ++i)
            service->records[job->items[i].slot].busy = false;
        complete_job(service, job, false);
    } else if (job->waiting != 0) {
        job->state = JOB_RESTORE_WAIT;
    } else {
        job->state = JOB_RESIDUAL;
        isolate(service, -EUCLEAN);
    }
}

/** 每次只推进一个可执行阶段，不在控制线程里忙等硬件或无限重复失败的补偿 */
static void advance(struct dppd_flow_service *service, struct flow_job *job, uint64_t now_ns)
{
    if (job->waiting != 0)
        return;
    switch (job->state) {
    case JOB_CREATE_WAIT:
    case JOB_DELETE_WAIT:
        if (job->result.status == 0) {
            int rc = publish(service, job);
            if (rc == 0 || job->state == JOB_RESIDUAL)
                break;
            job->result.status = rc;
        }
        job->state = job->result.remove ? JOB_RESTORE_START : JOB_CLEAN_START;
        break;
    case JOB_CLEAN_START:
        cleanup(service, job, now_ns);
        break;
    case JOB_CLEAN_WAIT:
    case JOB_RESTORE_WAIT:
        if (job->cleanup_failed) {
            job->state = JOB_RESIDUAL;
            isolate(service, -EUCLEAN);
        } else {
            job->state = job->state == JOB_CLEAN_WAIT ? JOB_CLEAN_START : JOB_RESTORE_START;
        }
        break;
    case JOB_RESTORE_START:
        restore(service, job, now_ns);
        break;
    default:
        break;
    }
}

int dppd_flow_service_process(struct dppd_flow_service *service, uint64_t now_ns, uint32_t budget)
{
    struct dppd_async_flow_result results[64];
    uint64_t oldest = UINT64_MAX;
    int rc = 0;
    if (service == NULL || budget == 0 || budget > 64)
        return -EINVAL;
    dppd_flow_service_age_poll(service, now_ns, budget);
    pthread_mutex_lock(&service->lock);
    service->process_ns = now_ns;
    if (service->active == UINT32_MAX && !service->status.isolated) {
        for (uint32_t i = 0; i < service->job_capacity; ++i)
            if (service->jobs[i].state == JOB_NEW && service->jobs[i].serial < oldest) {
                oldest = service->jobs[i].serial;
                service->active = i;
            }
        if (service->active != UINT32_MAX)
            activate(service, &service->jobs[service->active], now_ns);
    }
    for (uint32_t q = 0; q < service->config.queue_count; ++q) {
        rc = dppd_async_flow_process(service->config.queues[q], now_ns, service->config.timeout_ns, budget);
        if (rc != 0) {
            isolate(service, rc);
            if (service->active != UINT32_MAX && service->jobs[service->active].result.status == 0)
                service->jobs[service->active].result.status = -EUCLEAN;
        }
        int count = dppd_async_flow_collect(service->config.queues[q], results, budget);
        if (count < 0)
            isolate(service, count);
        for (int i = 0; i < count; ++i)
            receive(service, q, &results[i]);
    }
    if (service->active != UINT32_MAX)
        advance(service, &service->jobs[service->active], now_ns);
    rc = service->status.isolated ? -EUCLEAN : 0;
    pthread_mutex_unlock(&service->lock);
    return rc;
}

int dppd_flow_service_collect(struct dppd_flow_service *service, const struct dppd_flow_job_id *id,
    struct dppd_flow_job_result *result)
{
    struct flow_job *job;
    int rc = 0;
    if (service == NULL || result == NULL)
        return -EINVAL;
    pthread_mutex_lock(&service->lock);
    job = lookup_job(service, id);
    if (job == NULL)
        rc = -ESTALE;
    else if (job->auto_collect)
        rc = -EPERM;
    else if (job->state != JOB_DONE)
        rc = job->state == JOB_RESIDUAL ? -EUCLEAN : -EAGAIN;
    else {
        *result = job->result;
        service->weights[job->queue] -= job->result.count;
        --service->status.jobs;
        --service->status.completed;
        memset(job, 0, sizeof(*job));
    }
    pthread_mutex_unlock(&service->lock);
    return rc;
}

int dppd_flow_service_cancel(struct dppd_flow_service *service, const struct dppd_flow_job_id *id)
{
    struct flow_job *job;
    int rc = 0;
    if (service == NULL)
        return -EINVAL;
    pthread_mutex_lock(&service->lock);
    job = lookup_job(service, id);
    if (job == NULL)
        rc = -ESTALE;
    else if (job->auto_collect)
        rc = -EPERM;
    else if (job->state != JOB_NEW)
        rc = -EBUSY;
    else {
        for (uint32_t i = 0; i < job->result.count; ++i)
            if (job->result.remove)
                service->records[job->items[i].slot].busy = false;
            else if (release_record(service, job->requests[i].rule.id, job->items[i].slot) != 0)
                rc = -EUCLEAN;
        if (rc == 0) {
            job->result.status = -ECANCELED;
            /** 未激活的取消不能清除另一笔正在处理的 active 身份 */
            job->state = JOB_DONE;
            ++service->status.completed;
        }
    }
    pthread_mutex_unlock(&service->lock);
    return rc;
}

/** 只重试仍能精确定位的清理，账本发布异常或陌生完成不能靠普通重试清除 */
int dppd_flow_service_retry(struct dppd_flow_service *service)
{
    int rc = 0;
    if (service == NULL)
        return -EINVAL;
    pthread_mutex_lock(&service->lock);
    for (uint32_t q = 0; q < service->config.queue_count; ++q) {
        struct dppd_async_flow_status status;
        dppd_async_flow_status(service->config.queues[q], &status);
        if (status.fault)
            dppd_async_flow_resume(service->config.queues[q]);
    }
    if (service->active != UINT32_MAX) {
        struct flow_job *job = &service->jobs[service->active];
        if (job->publication_unknown) {
            rc = -EUCLEAN;
        } else if (job->state != JOB_RESIDUAL) {
            rc = -EAGAIN;
        } else {
            job->cleanup_failed = false;
            job->state = JOB_CLEAN_START;
        }
    } else {
        for (uint32_t q = 0; q < service->config.queue_count; ++q) {
            struct dppd_async_flow_status status;
            dppd_async_flow_status(service->config.queues[q], &status);
            if (status.fault && dppd_async_flow_resume(service->config.queues[q]) != 0)
                rc = -EUCLEAN;
        }
        if (rc == 0) {
            service->status.isolated = false;
            service->status.last_error = 0;
        }
    }
    pthread_mutex_unlock(&service->lock);
    return rc;
}

int dppd_flow_service_get(struct dppd_flow_service *service, uint64_t id, struct dppd_rule *rule)
{
    int rc;
    if (service == NULL || rule == NULL)
        return -EINVAL;
    pthread_mutex_lock(&service->lock);
    rc = dppd_rule_repository_get(&service->rules, id, rule);
    pthread_mutex_unlock(&service->lock);
    return rc;
}

int dppd_flow_service_status(struct dppd_flow_service *service, struct dppd_flow_service_status *status)
{
    if (service == NULL || status == NULL)
        return -EINVAL;
    pthread_mutex_lock(&service->lock);
    *status = service->status;
    status->rules = service->rules.count;
    status->revision = service->rules.generation;
    pthread_mutex_unlock(&service->lock);
    return 0;
}

int dppd_flow_service_age_poll(struct dppd_flow_service *service, uint64_t now_ns, uint32_t budget)
{
    struct dppd_flow_remove_request removals[64];
    struct dppd_async_aged_flow aged[64];
    uint32_t queues[64], selected = 0;
    int first_error = 0, accepted = 0;
    if (service == NULL || budget == 0 || budget > 64)
        return -EINVAL;
    pthread_mutex_lock(&service->lock);
    if (service->status.aged_rules == 0 || service->status.isolated || now_ns < service->age_next_poll_ns) {
        pthread_mutex_unlock(&service->lock);
        return 0;
    }
    service->age_next_poll_ns = after_ns(now_ns, UINT64_C(100000000));
    uint32_t start_queue = service->age_queue_cursor;
    for (uint32_t visited = 0; visited < service->config.queue_count && selected < budget; ++visited) {
        uint32_t q = (start_queue + visited) % service->config.queue_count;
        service->age_queue_cursor = (q + 1U) % service->config.queue_count;
        uint32_t capacity = budget - selected;
        if (capacity > service->config.depth)
            capacity = service->config.depth;
        int count = dppd_async_flow_aged(service->config.queues[q], aged, capacity);
        ++service->status.age_queries;
        if (count == -ENOTSUP)
            continue;
        if (count < 0) {
            ++service->status.age_query_errors;
            if (first_error == 0)
                first_error = count;
            continue;
        }
        for (int i = 0; i < count; ++i) {
            uint32_t slot = dppd_slot_index_find(&service->owners, aged[i].id, 0);
            struct dppd_rule rule;
            if (slot == DPPD_SLOT_NONE)
                continue;
            struct service_rule *record = &service->records[slot];
            if (!record->committed || record->busy || record->queue != q || record->flow.owner != aged[i].flow.owner ||
                record->flow.serial != aged[i].flow.serial || record->flow.slot != aged[i].flow.slot ||
                dppd_rule_repository_get(&service->rules, aged[i].id, &rule) != 0 ||
                rule.generation != aged[i].generation || !rule_aging(&rule))
                continue;
            removals[selected] = (struct dppd_flow_remove_request){rule.id, rule.generation};
            queues[selected++] = q;
        }
    }
    service->status.age_query_error = first_error;
    pthread_mutex_unlock(&service->lock);
    /** 解锁后走同一个精确删除入口，其他请求抢先更新时由版本和 busy 再次拦截 */
    for (uint32_t q = 0; q < service->config.queue_count; ++q) {
        struct dppd_flow_remove_request batch[64];
        uint32_t count = 0;
        struct dppd_flow_job_id id;
        for (uint32_t i = 0; i < selected; ++i)
            if (queues[i] == q)
                batch[count++] = removals[i];
        if (count == 0)
            continue;
        int rc = submit_remove(service, q, batch, count, now_ns, true, &id);
        pthread_mutex_lock(&service->lock);
        if (rc == 0) {
            accepted += (int)count;
        } else if (rc == -EAGAIN || rc == -EBUSY) {
            ++service->status.age_backpressure;
        } else if (rc != -ESTALE && rc != -ENOENT) {
            service->status.age_delete_error = rc;
            ++service->status.age_delete_errors;
            if (first_error == 0)
                first_error = rc;
        }
        pthread_mutex_unlock(&service->lock);
    }
    return first_error != 0 ? first_error : accepted;
}
