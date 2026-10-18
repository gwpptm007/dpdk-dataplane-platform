#include "dppd/async_flow.h"
#include "dppd/slot_index.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum operation_state { OP_FREE, OP_QUEUED, OP_SENT, OP_DONE };

struct async_flow {
    struct dppd_rule rule;
    struct dppd_flow_template_id table;
    struct dppd_flow_template_lease lease;
    struct dppd_resource_ticket ticket;
    void *object;
    uint64_t serial;
    enum dppd_async_flow_state state;
    bool used;
};

struct async_operation {
    struct dppd_async_flow_result result;
    struct dppd_resource_ticket ticket;
    uint64_t submitted_ns;
    uint32_t slot;
    enum operation_state state;
    enum dppd_async_flow_state previous;
    bool timed_out;
};

struct dppd_async_flow_queue {
    pthread_mutex_t lock;
    struct dppd_async_flow_config config;
    struct dppd_slot_index index;
    struct async_flow *flows;
    struct async_operation operations[DPPD_ASYNC_FLOW_DEPTH];
    struct dppd_resource_ticket ticket;
    struct dppd_async_flow_status status;
    uint64_t owner, serial;
    bool empty_pull;
    const void *consumer;
};

static _Atomic uint64_t next_owner = 1;
static _Atomic uintptr_t next_tag = 1;
static pthread_mutex_t claim_lock = PTHREAD_MUTEX_INITIALIZER;
static struct dppd_async_flow_queue *claims[DPPD_MAX_PORTS * 8U];

/** 同一驱动上下文的同一端口队列只能有一个消费者，两个独立实例的锁不能保护同一 PMD 队列 */
static int claim_queue(struct dppd_async_flow_queue *queue)
{
    uint32_t free_slot = DPPD_MAX_PORTS * 8U;
    int rc = -ENOSPC;
    pthread_mutex_lock(&claim_lock);
    for (uint32_t i = 0; i < DPPD_MAX_PORTS * 8U; ++i) {
        struct dppd_async_flow_queue *other = claims[i];
        if (other == NULL) {
            free_slot = i;
        } else if (other->config.driver.create == queue->config.driver.create &&
            other->config.driver.context == queue->config.driver.context &&
            other->config.port_id == queue->config.port_id && other->config.queue_id == queue->config.queue_id) {
            rc = -EADDRINUSE;
            goto done;
        }
    }
    if (free_slot != DPPD_MAX_PORTS * 8U) {
        claims[free_slot] = queue;
        rc = 0;
    }
done:
    pthread_mutex_unlock(&claim_lock);
    return rc;
}

static void unclaim_queue(struct dppd_async_flow_queue *queue)
{
    pthread_mutex_lock(&claim_lock);
    for (uint32_t i = 0; i < DPPD_MAX_PORTS * 8U; ++i)
        if (claims[i] == queue) {
            claims[i] = NULL;
            break;
        }
    pthread_mutex_unlock(&claim_lock);
}

static int allocate_identity(uint64_t *owner)
{
    uint64_t value = atomic_load(&next_owner);
    while (value != UINT64_MAX)
        if (atomic_compare_exchange_weak(&next_owner, &value, value + 1U)) {
            *owner = value;
            return 0;
        }
    return -EOVERFLOW;
}

static int allocate_tag(uintptr_t *tag)
{
    uintptr_t value = atomic_load(&next_tag);
    while (value != UINTPTR_MAX)
        if (atomic_compare_exchange_weak(&next_tag, &value, value + 1U)) {
            *tag = value;
            return 0;
        }
    return -EOVERFLOW;
}

static struct async_flow *lookup(struct dppd_async_flow_queue *queue, const struct dppd_async_flow_id *id)
{
    struct async_flow *flow;
    if (id == NULL || id->owner != queue->owner || id->slot >= queue->config.flow_capacity)
        return NULL;
    flow = &queue->flows[id->slot];
    return flow->used && flow->serial == id->serial ? flow : NULL;
}

static void fault(struct dppd_async_flow_queue *queue, int error, bool fatal)
{
    queue->status.fault = true;
    queue->status.fatal |= fatal;
    queue->status.last_error = error < 0 ? error : -EIO;
    queue->empty_pull = false;
}

static int reserve(struct dppd_async_flow_queue *queue, enum dppd_resource_kind kind,
    struct dppd_resource_ticket *ticket)
{
    const struct dppd_resource_demand demand = {kind, queue->config.port_id, 1};
    return dppd_admission_reserve(queue->config.admission, &demand, 1, ticket);
}

int dppd_async_flow_queue_create(const struct dppd_async_flow_config *config, struct dppd_async_flow_queue **output)
{
    struct dppd_async_flow_queue *queue;
    int rc;
    if (config == NULL || output == NULL || *output != NULL || config->admission == NULL || config->templates == NULL ||
        config->driver.create == NULL || config->driver.destroy == NULL || config->driver.push == NULL ||
        config->driver.pull == NULL || config->driver.release == NULL || config->depth == 0 ||
        config->depth > DPPD_ASYNC_FLOW_DEPTH || config->queue_id >= 8 || config->flow_capacity == 0 ||
        config->flow_capacity > 65536 ||
        (config->flow_resource != DPPD_RESOURCE_HARDWARE_FLOW && config->flow_resource != DPPD_RESOURCE_SOFTWARE_RULE))
        return -EINVAL;
    queue = calloc(1, sizeof(*queue));
    if (queue == NULL)
        return -ENOMEM;
    queue->config = *config;
    queue->flows = calloc(config->flow_capacity, sizeof(*queue->flows));
    if (queue->flows == NULL) {
        free(queue);
        return -ENOMEM;
    }
    rc = dppd_slot_index_init(&queue->index, config->flow_capacity);
    if (rc == 0)
        rc = allocate_identity(&queue->owner);
    if (rc == 0)
        rc = claim_queue(queue);
    if (rc == 0)
        rc = reserve(queue, DPPD_RESOURCE_FLOW_QUEUE, &queue->ticket);
    if (rc == 0) {
        rc = pthread_mutex_init(&queue->lock, NULL);
        if (rc != 0) {
            dppd_admission_release(config->admission, &queue->ticket);
            rc = -rc;
        }
    }
    if (rc != 0) {
        unclaim_queue(queue);
        dppd_slot_index_fini(&queue->index);
        free(queue->flows);
        free(queue);
        return rc;
    }
    rc = dppd_admission_commit(config->admission, &queue->ticket);
    if (rc != 0) {
        pthread_mutex_destroy(&queue->lock);
        unclaim_queue(queue);
        dppd_admission_release(config->admission, &queue->ticket);
        dppd_slot_index_fini(&queue->index);
        free(queue->flows);
        free(queue);
        return rc;
    }
    queue->status.depth = config->depth;
    queue->status.port_id = config->port_id;
    queue->status.queue_id = config->queue_id;
    queue->status.flow_resource = config->flow_resource;
    queue->serial = 1;
    *output = queue;
    return 0;
}

int dppd_async_flow_queue_close(struct dppd_async_flow_queue **output)
{
    struct dppd_async_flow_queue *queue;
    int rc;
    if (output == NULL || *output == NULL)
        return -EINVAL;
    queue = *output;
    pthread_mutex_lock(&queue->lock);
    if (queue->consumer != NULL || queue->status.flows != 0 || queue->status.queued != 0 ||
        queue->status.pending != 0 || queue->status.completed != 0 || queue->status.fatal) {
        pthread_mutex_unlock(&queue->lock);
        return -EBUSY;
    }
    rc = dppd_admission_release(queue->config.admission, &queue->ticket);
    pthread_mutex_unlock(&queue->lock);
    if (rc != 0)
        return rc;
    pthread_mutex_destroy(&queue->lock);
    unclaim_queue(queue);
    dppd_slot_index_fini(&queue->index);
    free(queue->flows);
    free(queue);
    *output = NULL;
    return 0;
}

int dppd_async_flow_attach(struct dppd_async_flow_queue *queue, const void *owner, struct dppd_admission *admission,
    struct dppd_flow_templates *templates)
{
    int rc = 0;
    if (queue == NULL || owner == NULL)
        return -EINVAL;
    pthread_mutex_lock(&queue->lock);
    if (queue->config.admission != admission || queue->config.templates != templates)
        rc = -EINVAL;
    else if (queue->consumer != NULL || queue->status.flows != 0 || queue->status.queued != 0 ||
        queue->status.pending != 0 || queue->status.completed != 0 || queue->status.fault)
        rc = -EBUSY;
    else
        queue->consumer = owner;
    pthread_mutex_unlock(&queue->lock);
    return rc;
}

int dppd_async_flow_detach(struct dppd_async_flow_queue *queue, const void *owner)
{
    int rc = 0;
    if (queue == NULL || owner == NULL)
        return -EINVAL;
    pthread_mutex_lock(&queue->lock);
    if (queue->consumer != owner)
        rc = -ESTALE;
    else if (queue->status.flows != 0 || queue->status.queued != 0 || queue->status.pending != 0 ||
        queue->status.completed != 0 || queue->status.fault)
        rc = -EBUSY;
    else
        queue->consumer = NULL;
    pthread_mutex_unlock(&queue->lock);
    return rc;
}

static struct async_operation *free_operation(struct dppd_async_flow_queue *queue)
{
    uint32_t i;
    for (i = 0; i < queue->config.depth; ++i)
        if (queue->operations[i].state == OP_FREE)
            return &queue->operations[i];
    return NULL;
}

/** 操作额度覆盖排队、驱动未完成和完成未领取三个阶段，领取前不会腾出背压空间 */
static int prepare_operation(struct dppd_async_flow_queue *queue, uint32_t slot, bool destroy, uint64_t now_ns,
    struct async_operation **output)
{
    struct async_operation *op = free_operation(queue);
    struct async_flow *flow = &queue->flows[slot];
    int rc;
    if (op == NULL)
        return -EAGAIN;
    memset(op, 0, sizeof(*op));
    rc = allocate_tag(&op->result.tag);
    if (rc == 0)
        rc = reserve(queue, DPPD_RESOURCE_INFLIGHT_OP, &op->ticket);
    if (rc != 0)
        return rc;
    op->state = OP_QUEUED;
    op->slot = slot;
    op->previous = flow->state;
    op->submitted_ns = now_ns;
    op->result.flow = (struct dppd_async_flow_id){queue->owner, flow->serial, slot};
    op->result.rule_id = flow->rule.id;
    op->result.generation = flow->rule.generation;
    op->result.destroy = destroy;
    ++queue->status.queued;
    *output = op;
    return 0;
}

static int release_flow(struct dppd_async_flow_queue *queue, uint32_t slot)
{
    struct async_flow *flow = &queue->flows[slot];
    int rc = dppd_slot_index_erase(&queue->index, flow->rule.id, flow->rule.generation);
    if (rc == 0)
        rc = dppd_flow_template_release(queue->config.templates, &flow->lease);
    if (rc == 0)
        rc = dppd_admission_release(queue->config.admission, &flow->ticket);
    if (rc != 0) {
        fault(queue, -EUCLEAN, true);
        return -EUCLEAN;
    }
    memset(flow, 0, sizeof(*flow));
    --queue->status.flows;
    return 0;
}

static int prepare_flow(struct dppd_async_flow_queue *queue, const struct dppd_async_flow_create_request *request,
    uint32_t *slot)
{
    struct async_flow *flow;
    struct dppd_resource_demand demands[2] = {{queue->config.flow_resource, queue->config.port_id, 1},
        {DPPD_RESOURCE_COUNTER, queue->config.port_id, 0}};
    int rc;
    uint16_t i;
    rc = dppd_slot_index_insert(&queue->index, request->rule.id, request->rule.generation, slot);
    if (rc != 0)
        return rc;
    flow = &queue->flows[*slot];
    flow->rule = request->rule;
    flow->table = request->table;
    rc = dppd_flow_template_acquire(queue->config.templates, &request->table, &request->rule, &flow->lease);
    if (rc != 0)
        goto erase;
    for (i = 0; i < request->rule.nb_actions; ++i)
        if (request->rule.actions[i].type == DPPD_ACTION_COUNT)
            demands[1].amount = 1;
    rc = dppd_admission_reserve(queue->config.admission, demands, demands[1].amount == 0 ? 1 : 2, &flow->ticket);
    if (rc != 0) {
        dppd_flow_template_release(queue->config.templates, &flow->lease);
        goto erase;
    }
    flow->serial = queue->serial++;
    flow->used = true;
    flow->state = DPPD_ASYNC_FLOW_QUEUED;
    ++queue->status.flows;
    return 0;
erase:
    dppd_slot_index_erase(&queue->index, request->rule.id, request->rule.generation);
    memset(flow, 0, sizeof(*flow));
    return rc;
}

int dppd_async_flow_submit(struct dppd_async_flow_queue *queue, const struct dppd_async_flow_create_request *requests,
    uint32_t count, uint64_t now_ns, struct dppd_async_flow_id *flows)
{
    struct async_operation *prepared[DPPD_ASYNC_FLOW_BATCH];
    uint32_t i, prepared_count = 0;
    int rc = 0;
    if (queue == NULL || requests == NULL || flows == NULL || count == 0 || count > DPPD_ASYNC_FLOW_BATCH)
        return -EINVAL;
    pthread_mutex_lock(&queue->lock);
    if (queue->status.fault) {
        rc = -EUCLEAN;
        goto done;
    }
    if (count > queue->config.depth - queue->status.queued - queue->status.pending - queue->status.completed) {
        rc = -EAGAIN;
        goto done;
    }
    if (queue->serial >= UINT64_MAX - count) {
        rc = -EOVERFLOW;
        goto done;
    }
    for (i = 0; i < count; ++i) {
        const struct dppd_rule *rule = &requests[i].rule;
        uint32_t slot;
        if (rule->id == 0 || rule->id == UINT64_MAX || rule->generation == 0 || rule->generation == UINT64_MAX ||
            rule->install_port_id != queue->config.port_id) {
            rc = -EINVAL;
            break;
        }
        rc = prepare_flow(queue, &requests[i], &slot);
        if (rc != 0)
            break;
        rc = prepare_operation(queue, slot, false, now_ns, &prepared[prepared_count]);
        if (rc != 0) {
            release_flow(queue, slot);
            break;
        }
        ++prepared_count;
    }
    if (rc != 0) {
        /** 此时尚未调用任何驱动，撤回本批预留即可，不会留下已安装前缀 */
        for (i = 0; i < prepared_count; ++i) {
            struct async_operation *op = prepared[i];
            release_flow(queue, op->slot);
            dppd_admission_release(queue->config.admission, &op->ticket);
            memset(op, 0, sizeof(*op));
            --queue->status.queued;
        }
        goto done;
    }
    for (i = 0; i < count; ++i)
        flows[i] = prepared[i]->result.flow;
    queue->status.submitted += count;
done:
    pthread_mutex_unlock(&queue->lock);
    return rc;
}

int dppd_async_flow_destroy(struct dppd_async_flow_queue *queue, const struct dppd_async_flow_id *id, uint64_t now_ns)
{
    struct async_flow *flow;
    struct async_operation *op;
    int rc;
    if (queue == NULL)
        return -EINVAL;
    pthread_mutex_lock(&queue->lock);
    flow = lookup(queue, id);
    if (flow == NULL)
        rc = -ESTALE;
    else if (flow->object == NULL && flow->state == DPPD_ASYNC_FLOW_CREATE_FAILED)
        rc = -EUCLEAN;
    else if (flow->state != DPPD_ASYNC_FLOW_READY && flow->state != DPPD_ASYNC_FLOW_CREATE_FAILED)
        rc = -EBUSY;
    else {
        rc = prepare_operation(queue, id->slot, true, now_ns, &op);
        if (rc == 0) {
            flow->state = DPPD_ASYNC_FLOW_DESTROYING;
            ++queue->status.submitted;
        }
    }
    pthread_mutex_unlock(&queue->lock);
    return rc;
}

static void finish(struct dppd_async_flow_queue *queue, struct async_operation *op, int status)
{
    struct async_flow *flow = &queue->flows[op->slot];
    int rc = 0;
    if (op->result.destroy) {
        if (status == 0) {
            queue->config.driver.release(queue->config.driver.context, flow->object);
            flow->object = NULL;
            rc = release_flow(queue, op->slot);
        } else {
            flow->state = op->previous;
        }
    } else if (flow->object == NULL) {
        if (op->state == OP_QUEUED) {
            rc = release_flow(queue, op->slot);
        } else {
            /** 已声称入队却没有句柄属于未知实际状态，完成通知也不能代替实际清理 */
            flow->state = DPPD_ASYNC_FLOW_CREATE_FAILED;
            fault(queue, -EUCLEAN, true);
            rc = -EUCLEAN;
        }
    } else {
        flow->state = status == 0 ? DPPD_ASYNC_FLOW_READY : DPPD_ASYNC_FLOW_CREATE_FAILED;
        rc = dppd_admission_commit(queue->config.admission, &flow->ticket);
        if (rc != 0)
            fault(queue, -EUCLEAN, true);
    }
    op->result.status = rc != 0 ? -EUCLEAN : status;
    op->result.object_exists = flow->used && flow->object != NULL;
    if (op->state == OP_SENT)
        --queue->status.pending;
    else
        --queue->status.queued;
    op->state = OP_DONE;
    ++queue->status.completed;
    ++queue->status.completions;
}

int dppd_async_flow_cancel(struct dppd_async_flow_queue *queue, const struct dppd_async_flow_id *id)
{
    struct async_flow *flow;
    uint32_t i;
    int rc = -EBUSY;
    if (queue == NULL)
        return -EINVAL;
    pthread_mutex_lock(&queue->lock);
    flow = lookup(queue, id);
    if (flow == NULL)
        rc = -ESTALE;
    else
        for (i = 0; i < queue->config.depth; ++i) {
            struct async_operation *op = &queue->operations[i];
            if (op->state == OP_QUEUED && op->slot == id->slot) {
                finish(queue, op, -ECANCELED);
                rc = 0;
                break;
            }
        }
    pthread_mutex_unlock(&queue->lock);
    return rc;
}

static void dispatch(struct dppd_async_flow_queue *queue, struct async_operation *op)
{
    struct async_flow *flow = &queue->flows[op->slot];
    struct dppd_flow_template_info info;
    int rc;
    if (op->result.destroy) {
        rc = queue->config.driver.destroy(queue->config.driver.context, queue->config.port_id, queue->config.queue_id,
            flow->object, op->result.tag);
    } else {
        rc = dppd_flow_template_get(queue->config.templates, &flow->table, &info);
        if (rc == 0)
            rc = queue->config.driver.create(queue->config.driver.context, queue->config.port_id,
                queue->config.queue_id, info.parts.table, &flow->rule, op->result.tag, &flow->object);
        if ((rc == 0 && flow->object == NULL) || (rc != 0 && flow->object != NULL)) {
            /** 不合约的结果不能推断是否入队，保留身份等待精确完成并阻止新建 */
            fault(queue, -EUCLEAN, true);
            rc = 0;
        }
    }
    if (rc != 0) {
        finish(queue, op, rc);
        return;
    }
    op->state = OP_SENT;
    --queue->status.queued;
    ++queue->status.pending;
    if (!op->result.destroy)
        flow->state = DPPD_ASYNC_FLOW_CREATING;
    queue->empty_pull = false;
}

int dppd_async_flow_process(struct dppd_async_flow_queue *queue, uint64_t now_ns, uint64_t timeout_ns, uint32_t budget)
{
    struct dppd_async_driver_result results[DPPD_ASYNC_FLOW_BATCH];
    uint32_t i, sent = 0;
    int rc, count;
    if (queue == NULL || budget == 0 || budget > DPPD_ASYNC_FLOW_BATCH || timeout_ns == 0)
        return -EINVAL;
    pthread_mutex_lock(&queue->lock);
    for (i = 0; i < queue->config.depth && sent < budget; ++i) {
        struct async_operation *op = &queue->operations[i];
        if (op->state == OP_QUEUED && (!queue->status.fault || op->result.destroy)) {
            dispatch(queue, op);
            ++sent;
        }
    }
    if (queue->status.pending != 0) {
        rc = queue->config.driver.push(queue->config.driver.context, queue->config.port_id, queue->config.queue_id);
        if (rc != 0)
            fault(queue, rc, false);
    }
    count = queue->config.driver.pull(queue->config.driver.context, queue->config.port_id, queue->config.queue_id,
        results, budget);
    if (count < 0 || count > (int)budget) {
        fault(queue, count < 0 ? count : -EUCLEAN, count > (int)budget);
    } else {
        queue->empty_pull = count == 0;
        for (int result = 0; result < count; ++result) {
            struct async_operation *matched = NULL;
            for (i = 0; i < queue->config.depth; ++i)
                if (queue->operations[i].state == OP_SENT && queue->operations[i].result.tag == results[result].tag) {
                    matched = &queue->operations[i];
                    break;
                }
            if (matched == NULL)
                fault(queue, -EPROTO, true);
            else
                finish(queue, matched, results[result].status == 0 ? 0 : -EIO);
        }
    }
    for (i = 0; i < queue->config.depth; ++i) {
        struct async_operation *op = &queue->operations[i];
        if ((op->state == OP_SENT || op->state == OP_QUEUED) && !op->timed_out && now_ns >= op->submitted_ns &&
            now_ns - op->submitted_ns >= timeout_ns) {
            op->timed_out = true;
            ++queue->status.timeouts;
            fault(queue, -ETIMEDOUT, false);
        }
    }
    rc = queue->status.fault ? -EUCLEAN : 0;
    pthread_mutex_unlock(&queue->lock);
    return rc;
}

int dppd_async_flow_collect(struct dppd_async_flow_queue *queue, struct dppd_async_flow_result *results,
    uint32_t capacity)
{
    uint32_t i, count = 0;
    if (queue == NULL || results == NULL || capacity == 0 || capacity > DPPD_ASYNC_FLOW_BATCH)
        return -EINVAL;
    pthread_mutex_lock(&queue->lock);
    for (i = 0; i < queue->config.depth && count < capacity; ++i) {
        struct async_operation *op = &queue->operations[i];
        if (op->state != OP_DONE)
            continue;
        if (dppd_admission_release(queue->config.admission, &op->ticket) != 0) {
            fault(queue, -EUCLEAN, true);
            break;
        }
        results[count++] = op->result;
        memset(op, 0, sizeof(*op));
        --queue->status.completed;
    }
    pthread_mutex_unlock(&queue->lock);
    return (int)count;
}

int dppd_async_flow_get(struct dppd_async_flow_queue *queue, const struct dppd_async_flow_id *id,
    enum dppd_async_flow_state *state)
{
    struct async_flow *flow;
    if (queue == NULL || state == NULL)
        return -EINVAL;
    pthread_mutex_lock(&queue->lock);
    flow = lookup(queue, id);
    if (flow != NULL)
        *state = flow->state;
    pthread_mutex_unlock(&queue->lock);
    return flow == NULL ? -ESTALE : 0;
}

int dppd_async_flow_status(struct dppd_async_flow_queue *queue, struct dppd_async_flow_status *status)
{
    if (queue == NULL || status == NULL)
        return -EINVAL;
    pthread_mutex_lock(&queue->lock);
    *status = queue->status;
    pthread_mutex_unlock(&queue->lock);
    return 0;
}

int dppd_async_flow_resume(struct dppd_async_flow_queue *queue)
{
    int rc = 0;
    if (queue == NULL)
        return -EINVAL;
    pthread_mutex_lock(&queue->lock);
    if (queue->status.fatal || queue->status.queued != 0 || queue->status.pending != 0 ||
        queue->status.completed != 0 || !queue->empty_pull)
        rc = -EUCLEAN;
    for (uint32_t i = 0; rc == 0 && i < queue->config.flow_capacity; ++i)
        if (queue->flows[i].used && queue->flows[i].state != DPPD_ASYNC_FLOW_READY)
            rc = -EUCLEAN;
    if (rc == 0) {
        queue->status.fault = false;
        queue->status.last_error = 0;
    }
    pthread_mutex_unlock(&queue->lock);
    return rc;
}
