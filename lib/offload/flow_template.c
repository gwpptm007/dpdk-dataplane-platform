/** 只在经过明确适配的版本使用实验 API，不把旧版本同步调用伪装成原生异步 */
#define ALLOW_EXPERIMENTAL_API
#include "dppd/flow_template.h"
#include "compiled_flow.h"

#include <errno.h>
#include <string.h>
#include <rte_errno.h>
#include <rte_version.h>

#if RTE_VERSION >= RTE_VERSION_NUM(24, 11, 0, 0)
static int driver_error(void)
{
    return rte_errno > 0 ? -rte_errno : -EIO;
}

int dppd_native_flow_configure(uint16_t port_id, uint16_t queues, uint32_t depth, uint32_t counters, uint32_t aging)
{
    struct rte_flow_port_info port = {0};
    struct rte_flow_queue_info queue = {0};
    struct rte_flow_port_attr attr = {.nb_counters = counters,
        .nb_aging_objects = aging,
        .flags = RTE_FLOW_PORT_FLAG_STRICT_QUEUE};
    struct rte_flow_queue_attr queue_attr = {.size = depth};
    const struct rte_flow_queue_attr *attributes[8];
    struct rte_flow_error error = {0};
    uint16_t i;
    int rc;

    if (queues == 0 || queues > 8 || depth == 0 || depth > 256)
        return -EINVAL;
    rc = rte_flow_info_get(port_id, &port, &queue, &error);
    if (rc != 0)
        return rc;
    if ((port.supported_flags & RTE_FLOW_PORT_FLAG_STRICT_QUEUE) == 0)
        return -ENOTSUP;
    if (queues > port.max_nb_queues || depth > queue.max_size || counters > port.max_nb_counters ||
        aging > port.max_nb_aging_objects)
        return -ENOSPC;
    for (i = 0; i < queues; ++i)
        attributes[i] = &queue_attr;
    return rte_flow_configure(port_id, &attr, queues, attributes, &error);
}

static int native_create(void *context, const struct dppd_flow_template_spec *spec,
    struct dppd_flow_template_parts *parts)
{
    struct dppd_compiled_flow compiled, masks;
    struct rte_flow_pattern_template_attr pattern_attr = {0};
    struct rte_flow_actions_template_attr actions_attr = {0};
    struct rte_flow_template_table_attr table_attr = {.nb_flows = spec->capacity};
    struct rte_flow_pattern_template *pattern;
    struct rte_flow_actions_template *actions;
    struct rte_flow_error error = {0};
    const uint16_t port = spec->example.install_port_id;
    uint16_t i;
    int rc;
    (void)context;

    if (spec->example.fallback == DPPD_FALLBACK_SOFTWARE_ONLY)
        return -ENOTSUP;
    rc = dppd_compile_flow(&spec->example, NULL, &compiled);
    if (rc != 0)
        return rc;
    /** 固定字段置全一掩码，可变字段保持零，COUNT 的每流身份始终由实际创建决定 */
    memset(&masks, 0, sizeof(masks));
    for (i = 0; i < spec->example.nb_actions; ++i) {
        const bool dynamic = (spec->dynamic_actions & (1U << i)) != 0;
        masks.actions[i].type = compiled.actions[i].type;
        switch (spec->example.actions[i].type) {
        case DPPD_ACTION_QUEUE:
            masks.queue[i].index = dynamic ? 0 : UINT16_MAX;
            masks.actions[i].conf = &masks.queue[i];
            break;
        case DPPD_ACTION_MARK:
            masks.mark[i].id = dynamic ? 0 : UINT32_MAX;
            masks.actions[i].conf = &masks.mark[i];
            break;
        case DPPD_ACTION_COUNT:
            masks.actions[i].conf = &masks.count[i];
            break;
        case DPPD_ACTION_AGE:
            masks.age[i].timeout = dynamic ? 0 : 0xffffffU;
            masks.actions[i].conf = &masks.age[i];
            break;
        case DPPD_ACTION_REPRESENTED_PORT:
            masks.ethdev_action[i].port_id = dynamic ? 0 : UINT16_MAX;
            masks.actions[i].conf = &masks.ethdev_action[i];
            break;
        case DPPD_ACTION_DROP:
            break;
        default:
            return -ENOTSUP;
        }
    }
    masks.actions[spec->example.nb_actions].type = RTE_FLOW_ACTION_TYPE_END;
    pattern_attr.ingress = actions_attr.ingress = compiled.attr.ingress;
    pattern_attr.egress = actions_attr.egress = compiled.attr.egress;
    pattern_attr.transfer = actions_attr.transfer = compiled.attr.transfer;
    parts->pattern = rte_flow_pattern_template_create(port, &pattern_attr, compiled.pattern, &error);
    if (parts->pattern == NULL)
        return driver_error();
    parts->actions = rte_flow_actions_template_create(port, &actions_attr, compiled.actions, masks.actions, &error);
    if (parts->actions == NULL)
        return driver_error();
    pattern = parts->pattern;
    actions = parts->actions;
    table_attr.flow_attr = compiled.attr;
    parts->table = rte_flow_template_table_create(port, &table_attr, &pattern, 1, &actions, 1, &error);
    return parts->table == NULL ? driver_error() : 0;
}

static int native_destroy(void *context, uint16_t port, struct dppd_flow_template_parts *parts)
{
    struct rte_flow_error error = {0};
    int rc;
    (void)context;

    /** 只有上层句柄销毁已确认成功才拆下层，失败保持原指针供精确重试 */
    if (parts->table != NULL) {
        rc = rte_flow_template_table_destroy(port, parts->table, &error);
        if (rc != 0)
            return rc;
        parts->table = NULL;
    }
    if (parts->actions != NULL) {
        rc = rte_flow_actions_template_destroy(port, parts->actions, &error);
        if (rc != 0)
            return rc;
        parts->actions = NULL;
    }
    if (parts->pattern != NULL) {
        rc = rte_flow_pattern_template_destroy(port, parts->pattern, &error);
        if (rc != 0)
            return rc;
        parts->pattern = NULL;
    }
    return 0;
}
#else
int dppd_native_flow_configure(uint16_t port_id, uint16_t queues, uint32_t depth, uint32_t counters, uint32_t aging)
{
    (void)port_id;
    (void)queues;
    (void)depth;
    (void)counters;
    (void)aging;
    return -ENOTSUP;
}

static int native_create(void *context, const struct dppd_flow_template_spec *spec,
    struct dppd_flow_template_parts *parts)
{
    (void)context;
    (void)spec;
    (void)parts;
    return -ENOTSUP;
}

static int native_destroy(void *context, uint16_t port, struct dppd_flow_template_parts *parts)
{
    (void)context;
    (void)port;
    (void)parts;
    return -ENOTSUP;
}
#endif

struct dppd_flow_template_driver dppd_native_flow_template_driver(void)
{
    return (struct dppd_flow_template_driver){.create = native_create, .destroy = native_destroy};
}
