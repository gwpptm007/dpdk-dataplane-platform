#include <assert.h>
#include <errno.h>
#include <string.h>
#include "dppd/planner.h"
#include "dppd/software_backend.h"

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

/** QUEUE 的 RX 分流不能由 TX 队列冒充，三种策略和软件强制模式都不得静默模拟 */
static void test_queue_semantics(void)
{
    struct dppd_topology discovered = topology();
    for (unsigned int feature = 0; feature < 3; ++feature)
        for (unsigned int hardware = 0; hardware < 2; ++hardware)
            for (enum dppd_fallback_policy policy = DPPD_FALLBACK_REQUIRE_HARDWARE;
                 policy <= DPPD_FALLBACK_SOFTWARE_ONLY; ++policy) {
                struct dppd_rule rule = ingress_rule(policy);
                rule.install_port_id = 10;
                rule.actions[0].type = DPPD_ACTION_QUEUE;
                rule.actions[0].conf.queue_id = 1;
                struct dppd_planner_context context = {.topology = &discovered,
                    .install_port_id = 10,
                    .hardware_available = hardware != 0,
                    .software_equivalent = dppd_software_backend_rule_supported(&rule),
                    .connection_tracking = feature == 1,
                    .neighbor_discovery = feature == 2};
                assert(!context.software_equivalent);
                struct dppd_execution_plan plan;
                int rc = dppd_plan_rule(&context, &rule, &plan);
                if (feature == 0 && hardware && policy != DPPD_FALLBACK_SOFTWARE_ONLY) {
                    assert(rc == 0 && plan.backend == DPPD_PLAN_BACKEND_RTE_FLOW && plan.requires_validation);
                    assert(!plan.fallback_used);
                } else
                    assert(rc == -ENOTSUP && !plan.fallback_used);
            }
}

/** 即使调用方误报软件等价，AGE 也不能变成普通 DROP 或依赖 COUNT 猜测空闲状态 */
static void test_age_semantics(void)
{
    struct dppd_topology discovered = topology();
    for (unsigned int counted = 0; counted < 2; ++counted)
        for (unsigned int feature = 0; feature < 5; ++feature)
            for (unsigned int hardware = 0; hardware < 2; ++hardware)
                for (enum dppd_fallback_policy policy = DPPD_FALLBACK_REQUIRE_HARDWARE;
                     policy <= DPPD_FALLBACK_SOFTWARE_ONLY; ++policy) {
                    struct dppd_rule rule = ingress_rule(policy);
                    rule.install_port_id = 10;
                    rule.nb_actions = counted ? 3 : 2;
                    if (counted)
                        rule.actions[0].type = DPPD_ACTION_COUNT;
                    rule.actions[counted].type = DPPD_ACTION_AGE;
                    rule.actions[counted].conf.age_timeout_seconds = 60;
                    rule.actions[counted + 1U].type = DPPD_ACTION_DROP;
                    if (feature == 3) {
                        rule.nb_matches = 2;
                        rule.matches[1].type = DPPD_MATCH_IPV6;
                    }
                    struct dppd_planner_context context = {.topology = &discovered,
                        .install_port_id = 10,
                        .hardware_available = hardware != 0,
                        .software_equivalent = true,
                        .connection_tracking = feature == 1 || feature == 4,
                        .neighbor_discovery = feature == 2 || feature == 4};
                    assert(!dppd_software_backend_rule_supported(&rule));
                    struct dppd_execution_plan plan;
                    int rc = dppd_plan_rule(&context, &rule, &plan);
                    if (feature == 0 && hardware && policy != DPPD_FALLBACK_SOFTWARE_ONLY) {
                        assert(rc == 0 && plan.backend == DPPD_PLAN_BACKEND_RTE_FLOW && plan.requires_validation);
                        assert(plan.reason == DPPD_PLAN_REASON_HARDWARE_SELECTED && !plan.fallback_used);
                        assert(plan.rule_id == rule.id && plan.rule_generation == rule.generation &&
                            plan.install_port_id == 10);
                    } else {
                        assert(rc == -ENOTSUP && !plan.fallback_used && !plan.requires_validation);
                        enum dppd_plan_reason expected = feature != 0  ? DPPD_PLAN_REASON_NO_EQUIVALENT_FALLBACK
                            : policy == DPPD_FALLBACK_SOFTWARE_ONLY    ? DPPD_PLAN_REASON_SOFTWARE_UNAVAILABLE
                            : policy == DPPD_FALLBACK_REQUIRE_HARDWARE ? DPPD_PLAN_REASON_HARDWARE_UNAVAILABLE
                                                                       : DPPD_PLAN_REASON_NO_EQUIVALENT_FALLBACK;
                        assert(plan.reason == expected);
                    }
                }

    /** 有明确交换域的 transfer 仍可选择硬件，但没有硬件时绝不能退成软件 */
    struct dppd_rule rule = transfer_rule();
    rule.nb_actions = 2;
    rule.actions[1] = rule.actions[0];
    rule.actions[0].type = DPPD_ACTION_AGE;
    rule.actions[0].conf.age_timeout_seconds = 0xffffffU;
    struct dppd_planner_context context = {.topology = &discovered,
        .install_port_id = 10,
        .hardware_available = true,
        .software_equivalent = true};
    struct dppd_execution_plan plan;
    assert(!dppd_software_backend_rule_supported(&rule));
    assert(dppd_plan_rule(&context, &rule, &plan) == 0 && plan.backend == DPPD_PLAN_BACKEND_RTE_FLOW &&
        plan.requires_validation && !plan.fallback_used);
    rule.fallback = DPPD_FALLBACK_PREFER_HARDWARE;
    context.hardware_available = false;
    assert(dppd_plan_rule(&context, &rule, &plan) == -ENOTSUP && !plan.fallback_used);
    assert(plan.reason == DPPD_PLAN_REASON_NO_EQUIVALENT_FALLBACK);
}

/** 共享对象不能由软件私有计数冒充，即使外部错误声明等价也要坚持驱动路径 */
static void test_shared_semantics(void)
{
    struct dppd_topology discovered = topology();
    for (unsigned int feature = 0; feature < 4; ++feature)
        for (unsigned int hardware = 0; hardware < 2; ++hardware)
            for (enum dppd_fallback_policy policy = DPPD_FALLBACK_REQUIRE_HARDWARE;
                 policy <= DPPD_FALLBACK_SOFTWARE_ONLY; ++policy) {
                struct dppd_rule rule = ingress_rule(policy);
                rule.nb_actions = 2;
                rule.actions[1] = rule.actions[0];
                rule.actions[0].type = DPPD_ACTION_SHARED_COUNT;
                rule.actions[0].conf.shared_count.id = 90;
                rule.actions[0].conf.shared_count.generation = 7;
                struct dppd_planner_context context = {.topology = &discovered,
                    .install_port_id = 10,
                    .hardware_available = hardware != 0,
                    .software_equivalent = true,
                    .connection_tracking = feature == 1,
                    .neighbor_discovery = feature == 2};
                if (feature == 3)
                    rule.matches[0].type = DPPD_MATCH_IPV6;
                assert(!dppd_software_backend_rule_supported(&rule));
                struct dppd_execution_plan plan;
                int rc = dppd_plan_rule(&context, &rule, &plan);
                if (feature == 0 && hardware && policy != DPPD_FALLBACK_SOFTWARE_ONLY)
                    assert(rc == 0 && plan.backend == DPPD_PLAN_BACKEND_RTE_FLOW && plan.requires_validation);
                else
                    assert(rc == -ENOTSUP && !plan.fallback_used);
            }
}

int main(void)
{
    test_queue_semantics();
    test_age_semantics();
    test_shared_semantics();
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

    /** 三层转发必须经过软件改写，交换域直转即便硬件支持也不能绕过 worker */
    context.ipv4_routing = true;
    assert(dppd_plan_rule(&context, &rule, &plan) == -ENOTSUP);
    assert(plan.reason == DPPD_PLAN_REASON_NO_EQUIVALENT_FALLBACK);
    context.ipv4_routing = false;

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
    /** 即便硬件可安装入口规则，启用 ND 后也必须让本机控制报文到达 worker */
    context.install_port_id = 10;
    context.software_equivalent = true;
    context.hardware_available = true;
    context.neighbor_discovery = true;
    rule = ingress_rule(DPPD_FALLBACK_PREFER_HARDWARE);
    assert(dppd_plan_rule(&context, &rule, &plan) == 0 && plan.backend == DPPD_PLAN_BACKEND_SOFTWARE);
    rule.fallback = DPPD_FALLBACK_REQUIRE_HARDWARE;
    assert(dppd_plan_rule(&context, &rule, &plan) == -ENOTSUP);
    return 0;
}
