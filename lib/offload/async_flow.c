#define ALLOW_EXPERIMENTAL_API
#include "dppd/async_flow.h"
#include "compiled_flow.h"

#include <errno.h>
#include <stdlib.h>
#include <rte_errno.h>
#include <rte_version.h>

#if RTE_VERSION >= RTE_VERSION_NUM(24, 11, 0, 0)
struct native_async_flow {
    /** 保留自引用编译存储到确认删除，驱动可在创建完成前读取其参数 */
    struct dppd_compiled_flow compiled;
    struct rte_flow *flow;
};

static int native_create(void *context, uint16_t port, uint32_t queue, void *table, const struct dppd_rule *rule,
    uintptr_t tag, void **output)
{
    struct native_async_flow *object;
    const struct rte_flow_op_attr attr = {.postpone = 1};
    struct rte_flow_error error = {0};
    int rc;
    (void)context;
    if (table == NULL)
        return -EINVAL;
    object = calloc(1, sizeof(*object));
    if (object == NULL)
        return -ENOMEM;
    rc = dppd_compile_flow(rule, NULL, &object->compiled);
    if (rc == 0) {
        /** 严格队列的 AGE 通知携带独立整数身份，不能借用 flow 指针或已释放的请求地址 */
        for (uint16_t i = 0; i < rule->nb_actions; ++i)
            if (rule->actions[i].type == DPPD_ACTION_AGE)
                object->compiled.age[i].context = (void *)tag;
        object->flow = rte_flow_async_create(port, queue, &attr, table, object->compiled.pattern, 0,
            object->compiled.actions, 0, (void *)tag, &error);
        if (object->flow == NULL)
            rc = rte_errno > 0 ? -rte_errno : -EIO;
    }
    if (rc != 0) {
        free(object);
        return rc;
    }
    *output = object;
    return 0;
}

static int native_destroy(void *context, uint16_t port, uint32_t queue, void *handle, uintptr_t tag)
{
    struct native_async_flow *object = handle;
    const struct rte_flow_op_attr attr = {.postpone = 1};
    struct rte_flow_error error = {0};
    (void)context;
    return rte_flow_async_destroy(port, queue, &attr, object->flow, (void *)tag, &error);
}

static int native_push(void *context, uint16_t port, uint32_t queue)
{
    struct rte_flow_error error = {0};
    (void)context;
    return rte_flow_push(port, queue, &error);
}

static int native_pull(void *context, uint16_t port, uint32_t queue, struct dppd_async_driver_result *results,
    uint16_t count)
{
    struct rte_flow_op_result completed[DPPD_ASYNC_FLOW_BATCH];
    struct rte_flow_error error = {0};
    int rc;
    (void)context;
    if (count == 0 || count > DPPD_ASYNC_FLOW_BATCH)
        return -EINVAL;
    rc = rte_flow_pull(port, queue, completed, count, &error);
    if (rc < 0 || rc > count)
        return rc;
    for (int i = 0; i < rc; ++i) {
        /** user_data 只还原不透明整数，不解引用驱动返回的任意指针 */
        results[i].tag = (uintptr_t)completed[i].user_data;
        results[i].status = completed[i].status == RTE_FLOW_OP_SUCCESS ? 0 : -EIO;
    }
    return rc;
}
static int native_aged(void *context, uint16_t port, uint32_t queue, uintptr_t *tags, uint32_t count)
{
    void *contexts[64];
    struct rte_flow_error error = {0};
    (void)context;
    if (count == 0 || count > 64)
        return -EINVAL;
    int rc = rte_flow_get_q_aged_flows(port, queue, contexts, count, &error);
    if (rc < 0 || rc > (int)count)
        return rc;
    for (int i = 0; i < rc; ++i)
        tags[i] = (uintptr_t)contexts[i];
    return rc;
}
#else
static int native_create(void *context, uint16_t port, uint32_t queue, void *table, const struct dppd_rule *rule,
    uintptr_t tag, void **output)
{
    (void)context;
    (void)port;
    (void)queue;
    (void)table;
    (void)rule;
    (void)tag;
    (void)output;
    return -ENOTSUP;
}

static int native_destroy(void *context, uint16_t port, uint32_t queue, void *handle, uintptr_t tag)
{
    (void)context;
    (void)port;
    (void)queue;
    (void)handle;
    (void)tag;
    return -ENOTSUP;
}

static int native_push(void *context, uint16_t port, uint32_t queue)
{
    (void)context;
    (void)port;
    (void)queue;
    return -ENOTSUP;
}

static int native_pull(void *context, uint16_t port, uint32_t queue, struct dppd_async_driver_result *results,
    uint16_t count)
{
    (void)context;
    (void)port;
    (void)queue;
    (void)results;
    (void)count;
    return -ENOTSUP;
}
static int native_aged(void *context, uint16_t port, uint32_t queue, uintptr_t *tags, uint32_t count)
{
    (void)context;
    (void)port;
    (void)queue;
    (void)tags;
    (void)count;
    return -ENOTSUP;
}
#endif

static void native_release(void *context, void *object)
{
    (void)context;
    free(object);
}

struct dppd_async_flow_driver dppd_native_async_flow_driver(void)
{
    return (struct dppd_async_flow_driver){.create = native_create,
        .destroy = native_destroy,
        .push = native_push,
        .pull = native_pull,
        .release = native_release,
        .aged = native_aged};
}
