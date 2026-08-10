#include "dppd/offload.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <rte_errno.h>
#include <rte_flow.h>

struct compiled_flow {
    struct rte_flow_attr attr;
    struct rte_flow_item pattern[DPPD_RULE_MAX_ITEMS + 1U];
    struct rte_flow_item_ipv4 ipv4_spec[DPPD_RULE_MAX_ITEMS];
    struct rte_flow_item_ipv4 ipv4_mask[DPPD_RULE_MAX_ITEMS];
    struct rte_flow_item_udp udp_spec[DPPD_RULE_MAX_ITEMS];
    struct rte_flow_item_udp udp_mask[DPPD_RULE_MAX_ITEMS];
    struct rte_flow_item_tcp tcp_spec[DPPD_RULE_MAX_ITEMS];
    struct rte_flow_item_tcp tcp_mask[DPPD_RULE_MAX_ITEMS];
    struct rte_flow_item_ethdev ethdev_spec[DPPD_RULE_MAX_ITEMS];
    struct rte_flow_item_ethdev ethdev_mask[DPPD_RULE_MAX_ITEMS];
    struct rte_flow_action actions[DPPD_RULE_MAX_ACTIONS + 1U];
    struct rte_flow_action_queue queue[DPPD_RULE_MAX_ACTIONS];
    struct rte_flow_action_mark mark[DPPD_RULE_MAX_ACTIONS];
    struct rte_flow_action_count count[DPPD_RULE_MAX_ACTIONS];
    struct rte_flow_action_ethdev ethdev_action[DPPD_RULE_MAX_ACTIONS];
};

static int report_error(struct dppd_flow_error *output,
                        int code,
                        const char *operation,
                        const struct rte_flow_error *flow_error)
{
    const char *detail = NULL;

    if (flow_error != NULL)
        detail = flow_error->message;
    if (detail == NULL && code != 0)
        detail = rte_strerror(code < 0 ? -code : code);
    if (detail == NULL)
        detail = "unknown error";
    if (output != NULL) {
        output->code = code;
        snprintf(output->message, sizeof(output->message), "%s: %s", operation, detail);
    }
    return code;
}

static void clear_error(struct dppd_flow_error *error)
{
    if (error != NULL)
        memset(error, 0, sizeof(*error));
}

static int compile_match(const struct dppd_match *match,
                         uint16_t index,
                         struct compiled_flow *compiled)
{
    struct rte_flow_item *item = &compiled->pattern[index];

    switch (match->type) {
    case DPPD_MATCH_ETH:
        item->type = RTE_FLOW_ITEM_TYPE_ETH;
        return 0;
    case DPPD_MATCH_IPV4:
        compiled->ipv4_spec[index].hdr.src_addr = match->spec.ipv4.src_be;
        compiled->ipv4_spec[index].hdr.dst_addr = match->spec.ipv4.dst_be;
        compiled->ipv4_mask[index].hdr.src_addr = match->spec.ipv4.src_mask_be;
        compiled->ipv4_mask[index].hdr.dst_addr = match->spec.ipv4.dst_mask_be;
        item->type = RTE_FLOW_ITEM_TYPE_IPV4;
        item->spec = &compiled->ipv4_spec[index];
        item->mask = &compiled->ipv4_mask[index];
        return 0;
    case DPPD_MATCH_UDP:
        compiled->udp_spec[index].hdr.src_port = match->spec.l4.src_be;
        compiled->udp_spec[index].hdr.dst_port = match->spec.l4.dst_be;
        compiled->udp_mask[index].hdr.src_port = match->spec.l4.src_mask_be;
        compiled->udp_mask[index].hdr.dst_port = match->spec.l4.dst_mask_be;
        item->type = RTE_FLOW_ITEM_TYPE_UDP;
        item->spec = &compiled->udp_spec[index];
        item->mask = &compiled->udp_mask[index];
        return 0;
    case DPPD_MATCH_TCP:
        compiled->tcp_spec[index].hdr.src_port = match->spec.l4.src_be;
        compiled->tcp_spec[index].hdr.dst_port = match->spec.l4.dst_be;
        compiled->tcp_mask[index].hdr.src_port = match->spec.l4.src_mask_be;
        compiled->tcp_mask[index].hdr.dst_port = match->spec.l4.dst_mask_be;
        item->type = RTE_FLOW_ITEM_TYPE_TCP;
        item->spec = &compiled->tcp_spec[index];
        item->mask = &compiled->tcp_mask[index];
        return 0;
    case DPPD_MATCH_REPRESENTED_PORT:
        compiled->ethdev_spec[index].port_id = match->spec.ethdev_port_id;
        compiled->ethdev_mask[index].port_id = UINT16_MAX;
        item->type = RTE_FLOW_ITEM_TYPE_REPRESENTED_PORT;
        item->spec = &compiled->ethdev_spec[index];
        item->mask = &compiled->ethdev_mask[index];
        return 0;
    default:
        return -ENOTSUP;
    }
}

static int compile_action(const struct dppd_action *source,
                          uint16_t index,
                          uint64_t rule_id,
                          struct compiled_flow *compiled)
{
    struct rte_flow_action *action = &compiled->actions[index];

    switch (source->type) {
    case DPPD_ACTION_DROP:
        action->type = RTE_FLOW_ACTION_TYPE_DROP;
        return 0;
    case DPPD_ACTION_QUEUE:
        compiled->queue[index].index = source->conf.queue_id;
        action->type = RTE_FLOW_ACTION_TYPE_QUEUE;
        action->conf = &compiled->queue[index];
        return 0;
    case DPPD_ACTION_MARK:
        compiled->mark[index].id = source->conf.mark_id;
        action->type = RTE_FLOW_ACTION_TYPE_MARK;
        action->conf = &compiled->mark[index];
        return 0;
    case DPPD_ACTION_COUNT:
        compiled->count[index].id = (uint32_t)rule_id;
        action->type = RTE_FLOW_ACTION_TYPE_COUNT;
        action->conf = &compiled->count[index];
        return 0;
    case DPPD_ACTION_REPRESENTED_PORT:
        compiled->ethdev_action[index].port_id = source->conf.ethdev_port_id;
        action->type = RTE_FLOW_ACTION_TYPE_REPRESENTED_PORT;
        action->conf = &compiled->ethdev_action[index];
        return 0;
    default:
        return -ENOTSUP;
    }
}

static int compile_rule(const struct dppd_rule *rule, struct compiled_flow *compiled)
{
    uint16_t i;
    int rc;

    memset(compiled, 0, sizeof(*compiled));
    compiled->attr.group = rule->group;
    compiled->attr.priority = rule->priority;
    if (rule->domain == DPPD_RULE_DOMAIN_INGRESS)
        compiled->attr.ingress = 1;
    else if (rule->domain == DPPD_RULE_DOMAIN_EGRESS)
        compiled->attr.egress = 1;
    else
        compiled->attr.transfer = 1;

    for (i = 0; i < rule->nb_matches; ++i) {
        rc = compile_match(&rule->matches[i], i, compiled);
        if (rc != 0)
            return rc;
    }
    compiled->pattern[rule->nb_matches].type = RTE_FLOW_ITEM_TYPE_END;

    for (i = 0; i < rule->nb_actions; ++i) {
        rc = compile_action(&rule->actions[i], i, rule->id, compiled);
        if (rc != 0)
            return rc;
    }
    compiled->actions[rule->nb_actions].type = RTE_FLOW_ACTION_TYPE_END;
    return 0;
}

static int validate_rule(const struct dppd_rule *rule,
                         struct dppd_flow_error *error,
                         const char *operation)
{
    char validation_error[256];

    if (rule == NULL)
        return report_error(error, -EINVAL, operation, NULL);
    if (rule->fallback == DPPD_FALLBACK_SOFTWARE_ONLY)
        return report_error(error, -ENOTSUP,
                            "use software-only rule in hardware", NULL);
    if (dppd_rule_validate(rule, validation_error, sizeof(validation_error)) != 0) {
        if (error != NULL) {
            error->code = -EINVAL;
            snprintf(error->message, sizeof(error->message),
                     "invalid rule: %.241s", validation_error);
        }
        return -EINVAL;
    }
    return 0;
}

int dppd_flow_validate(uint16_t port_id,
                       const struct dppd_rule *rule,
                       struct dppd_flow_error *error)
{
    struct compiled_flow compiled;
    struct rte_flow_error flow_error;
    int rc;

    clear_error(error);
    rc = validate_rule(rule, error, "validate");
    if (rc != 0)
        return rc;
    rc = compile_rule(rule, &compiled);
    if (rc != 0)
        return report_error(error, rc, "compile", NULL);

    memset(&flow_error, 0, sizeof(flow_error));
    rc = rte_flow_validate(port_id,
                           &compiled.attr,
                           compiled.pattern,
                           compiled.actions,
                           &flow_error);
    if (rc != 0)
        return report_error(error, rc, "validate", &flow_error);
    return 0;
}

int dppd_flow_create(uint16_t port_id,
                     const struct dppd_rule *rule,
                     struct dppd_flow_handle *handle,
                     struct dppd_flow_error *error)
{
    struct compiled_flow compiled;
    struct rte_flow_error flow_error;
    struct rte_flow *flow;
    uint16_t i;
    int rc;

    clear_error(error);
    if (handle == NULL)
        return report_error(error, -EINVAL, "create", NULL);
    memset(handle, 0, sizeof(*handle));
    rc = validate_rule(rule, error, "create");
    if (rc != 0)
        return rc;
    rc = compile_rule(rule, &compiled);
    if (rc != 0)
        return report_error(error, rc, "compile", NULL);

    memset(&flow_error, 0, sizeof(flow_error));
    flow = rte_flow_create(port_id,
                           &compiled.attr,
                           compiled.pattern,
                           compiled.actions,
                           &flow_error);
    if (flow == NULL) {
        rc = rte_errno != 0 ? -rte_errno : -EIO;
        return report_error(error, rc, "create", &flow_error);
    }

    handle->rule_id = rule->id;
    handle->rule_generation = rule->generation;
    handle->port_id = port_id;
    handle->flow = flow;
    for (i = 0; i < rule->nb_actions; ++i) {
        if (rule->actions[i].type == DPPD_ACTION_COUNT) {
            handle->has_count = true;
            break;
        }
    }
    return 0;
}

int dppd_flow_install(uint16_t port_id,
                      const struct dppd_rule *rule,
                      struct dppd_flow_handle *handle,
                      struct dppd_flow_error *error)
{
    int rc;

    rc = dppd_flow_validate(port_id, rule, error);
    if (rc != 0)
        return rc;
    return dppd_flow_create(port_id, rule, handle, error);
}

int dppd_flow_remove(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    struct rte_flow_error flow_error;
    int rc;

    clear_error(error);
    if (handle == NULL || handle->flow == NULL)
        return report_error(error, -EINVAL, "destroy", NULL);
    memset(&flow_error, 0, sizeof(flow_error));
    rc = rte_flow_destroy(handle->port_id, handle->flow, &flow_error);
    if (rc != 0)
        return report_error(error, rc, "destroy", &flow_error);
    memset(handle, 0, sizeof(*handle));
    return 0;
}

int dppd_flow_query_count(const struct dppd_flow_handle *handle,
                          uint64_t *hits,
                          uint64_t *bytes,
                          struct dppd_flow_error *error)
{
    const struct rte_flow_action action = {
        .type = RTE_FLOW_ACTION_TYPE_COUNT,
    };
    struct rte_flow_query_count query;
    struct rte_flow_error flow_error;
    int rc;

    clear_error(error);
    if (handle == NULL || handle->flow == NULL || hits == NULL || bytes == NULL)
        return report_error(error, -EINVAL, "query count", NULL);
    if (!handle->has_count)
        return report_error(error, -ENOENT, "query count action", NULL);

    memset(&query, 0, sizeof(query));
    memset(&flow_error, 0, sizeof(flow_error));
    rc = rte_flow_query(handle->port_id, handle->flow, &action, &query, &flow_error);
    if (rc != 0)
        return report_error(error, rc, "query count", &flow_error);
    *hits = query.hits_set ? query.hits : 0;
    *bytes = query.bytes_set ? query.bytes : 0;
    return 0;
}

int dppd_flow_flush(uint16_t port_id, struct dppd_flow_error *error)
{
    struct rte_flow_error flow_error;
    int rc;

    clear_error(error);
    memset(&flow_error, 0, sizeof(flow_error));
    rc = rte_flow_flush(port_id, &flow_error);
    if (rc != 0)
        return report_error(error, rc, "flush", &flow_error);
    return 0;
}
