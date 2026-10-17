#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dppd/rte_flow_backend.h"

static unsigned int queries, removals;
static int query_error, remove_error;
static bool seconds_valid = true;

static struct dppd_rule make_rule(uint64_t generation, bool age)
{
    struct dppd_rule rule = {.id = 100,
        .generation = generation,
        .install_port_id = 7,
        .domain = DPPD_RULE_DOMAIN_INGRESS,
        .fallback = DPPD_FALLBACK_REQUIRE_HARDWARE,
        .nb_matches = 1,
        .nb_actions = age ? 2 : 1,
        .matches = {{.type = DPPD_MATCH_ETH}}};
    if (age)
        rule.actions[0] = (struct dppd_action){.type = DPPD_ACTION_AGE, .conf.age_timeout_seconds = 13};
    rule.actions[rule.nb_actions - 1].type = DPPD_ACTION_DROP;
    return rule;
}

/** AGE 是附加观察动作，不能抢占最终处理动作，也不能把超时截成零或低 24 位 */
static void rule_contract(void)
{
    struct dppd_rule rule = make_rule(10, true), changed = rule;
    char reason[128];
    assert(dppd_rule_validate(&rule, reason, sizeof(reason)) == 0);
    changed.generation++;
    assert(dppd_rule_equal(&rule, &changed));
    changed.actions[0].conf.age_timeout_seconds++;
    assert(!dppd_rule_equal(&rule, &changed));
    changed = rule;
    changed.actions[0].conf.age_timeout_seconds = 1;
    assert(dppd_rule_validate(&changed, reason, sizeof(reason)) == 0);
    changed.actions[0].conf.age_timeout_seconds = 0xffffffU;
    assert(dppd_rule_validate(&changed, reason, sizeof(reason)) == 0);
    const uint32_t invalid[] = {0, 0x1000000U, UINT32_MAX};
    for (unsigned int i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        changed.actions[0].conf.age_timeout_seconds = invalid[i];
        assert(dppd_rule_validate(&changed, reason, sizeof(reason)) != 0);
    }
    changed = rule;
    changed.actions[2] = changed.actions[1];
    changed.actions[1] = changed.actions[0];
    changed.nb_actions = 3;
    assert(dppd_rule_validate(&changed, reason, sizeof(reason)) != 0);
    changed = rule;
    changed.actions[0] = rule.actions[1];
    changed.actions[1] = rule.actions[0];
    assert(dppd_rule_validate(&changed, reason, sizeof(reason)) != 0);
    changed = rule;
    changed.nb_actions = 1;
    assert(dppd_rule_validate(&changed, reason, sizeof(reason)) != 0);
}

static int fake_validate(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_error *error)
{
    (void)error;
    char reason[128];
    assert(port == 7 && dppd_rule_validate(rule, reason, sizeof(reason)) == 0);
    return 0;
}

static int fake_create(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_handle *handle,
    struct dppd_flow_error *error)
{
    (void)error;
    *handle = (struct dppd_flow_handle){.rule_id = rule->id,
        .rule_generation = rule->generation,
        .port_id = port,
        .flow = (struct rte_flow *)(uintptr_t)rule->generation};
    for (uint16_t i = 0; i < rule->nb_actions; ++i)
        handle->has_age |= rule->actions[i].type == DPPD_ACTION_AGE;
    return 0;
}

static int fake_remove(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    removals++;
    if (remove_error != 0)
        return remove_error;
    memset(handle, 0, sizeof(*handle));
    return 0;
}

/** 即使接口在返回错误前写坏结果，后端也必须保留调用方上次有效的答复 */
static int fake_query(const struct dppd_flow_handle *handle, struct dppd_flow_age_result *result,
    struct dppd_flow_error *error)
{
    queries++;
    *result = (struct dppd_flow_age_result){.aged = handle->rule_generation == 10,
        .sec_since_last_hit_valid = seconds_valid,
        .sec_since_last_hit = (uint32_t)handle->rule_generation};
    if (error != NULL) {
        error->code = query_error;
        snprintf(error->message, sizeof(error->message), "simulated AGE query");
    }
    return query_error;
}

static struct dppd_transaction_item item_for(uint64_t generation, bool age)
{
    return (struct dppd_transaction_item){.rule = make_rule(generation, age),
        .plan = {.rule_id = 100,
            .rule_generation = generation,
            .install_port_id = 7,
            .backend = DPPD_PLAN_BACKEND_RTE_FLOW,
            .requires_validation = true}};
}

static void install(struct dppd_rte_flow_backend *backend, uint64_t generation, bool age)
{
    struct dppd_transaction_item item = item_for(generation, age);
    struct dppd_transaction transaction;
    struct dppd_transaction_backends backends = {.rte_flow = dppd_rte_flow_transaction_backend(backend)};
    assert(dppd_transaction_init(&transaction, generation, &item, 1) == 0);
    assert(dppd_transaction_run(&transaction, &backends) == 0);
    assert(dppd_transaction_finalize(&transaction, &backends) == 0);
}

static void preserved(const struct dppd_flow_age_result *result)
{
    assert(result->aged && result->sec_since_last_hit_valid && result->sec_since_last_hit == 17);
}

/** 新旧版本同时存在时按精确版本查询，未安装槽位、缺动作和缺接口都有独立错误 */
static void backend_contract(void)
{
    const struct dppd_flow_api api = {.validate = fake_validate,
        .create = fake_create,
        .remove = fake_remove,
        .query_age = fake_query};
    struct dppd_rte_flow_backend backend;
    struct dppd_flow_error error;
    struct dppd_flow_age_result result = {.aged = true, .sec_since_last_hit_valid = true, .sec_since_last_hit = 17};
    assert(dppd_rte_flow_backend_init(&backend, 5, NULL) == 0);
    assert(backend.api.query_age == dppd_flow_query_age);
    assert(dppd_rte_flow_backend_fini(&backend) == 0);
    assert(dppd_rte_flow_backend_init(&backend, 5, &api) == 0);
    install(&backend, 10, true);
    install(&backend, 11, true);
    install(&backend, 12, false);
    struct dppd_transaction_backend operations = dppd_rte_flow_transaction_backend(&backend);
    struct dppd_transaction_item pending = item_for(13, true);
    uintptr_t token;
    assert(operations.validate(operations.context, &pending) == 0);
    assert(operations.prepare(operations.context, &pending, &token) == 0);
    assert(dppd_rte_flow_backend_query_age(&backend, 100, 13, &result, &error) == -ENOENT);
    preserved(&result);
    assert(operations.rollback(operations.context, &pending, token, false) == 0);
    assert(dppd_rte_flow_backend_query_age(&backend, 100, 12, &result, &error) == -ENODATA);
    assert(dppd_rte_flow_backend_query_age(&backend, 100, 99, &result, &error) == -ENOENT);
    assert(dppd_rte_flow_backend_query_age(&backend, 100, 0, &result, &error) == -EINVAL);
    assert(dppd_rte_flow_backend_query_age(&backend, 100, UINT64_MAX, &result, &error) == -EINVAL);
    assert(dppd_rte_flow_backend_query_age(NULL, 100, 10, &result, &error) == -EINVAL);
    preserved(&result);
    assert(queries == 0);
    backend.api.query_age = NULL;
    assert(dppd_rte_flow_backend_query_age(&backend, 100, 10, &result, &error) == -ENOTSUP);
    preserved(&result);
    backend.api.query_age = fake_query;
    query_error = -EIO;
    assert(dppd_rte_flow_backend_query_age(&backend, 100, 10, &result, &error) == -EIO);
    preserved(&result);
    assert(error.code == -EIO && strstr(error.message, "simulated AGE query") != NULL);
    query_error = 0;
    assert(dppd_rte_flow_backend_query_age(&backend, 100, 10, &result, &error) == 0);
    assert(result.aged && result.sec_since_last_hit_valid && result.sec_since_last_hit == 10);
    assert(dppd_rte_flow_backend_query_age(&backend, 100, 11, &result, &error) == 0);
    assert(!result.aged && result.sec_since_last_hit == 11);
    seconds_valid = false;
    assert(dppd_rte_flow_backend_query_age(&backend, 100, 10, &result, &error) == 0);
    assert(result.aged && !result.sec_since_last_hit_valid && result.sec_since_last_hit == 0);
    seconds_valid = true;
    struct dppd_flow_handle *handle = (struct dppd_flow_handle *)dppd_rte_flow_backend_find_version(&backend, 100, 10);
    assert(handle != NULL);
    handle->has_age = false;
    assert(dppd_rte_flow_backend_query_age(&backend, 100, 10, &result, &error) == -EUCLEAN);
    handle->has_age = true;
    remove_error = -EBUSY;
    assert(dppd_rte_flow_backend_remove_version(&backend, 100, 10, &error) == -EBUSY);
    assert(dppd_rte_flow_backend_count(&backend) == 3 && handle->flow != NULL && handle->has_age);
    assert(dppd_rte_flow_backend_query_age(&backend, 100, 10, &result, &error) == 0 && result.aged);
    remove_error = 0;
    assert(dppd_rte_flow_backend_remove_version(&backend, 100, 10, &error) == 0);
    assert(dppd_rte_flow_backend_query_age(&backend, 100, 10, &result, &error) == -ENOENT);
    assert(dppd_rte_flow_backend_fini(&backend) == 0 && removals == 4);
}

int main(void)
{
    rule_contract();
    backend_contract();
    return 0;
}
