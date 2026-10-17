#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dppd/age.h"
#include "dppd/control.h"

struct fake_flow {
    bool used, aged;
    int query_error, remove_error;
    struct dppd_rule rule;
};

/** 模拟驱动只返回老化事实，扫描、版本保护、删除和持久化都走正式控制接口 */
static struct {
    struct fake_flow flows[80];
    unsigned int queries, removes;
    uint64_t queried[128];
    struct dppd_control_service *replace_during_query;
} fake;

static struct fake_flow *flow_for(struct dppd_control_service *control, uint64_t id, uint64_t generation)
{
    const struct dppd_flow_handle *handle = dppd_rte_flow_backend_find_version(&control->rte_flow, id, generation);
    assert(handle != NULL);
    struct fake_flow *flow = (struct fake_flow *)handle->flow;
    assert(flow->used && flow->rule.id == id && flow->rule.generation == generation);
    return flow;
}

static int fake_validate(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_error *error)
{
    (void)error;
    char reason[128];
    assert(port == 0 && dppd_rule_validate(rule, reason, sizeof(reason)) == 0);
    return 0;
}

static int fake_create(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_handle *handle,
    struct dppd_flow_error *error)
{
    (void)error;
    for (unsigned int i = 0; i < sizeof(fake.flows) / sizeof(*fake.flows); ++i) {
        struct fake_flow *flow = &fake.flows[i];
        if (flow->used)
            continue;
        *flow = (struct fake_flow){.used = true, .rule = *rule};
        *handle = (struct dppd_flow_handle){.rule_id = rule->id,
            .rule_generation = rule->generation,
            .port_id = port,
            .flow = (struct rte_flow *)flow};
        for (uint16_t action = 0; action < rule->nb_actions; ++action)
            handle->has_age |= rule->actions[action].type == DPPD_ACTION_AGE;
        return 0;
    }
    return -ENOSPC;
}

static int fake_remove(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    struct fake_flow *flow = (struct fake_flow *)handle->flow;
    assert(flow != NULL && flow->used && flow->rule.generation == handle->rule_generation);
    fake.removes++;
    if (flow->remove_error != 0)
        return flow->remove_error;
    memset(flow, 0, sizeof(*flow));
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static int fake_query(const struct dppd_flow_handle *handle, struct dppd_flow_age_result *result,
    struct dppd_flow_error *error)
{
    struct fake_flow *flow = (struct fake_flow *)handle->flow;
    assert(flow != NULL && flow->used && handle->has_age);
    assert(fake.queries < sizeof(fake.queried) / sizeof(*fake.queried));
    fake.queried[fake.queries++] = handle->rule_id;
    *result =
        (struct dppd_flow_age_result){.aged = flow->aged, .sec_since_last_hit_valid = true, .sec_since_last_hit = 9};
    int rc = flow->query_error;
    if (fake.replace_during_query != NULL) {
        /** 故意在查询与删除之间换代，过期的驱动答复不能删除新版本 */
        struct dppd_control_service *control = fake.replace_during_query;
        struct dppd_rule replacement = flow->rule;
        struct dppd_control_apply_result applied;
        fake.replace_during_query = NULL;
        replacement.priority++;
        assert(dppd_control_apply(control, 0, &replacement, replacement.generation, &applied) == 0);
    }
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
        error->code = rc;
    }
    return rc;
}

static const struct dppd_flow_api api = {.validate = fake_validate,
    .create = fake_create,
    .remove = fake_remove,
    .query_age = fake_query};
static const struct dppd_topology topology = {.nb_endpoints = 1, .endpoints = {{.ethdev_port_id = 0}}};

static void initialize(struct dppd_control_service *control)
{
    memset(&fake, 0, sizeof(fake));
    assert(dppd_control_init(control, &topology, 72, &api) == 0);
}

static uint64_t install(struct dppd_control_service *control, uint64_t id, bool age)
{
    struct dppd_rule rule = {.id = id,
        .domain = DPPD_RULE_DOMAIN_INGRESS,
        .fallback = DPPD_FALLBACK_REQUIRE_HARDWARE,
        .nb_matches = 1,
        .nb_actions = age ? 2 : 1,
        .matches = {{.type = DPPD_MATCH_ETH}}};
    if (age)
        rule.actions[0] = (struct dppd_action){.type = DPPD_ACTION_AGE, .conf.age_timeout_seconds = 5};
    rule.actions[rule.nb_actions - 1].type = DPPD_ACTION_DROP;
    struct dppd_control_apply_result applied;
    assert(dppd_control_apply(control, 0, &rule, 0, &applied) == 0);
    return applied.generation;
}

static void preserved(const struct dppd_control_age_result *result)
{
    assert(result->rule_id == 77 && result->generation == 88 && result->state.aged);
    assert(result->state.sec_since_last_hit_valid && result->state.sec_since_last_hit == 17);
}

/** 精确查询只读且失败保留输出，不支持 AGE 时不能通过零计数或软件时间误判到期 */
static void queries(void)
{
    struct dppd_control_service control;
    initialize(&control);
    uint64_t aged_generation = install(&control, 10, true);
    uint64_t plain_generation = install(&control, 20, false);
    struct dppd_control_age_result result = {.rule_id = 77,
        .generation = 88,
        .state = {.aged = true, .sec_since_last_hit_valid = true, .sec_since_last_hit = 17}};
    assert(dppd_control_query_age(&control, 10, aged_generation + 1U, &result) == -ESTALE);
    assert(dppd_control_query_age(&control, 20, plain_generation, &result) == -ENODATA);
    preserved(&result);
    control.rte_flow.api.query_age = NULL;
    assert(dppd_control_query_age(&control, 10, aged_generation, &result) == -ENOTSUP);
    preserved(&result);
    assert(dppd_control_age_poll(&control, 0, 2) == -ENOTSUP);
    assert(fake.queries == 0 && fake.removes == 0 && control.rules.count == 2);
    control.rte_flow.api.query_age = fake_query;
    flow_for(&control, 10, aged_generation)->query_error = -EIO;
    assert(dppd_control_query_age(&control, 10, aged_generation, &result) == -EIO);
    preserved(&result);
    flow_for(&control, 10, aged_generation)->query_error = 0;
    assert(dppd_control_query_age(&control, 10, DPPD_RULE_GENERATION_ANY, &result) == 0);
    assert(result.rule_id == 10 && result.generation == aged_generation && !result.state.aged);
    assert(result.state.sec_since_last_hit_valid && result.state.sec_since_last_hit == 9);
    control.recovery_state = DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED;
    assert(dppd_control_query_age(&control, 10, aged_generation, &result) == -EUCLEAN);
    assert(dppd_control_age_poll(&control, UINT64_C(1000000000), 2) == -EUCLEAN);
    assert(fake.removes == 0);
    control.recovery_state = DPPD_CONTROL_RECOVERY_READY;
    assert(dppd_control_fini(&control) == 0);
}

/** 到期删除同时移除实际对象和账本，并写入正式快照，重启不能把过期规则恢复回来 */
static void persisted_expiration(void)
{
    char directory[] = "/tmp/dppd-age-unit-XXXXXX", path[256];
    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/rules.bin", directory);
    struct dppd_control_service control;
    initialize(&control);
    assert(dppd_control_persistence_restore(&control, path) == 0);
    uint64_t generation = install(&control, 100, true);
    flow_for(&control, 100, generation)->aged = true;
    assert(dppd_control_age_poll(&control, 0, 1) == 0);
    assert(control.rules.count == 0 && dppd_rte_flow_backend_count(&control.rte_flow) == 0);
    assert(fake.removes == 1 && control.age.expired == 1 && control.age.last_error == 0);
    struct dppd_control_persistence_status persistence;
    dppd_control_persistence_status(&control, &persistence);
    assert(persistence.enabled && !persistence.dirty && persistence.persisted_generation == generation + 1U);
    assert(dppd_control_fini(&control) == 0);
    initialize(&control);
    assert(dppd_control_persistence_restore(&control, path) == 0);
    assert(control.rules.count == 0 && dppd_rte_flow_backend_count(&control.rte_flow) == 0);
    struct dppd_rule rule;
    assert(dppd_rule_repository_get(&control.rules, 100, &rule) == -ENOENT);
    assert(dppd_control_fini(&control) == 0);
    assert(unlink(path) == 0 && rmdir(directory) == 0);
}

/** 删除报错时保留原句柄，重试间隔未到不重复询问，一秒后再通过正式删除路径重试 */
static void remove_retry(void)
{
    struct dppd_control_service control;
    initialize(&control);
    uint64_t generation = install(&control, 10, true);
    struct fake_flow *flow = flow_for(&control, 10, generation);
    flow->aged = true;
    flow->remove_error = -EBUSY;
    assert(dppd_control_age_poll(&control, 0, 1) == -EBUSY);
    assert(control.rules.count == 1 && dppd_rte_flow_backend_count(&control.rte_flow) == 1 && flow->used);
    assert(control.age.delete_errors == 1 && control.age.expired == 0 && fake.removes == 1);
    assert(control.age.next_poll_ns == DPPD_AGE_RETRY_INTERVAL_NS);
    assert(dppd_control_age_poll(&control, DPPD_AGE_RETRY_INTERVAL_NS - 1U, 1) == 0);
    assert(fake.queries == 1 && fake.removes == 1);
    flow->remove_error = 0;
    assert(dppd_control_age_poll(&control, DPPD_AGE_RETRY_INTERVAL_NS, 1) == 0);
    assert(fake.queries == 2 && fake.removes == 2 && control.age.expired == 1 && control.age.last_error == 0);
    assert(control.rules.count == 0 && dppd_rte_flow_backend_count(&control.rte_flow) == 0);
    assert(dppd_control_fini(&control) == 0);
}

/** 扫描预算按所有规则前进，普通规则不会饿死后面的 AGE，错误只能在完整成功扫描后清除 */
static void pagination(void)
{
    struct dppd_control_service control;
    initialize(&control);
    install(&control, 10, false);
    uint64_t generation = install(&control, 20, true);
    install(&control, 30, true);
    flow_for(&control, 20, generation)->query_error = -EIO;
    assert(dppd_control_age_poll(&control, 0, 0) == -EINVAL);
    assert(dppd_control_age_poll(&control, 0, DPPD_AGE_POLL_BUDGET + 1U) == -EINVAL);
    assert(control.age.polls == 0 && fake.queries == 0);
    assert(dppd_control_age_poll(&control, 0, 1) == 0 && control.age.cursor == 10 && fake.queries == 0);
    assert(dppd_control_age_poll(&control, control.age.next_poll_ns, 1) == -EIO);
    assert(control.age.cursor == 20 && control.age.last_error == -EIO && control.age.query_errors == 1);
    flow_for(&control, 20, generation)->query_error = 0;
    assert(dppd_control_age_poll(&control, control.age.next_poll_ns, 1) == 0);
    assert(control.age.cursor == 0 && control.age.last_error == -EIO);
    assert(dppd_control_age_poll(&control, control.age.next_poll_ns, 1) == 0);
    assert(control.age.cursor == 10 && control.age.last_error == -EIO);
    assert(dppd_control_age_poll(&control, control.age.next_poll_ns, 1) == 0);
    assert(control.age.cursor == 20 && control.age.last_error == -EIO);
    assert(dppd_control_age_poll(&control, control.age.next_poll_ns, 1) == 0);
    assert(control.age.cursor == 0 && control.age.last_error == 0 && fake.removes == 0);
    assert(fake.queries == 4 && fake.queried[0] == 20 && fake.queried[1] == 30);
    assert(fake.queried[2] == 20 && fake.queried[3] == 30);
    struct dppd_age_status status;
    assert(dppd_control_age_status(&control, &status) == 0 && status.tracked == 2 && status.polls == 6);
    assert(dppd_control_fini(&control) == 0);

    initialize(&control);
    for (uint64_t id = 1; id <= DPPD_AGE_POLL_BUDGET + 3U; ++id)
        install(&control, id, id % 3U != 0);
    for (unsigned int page = 0; page < 4; ++page)
        assert(dppd_control_age_poll(&control, control.age.next_poll_ns, 20) == 0);
    unsigned int expected_queries = 0;
    for (uint64_t id = 1; id <= DPPD_AGE_POLL_BUDGET + 3U; ++id)
        if (id % 3U != 0)
            assert(fake.queried[expected_queries++] == id);
    assert(fake.queries == expected_queries && control.age.cursor == 0);
    assert(dppd_control_fini(&control) == 0);
}

/** 查询旧版本时故意发布新版本，随后使用精确旧版本删除必须失败且保留新对象 */
static void replacement(void)
{
    struct dppd_control_service control;
    initialize(&control);
    uint64_t generation = install(&control, 100, true);
    flow_for(&control, 100, generation)->aged = true;
    fake.replace_during_query = &control;
    assert(dppd_control_age_poll(&control, 0, 1) == -ESTALE);
    struct dppd_rule rule;
    assert(dppd_rule_repository_get(&control.rules, 100, &rule) == 0 && rule.generation == generation + 1U);
    assert(control.rules.count == 1 && dppd_rte_flow_backend_count(&control.rte_flow) == 1);
    assert(dppd_rte_flow_backend_find_version(&control.rte_flow, 100, generation) == NULL);
    assert(flow_for(&control, 100, rule.generation)->used && fake.removes == 1 && control.age.expired == 0);
    assert(dppd_control_age_poll(&control, control.age.next_poll_ns, 1) == 0);
    assert(control.rules.count == 1 && fake.removes == 1);
    assert(dppd_control_fini(&control) == 0);
}

/** 仓库版本耗尽时必须在删除驱动对象前拒绝，不能留下实际已删但账本版本回绕的状态 */
static void generation_exhaustion(void)
{
    for (uint64_t remaining = 0; remaining < 2; ++remaining) {
        struct dppd_control_service control;
        initialize(&control);
        uint64_t generation = install(&control, 10, true);
        struct fake_flow *flow = flow_for(&control, 10, generation);
        flow->aged = true;
        control.rules.generation = UINT64_MAX - remaining;
        assert(dppd_control_age_poll(&control, 0, 1) == -EOVERFLOW);
        assert(control.rules.generation == UINT64_MAX - remaining && control.rules.count == 1);
        assert(flow->used && fake.removes == 0 && dppd_rte_flow_backend_count(&control.rte_flow) == 1);
        assert(dppd_control_fini(&control) == 0);
    }
}

int main(void)
{
    queries();
    persisted_expiration();
    remove_retry();
    pagination();
    replacement();
    generation_exhaustion();
    return 0;
}
