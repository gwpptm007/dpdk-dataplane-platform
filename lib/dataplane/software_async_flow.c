#include "dppd/async_flow.h"
#include "dppd/software_backend.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct software_completion_queue {
    struct dppd_async_driver_result results[DPPD_ASYNC_FLOW_DEPTH];
    uint16_t count, ready;
};

struct dppd_software_async_flows {
    /** 每个队列由 async_flow 的实例锁独占，跨队列仅共享原子的对象总数 */
    struct software_completion_queue queues[8];
    atomic_uint objects;
};

struct software_flow_object {
    struct dppd_software_async_flows *owner;
    uint64_t id, generation;
    uint16_t port, queue;
};

static int software_create(void *context, uint16_t port, uint32_t queue, void *table, const struct dppd_rule *rule,
    uintptr_t tag, void **output)
{
    struct dppd_software_async_flows *flows = context;
    struct software_flow_object *object;
    char error[128];
    if (flows == NULL || queue >= 8 || table != NULL || rule->fallback != DPPD_FALLBACK_SOFTWARE_ONLY ||
        dppd_rule_validate(rule, error, sizeof(error)) != 0 || !dppd_software_backend_rule_supported(rule))
        return -ENOTSUP;
    if (flows->queues[queue].count == DPPD_ASYNC_FLOW_DEPTH)
        return -EAGAIN;
    object = calloc(1, sizeof(*object));
    if (object == NULL)
        return -ENOMEM;
    *object = (struct software_flow_object){flows, rule->id, rule->generation, port, (uint16_t)queue};
    atomic_fetch_add(&flows->objects, 1);
    flows->queues[queue].results[flows->queues[queue].count++] = (struct dppd_async_driver_result){tag, 0};
    *output = object;
    return 0;
}

static int software_destroy(void *context, uint16_t port, uint32_t queue, void *handle, uintptr_t tag)
{
    struct dppd_software_async_flows *flows = context;
    struct software_flow_object *object = handle;
    if (object == NULL || object->owner != flows || object->queue != queue || object->port != port || queue >= 8)
        return -ESTALE;
    if (flows->queues[queue].count == DPPD_ASYNC_FLOW_DEPTH)
        return -EAGAIN;
    flows->queues[queue].results[flows->queues[queue].count++] = (struct dppd_async_driver_result){tag, 0};
    return 0;
}

static int software_push(void *context, uint16_t port, uint32_t queue)
{
    struct dppd_software_async_flows *flows = context;
    (void)port;
    if (queue >= 8)
        return -EINVAL;
    flows->queues[queue].ready = flows->queues[queue].count;
    return 0;
}

static int software_pull(void *context, uint16_t port, uint32_t queue, struct dppd_async_driver_result *results,
    uint16_t count)
{
    struct dppd_software_async_flows *flows = context;
    struct software_completion_queue *pending;
    uint16_t copied;
    (void)port;
    if (queue >= 8)
        return -EINVAL;
    pending = &flows->queues[queue];
    copied = count < pending->ready ? count : pending->ready;
    memcpy(results, pending->results, copied * sizeof(*results));
    pending->count -= copied;
    pending->ready -= copied;
    memmove(pending->results, pending->results + copied, pending->count * sizeof(*results));
    return copied;
}

static void software_release(void *context, void *handle)
{
    struct dppd_software_async_flows *flows = context;
    atomic_fetch_sub(&flows->objects, 1);
    free(handle);
}

int dppd_software_async_flows_create(struct dppd_software_async_flows **output, struct dppd_async_flow_driver *driver)
{
    struct dppd_software_async_flows *flows;
    if (output == NULL || *output != NULL || driver == NULL)
        return -EINVAL;
    flows = calloc(1, sizeof(*flows));
    if (flows == NULL)
        return -ENOMEM;
    *driver = (struct dppd_async_flow_driver){.context = flows,
        .create = software_create,
        .destroy = software_destroy,
        .push = software_push,
        .pull = software_pull,
        .release = software_release};
    *output = flows;
    return 0;
}

int dppd_software_async_flows_close(struct dppd_software_async_flows **output)
{
    struct dppd_software_async_flows *flows;
    if (output == NULL || *output == NULL)
        return -EINVAL;
    flows = *output;
    if (atomic_load(&flows->objects) != 0)
        return -EBUSY;
    for (unsigned int i = 0; i < 8; ++i)
        if (flows->queues[i].count != 0)
            return -EBUSY;
    free(flows);
    *output = NULL;
    return 0;
}
