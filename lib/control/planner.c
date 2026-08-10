#include "dppd/planner.h"

#include <errno.h>
#include <string.h>

static int reject(struct dppd_execution_plan *plan,
                  enum dppd_plan_reason reason,
                  int error)
{
    plan->reason = reason;
    return error;
}

static int validate_transfer_domain(const struct dppd_planner_context *context,
                                    const struct dppd_rule *rule,
                                    struct dppd_execution_plan *plan)
{
    const struct dppd_endpoint *install_endpoint;
    uint16_t domain_id;
    uint16_t i;

    install_endpoint = dppd_topology_find(context->topology,
                                          context->install_port_id);
    if (install_endpoint == NULL)
        return reject(plan, DPPD_PLAN_REASON_ENDPOINT_NOT_FOUND, -ENOENT);
    if (!install_endpoint->has_switch_domain)
        return reject(plan, DPPD_PLAN_REASON_TRANSFER_DOMAIN_MISSING, -ENOTSUP);
    domain_id = install_endpoint->switch_domain_id;

    for (i = 0; i < rule->nb_matches; ++i) {
        const struct dppd_endpoint *endpoint;

        if (rule->matches[i].type != DPPD_MATCH_REPRESENTED_PORT)
            continue;
        endpoint = dppd_topology_find(context->topology,
                                      rule->matches[i].spec.ethdev_port_id);
        if (endpoint == NULL)
            return reject(plan, DPPD_PLAN_REASON_ENDPOINT_NOT_FOUND, -ENOENT);
        if (!endpoint->has_switch_domain)
            return reject(plan, DPPD_PLAN_REASON_TRANSFER_DOMAIN_MISSING,
                          -ENOTSUP);
        if (endpoint->switch_domain_id != domain_id)
            return reject(plan, DPPD_PLAN_REASON_TRANSFER_DOMAIN_MISMATCH,
                          -EXDEV);
    }
    for (i = 0; i < rule->nb_actions; ++i) {
        const struct dppd_endpoint *endpoint;

        if (rule->actions[i].type != DPPD_ACTION_REPRESENTED_PORT)
            continue;
        endpoint = dppd_topology_find(context->topology,
                                      rule->actions[i].conf.ethdev_port_id);
        if (endpoint == NULL)
            return reject(plan, DPPD_PLAN_REASON_ENDPOINT_NOT_FOUND, -ENOENT);
        if (!endpoint->has_switch_domain)
            return reject(plan, DPPD_PLAN_REASON_TRANSFER_DOMAIN_MISSING,
                          -ENOTSUP);
        if (endpoint->switch_domain_id != domain_id)
            return reject(plan, DPPD_PLAN_REASON_TRANSFER_DOMAIN_MISMATCH,
                          -EXDEV);
    }
    return 0;
}

int dppd_plan_rule(const struct dppd_planner_context *context,
                   const struct dppd_rule *rule,
                   struct dppd_execution_plan *plan)
{
    char validation_error[128];
    int rc;

    if (context == NULL || context->topology == NULL ||
        rule == NULL || plan == NULL)
        return -EINVAL;
    memset(plan, 0, sizeof(*plan));
    plan->rule_id = rule->id;
    plan->rule_generation = rule->generation;
    plan->install_port_id = context->install_port_id;
    if (dppd_rule_validate(rule, validation_error, sizeof(validation_error)) != 0)
        return reject(plan, DPPD_PLAN_REASON_INVALID_RULE, -EINVAL);

    if (dppd_topology_find(context->topology, context->install_port_id) == NULL)
        return reject(plan, DPPD_PLAN_REASON_ENDPOINT_NOT_FOUND, -ENOENT);

    if (rule->fallback == DPPD_FALLBACK_SOFTWARE_ONLY) {
        if (!context->software_equivalent)
            return reject(plan, DPPD_PLAN_REASON_SOFTWARE_UNAVAILABLE,
                          -ENOTSUP);
        if (rule->domain == DPPD_RULE_DOMAIN_TRANSFER)
            return reject(plan, DPPD_PLAN_REASON_NO_EQUIVALENT_FALLBACK,
                          -ENOTSUP);
        plan->backend = DPPD_PLAN_BACKEND_SOFTWARE;
        plan->reason = DPPD_PLAN_REASON_SOFTWARE_REQUESTED;
        return 0;
    }

    if (context->hardware_available) {
        /* hardware_available 仅代表存在候选 backend，真实支持仍需 validate。 */
        if (rule->domain == DPPD_RULE_DOMAIN_TRANSFER) {
            rc = validate_transfer_domain(context, rule, plan);
            if (rc != 0)
                return rc;
        }
        plan->backend = DPPD_PLAN_BACKEND_RTE_FLOW;
        plan->reason = DPPD_PLAN_REASON_HARDWARE_SELECTED;
        plan->requires_validation = true;
        return 0;
    }

    if (rule->fallback == DPPD_FALLBACK_REQUIRE_HARDWARE)
        return reject(plan, DPPD_PLAN_REASON_HARDWARE_UNAVAILABLE, -ENOTSUP);
    if (!context->software_equivalent)
        /* 没有等价软件语义时，PREFER_HARDWARE 也不能静默降级。 */
        return reject(plan, DPPD_PLAN_REASON_NO_EQUIVALENT_FALLBACK, -ENOTSUP);
    if (rule->domain == DPPD_RULE_DOMAIN_TRANSFER)
        return reject(plan, DPPD_PLAN_REASON_NO_EQUIVALENT_FALLBACK, -ENOTSUP);

    plan->backend = DPPD_PLAN_BACKEND_SOFTWARE;
    plan->reason = DPPD_PLAN_REASON_SOFTWARE_FALLBACK;
    plan->fallback_used = true;
    return 0;
}

const char *dppd_plan_reason_name(enum dppd_plan_reason reason)
{
    switch (reason) {
    case DPPD_PLAN_REASON_NONE:
        return "none";
    case DPPD_PLAN_REASON_SOFTWARE_REQUESTED:
        return "software-requested";
    case DPPD_PLAN_REASON_HARDWARE_SELECTED:
        return "hardware-selected";
    case DPPD_PLAN_REASON_HARDWARE_UNAVAILABLE:
        return "hardware-unavailable";
    case DPPD_PLAN_REASON_SOFTWARE_FALLBACK:
        return "software-fallback";
    case DPPD_PLAN_REASON_SOFTWARE_UNAVAILABLE:
        return "software-unavailable";
    case DPPD_PLAN_REASON_NO_EQUIVALENT_FALLBACK:
        return "no-equivalent-fallback";
    case DPPD_PLAN_REASON_ENDPOINT_NOT_FOUND:
        return "endpoint-not-found";
    case DPPD_PLAN_REASON_TRANSFER_DOMAIN_MISSING:
        return "transfer-domain-missing";
    case DPPD_PLAN_REASON_TRANSFER_DOMAIN_MISMATCH:
        return "transfer-domain-mismatch";
    case DPPD_PLAN_REASON_INVALID_RULE:
        return "invalid-rule";
    }
    return "unknown";
}
