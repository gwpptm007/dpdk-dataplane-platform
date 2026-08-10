#include <assert.h>
#include <errno.h>
#include <string.h>
#include "dppd/planner.h"

static struct dppd_rule ingress_rule(enum dppd_fallback_policy fallback)
{
    struct dppd_rule rule;

    memset(&rule, 0, sizeof(rule));
    rule.id = 2001;
    rule.generation = 7;
    rule.domain = DPPD_RULE_DOMAIN_INGRESS;
    rule.fallback = fallback;
    rule.nb_matches = 1;
    rule.matches[0].type = DPPD_MATCH_ETH;
    rule.nb_actions = 1;
    rule.actions[0].type = DPPD_ACTION_DROP;
    return rule;
}

static struct dppd_rule transfer_rule(void)
{
    struct dppd_rule rule;

    memset(&rule, 0, sizeof(rule));
    rule.id = 2002;
    rule.domain = DPPD_RULE_DOMAIN_TRANSFER;
    rule.fallback = DPPD_FALLBACK_REQUIRE_HARDWARE;
    rule.nb_matches = 1;
    rule.matches[0].type = DPPD_MATCH_REPRESENTED_PORT;
    rule.matches[0].spec.ethdev_port_id = 11;
    rule.nb_actions = 1;
    rule.actions[0].type = DPPD_ACTION_REPRESENTED_PORT;
    rule.actions[0].conf.ethdev_port_id = 12;
    return rule;
}

static struct dppd_topology topology(void)
{
    struct dppd_topology result;
    uint16_t i;

    memset(&result, 0, sizeof(result));
    result.nb_endpoints = 3;
    for (i = 0; i < result.nb_endpoints; ++i) {
        result.endpoints[i].ethdev_port_id = (uint16_t)(10 + i);
        result.endpoints[i].has_switch_domain = true;
        result.endpoints[i].switch_domain_id = 5;
    }
    return result;
}

int main(void)
{
    struct dppd_topology discovered = topology();
    struct dppd_planner_context context = {
        .topology = &discovered,
        .install_port_id = 10,
        .hardware_available = true,
        .software_equivalent = false,
    };
    struct dppd_execution_plan plan;
    struct dppd_rule rule = ingress_rule(DPPD_FALLBACK_PREFER_HARDWARE);

    assert(dppd_plan_rule(&context, &rule, &plan) == 0);
    assert(plan.backend == DPPD_PLAN_BACKEND_RTE_FLOW);
    assert(plan.requires_validation && !plan.fallback_used);
    assert(plan.reason == DPPD_PLAN_REASON_HARDWARE_SELECTED);
    assert(plan.rule_id == rule.id && plan.rule_generation == rule.generation);

    context.hardware_available = false;
    assert(dppd_plan_rule(&context, &rule, &plan) == -ENOTSUP);
    assert(plan.reason == DPPD_PLAN_REASON_NO_EQUIVALENT_FALLBACK);

    context.software_equivalent = true;
    assert(dppd_plan_rule(&context, &rule, &plan) == 0);
    assert(plan.backend == DPPD_PLAN_BACKEND_SOFTWARE);
    assert(plan.fallback_used);
    assert(plan.reason == DPPD_PLAN_REASON_SOFTWARE_FALLBACK);

    rule.fallback = DPPD_FALLBACK_REQUIRE_HARDWARE;
    assert(dppd_plan_rule(&context, &rule, &plan) == -ENOTSUP);
    assert(plan.reason == DPPD_PLAN_REASON_HARDWARE_UNAVAILABLE);

    rule.fallback = DPPD_FALLBACK_SOFTWARE_ONLY;
    assert(dppd_plan_rule(&context, &rule, &plan) == 0);
    assert(plan.backend == DPPD_PLAN_BACKEND_SOFTWARE);
    assert(!plan.fallback_used);
    assert(plan.reason == DPPD_PLAN_REASON_SOFTWARE_REQUESTED);

    context.software_equivalent = false;
    assert(dppd_plan_rule(&context, &rule, &plan) == -ENOTSUP);
    assert(plan.reason == DPPD_PLAN_REASON_SOFTWARE_UNAVAILABLE);

    rule = transfer_rule();
    context.hardware_available = true;
    assert(dppd_plan_rule(&context, &rule, &plan) == 0);
    assert(plan.backend == DPPD_PLAN_BACKEND_RTE_FLOW);

    discovered.endpoints[2].switch_domain_id = 6;
    assert(dppd_plan_rule(&context, &rule, &plan) == -EXDEV);
    assert(plan.reason == DPPD_PLAN_REASON_TRANSFER_DOMAIN_MISMATCH);

    discovered.endpoints[2].has_switch_domain = false;
    assert(dppd_plan_rule(&context, &rule, &plan) == -ENOTSUP);
    assert(plan.reason == DPPD_PLAN_REASON_TRANSFER_DOMAIN_MISSING);

    context.install_port_id = 99;
    assert(dppd_plan_rule(&context, &rule, &plan) == -ENOENT);
    assert(plan.reason == DPPD_PLAN_REASON_ENDPOINT_NOT_FOUND);
    assert(strcmp(dppd_plan_reason_name(plan.reason), "endpoint-not-found") == 0);
    return 0;
}
