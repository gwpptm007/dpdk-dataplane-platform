#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dppd/rte_flow_backend.h"

static unsigned int pmd_calls, journal_calls, ready_calls;
static bool driver_allowed = true, event_during_journal;
static uint64_t partial_rule;
static int ready_error;
static int journal_context;

static void entered(void)
{
    assert(driver_allowed);
    ++pmd_calls;
}

static int validate(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_error *error)
{
    (void)error;
    entered();
    assert(port == 7 && dppd_rule_validate(rule, NULL, 0) == 0);
    return 0;
}

static int create(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_handle *handle,
    struct dppd_flow_error *error)
{
    (void)error;
    entered();
    *handle = (struct dppd_flow_handle){.flow = (struct rte_flow *)(uintptr_t)rule->id,
        .rule_id = rule->id,
        .rule_generation = rule->generation,
        .port_id = port};
    for (uint16_t i = 0; i < rule->nb_actions; ++i) {
        handle->has_count |= rule->actions[i].type == DPPD_ACTION_COUNT;
        handle->has_age |= rule->actions[i].type == DPPD_ACTION_AGE;
    }
    return rule->id == partial_rule ? -ENOSPC : 0;
}

static int remove_flow(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    entered();
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static int query(const struct dppd_flow_handle *handle, uint64_t *hits, uint64_t *bytes, struct dppd_flow_error *error)
{
    (void)handle;
    (void)error;
    entered();
    *hits = 13;
    *bytes = 130;
    return 0;
}

static int age(const struct dppd_flow_handle *handle, struct dppd_flow_age_result *result,
    struct dppd_flow_error *error)
{
    (void)handle;
    (void)error;
    entered();
    *result = (struct dppd_flow_age_result){.sec_since_last_hit_valid = true, .sec_since_last_hit = 3};
    return 0;
}

static int shared_create(const struct dppd_shared_count_spec *spec, struct dppd_shared_count_handle *handle,
    struct dppd_flow_error *error)
{
    (void)error;
    entered();
    *handle = (struct dppd_shared_count_handle){.spec = *spec,
        .opaque = (struct rte_flow_action_handle *)(uintptr_t)spec->id};
    return 0;
}

static int shared_query(const struct dppd_shared_count_handle *handle, uint64_t *hits, uint64_t *bytes,
    struct dppd_flow_error *error)
{
    (void)handle;
    return query(NULL, hits, bytes, error);
}

static int shared_destroy(struct dppd_shared_count_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    entered();
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static int bound_validate(uint16_t port, const struct dppd_rule *rule, const struct dppd_flow_binding *binding,
    struct dppd_flow_error *error)
{
    assert(binding->actions[0] == (struct rte_flow_action_handle *)(uintptr_t)900);
    return validate(port, rule, error);
}

static int bound_create(uint16_t port, const struct dppd_rule *rule, const struct dppd_flow_binding *binding,
    struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    assert(binding->actions[0] == (struct rte_flow_action_handle *)(uintptr_t)900);
    return create(port, rule, handle, error);
}

static int simulate_reset(struct dppd_flow_error *error)
{
    (void)error;
    entered();
    return 0;
}

static const struct dppd_flow_api api = {.validate = validate,
    .create = create,
    .remove = remove_flow,
    .query_count = query,
    .query_age = age,
    .shared_count_create = shared_create,
    .shared_count_query = shared_query,
    .shared_count_destroy = shared_destroy,
    .validate_bound = bound_validate,
    .create_bound = bound_create,
    .simulate_reset = simulate_reset};
static const struct dppd_shared_count_spec counter = {.id = 900,
    .generation = 8,
    .install_port_id = 7,
    .domain = DPPD_RULE_DOMAIN_INGRESS};

static struct dppd_transaction_item item(uint64_t id, bool shared)
{
    struct dppd_transaction_item next = {
        .rule = {.id = id,
            .generation = id + 1,
            .install_port_id = 7,
            .domain = DPPD_RULE_DOMAIN_INGRESS,
            .nb_matches = 1,
            .matches = {{.type = DPPD_MATCH_ETH}},
            .nb_actions = 3,
            .actions = {{.type = shared ? DPPD_ACTION_SHARED_COUNT : DPPD_ACTION_COUNT},
                {.type = DPPD_ACTION_AGE, .conf.age_timeout_seconds = 60}, {.type = DPPD_ACTION_DROP}}},
        .plan = {.rule_id = id,
            .rule_generation = id + 1,
            .install_port_id = 7,
            .backend = DPPD_PLAN_BACKEND_RTE_FLOW}};
    if (shared) {
        next.rule.actions[0].conf.shared_count.id = 900;
        next.rule.actions[0].conf.shared_count.generation = 8;
    }
    return next;
}

static int ready(void *context)
{
    ++ready_calls;
    if (ready_error != 0)
        dppd_rte_flow_backend_quarantine(context);
    return ready_error;
}

static int before_shared(void *context, const struct dppd_shared_count_spec *spec, uint64_t *attempt)
{
    assert(context == &journal_context && spec->id == 900);
    ++journal_calls;
    *attempt = journal_calls;
    if (event_during_journal)
        ready_error = -ENODEV;
    return 0;
}

static int before_flow(void *context, uint16_t port, const struct dppd_rule *rule, uint64_t *attempt,
    uint8_t cookie[DPPD_TAP_COOKIE_SIZE])
{
    (void)rule;
    assert(context == &journal_context && port == 7);
    ++journal_calls;
    *attempt = journal_calls;
    memset(cookie, 0, DPPD_TAP_COOKIE_SIZE);
    if (event_during_journal)
        ready_error = -ENODEV;
    return 0;
}

static int after_create(void *context, uint64_t attempt, int error)
{
    (void)error;
    assert(context == &journal_context && attempt != 0 && driver_allowed);
    ++journal_calls;
    return 0;
}

static void after_remove(void *context, uint64_t attempt, int error)
{
    (void)after_create(context, attempt, error);
}

static void configure(struct dppd_rte_flow_backend *backend, bool journal)
{
    ready_error = 0;
    driver_allowed = true;
    event_during_journal = false;
    partial_rule = 0;
    assert(dppd_rte_flow_backend_init(backend, 8, &api) == 0);
    backend->driver_ready = ready;
    backend->driver_context = backend;
    if (journal) {
        backend->before_create_context = &journal_context;
        backend->before_create = before_flow;
        backend->after_create = after_create;
        backend->after_remove = after_remove;
        backend->before_shared_create = before_shared;
        backend->after_shared_create = after_create;
        backend->after_shared_remove = after_remove;
    }
}

/** 仅释放本测试自己分配的假对象内存，不为生产路径提供遗忘旧句柄的入口 */
static void release_test_memory(struct dppd_rte_flow_backend *backend)
{
    free(backend->objects);
    free(backend->shared_counts);
    dppd_flow_probe_cache_destroy(backend->probes);
}

static void quarantine_all_entries(void)
{
    struct dppd_rte_flow_backend backend;
    configure(&backend, true);
    assert(dppd_rte_flow_backend_shared_count_create(&backend, &counter, NULL) == 0);
    struct dppd_transaction_backend ops = dppd_rte_flow_transaction_backend(&backend);
    struct dppd_transaction_item rules[] = {item(10, false), item(20, true), item(30, true), item(40, true),
        item(50, true)};
    uintptr_t tokens[5];
    for (unsigned int i = 0; i < 5; ++i) {
        assert(ops.validate(ops.context, &rules[i]) == 0 && ops.prepare(ops.context, &rules[i], &tokens[i]) == 0);
        if (i == 3)
            continue;
        partial_rule = i == 4 ? rules[i].rule.id : 0;
        assert(ops.commit(ops.context, &rules[i], tokens[i]) == (i == 4 ? -ENOSPC : 0));
    }
    assert(backend.count == 5 && backend.shared_count_count == 1);
    struct dppd_shared_count_info info;
    assert(dppd_rte_flow_backend_shared_count_get(&backend, 900, 8, &info) == 0 && info.references == 4);
    struct dppd_flow_handle previous = *dppd_rte_flow_backend_find_version(&backend, 10, 11);
    dppd_rte_flow_backend_quarantine(&backend);
    struct dppd_rte_flow_backend before = backend;
    unsigned int calls = pmd_calls, records = journal_calls, checks = ready_calls;
    driver_allowed = false;
    dppd_rte_flow_backend_quarantine(&backend);
    struct dppd_flow_error error;
    assert(dppd_rte_flow_backend_driver_ready(&backend) == -EUCLEAN);
    assert(dppd_rte_flow_backend_validate_rule(&backend, 7, &rules[0].rule, &error) == -EUCLEAN);
    assert(error.code == -EUCLEAN && strstr(error.message, "quarantined") != NULL);
    assert(ops.validate(ops.context, &rules[1]) == -EUCLEAN);
    uintptr_t untouched = 999;
    assert(ops.prepare(ops.context, &rules[0], &untouched) == -EUCLEAN && untouched == 999);
    assert(ops.commit(ops.context, &rules[3], tokens[3]) == -EUCLEAN);
    assert(ops.rollback(ops.context, &rules[3], tokens[3], false) == -EUCLEAN);
    assert(ops.rollback(ops.context, &rules[4], tokens[4], true) == -EUCLEAN);
    assert(dppd_rte_flow_backend_remove(&backend, 10, &error) == -EUCLEAN);
    assert(dppd_rte_flow_backend_remove_version(&backend, 20, 21, &error) == -EUCLEAN);
    uint64_t hits = 71, bytes = 72;
    assert(dppd_rte_flow_backend_query_count(&backend, 10, 11, &hits, &bytes, &error) == -EUCLEAN);
    struct dppd_flow_age_result aged = {.aged = true, .sec_since_last_hit = 99}, saved = aged;
    assert(dppd_rte_flow_backend_query_age(&backend, 10, 11, &aged, &error) == -EUCLEAN);
    assert(memcmp(&aged, &saved, sizeof(aged)) == 0 && hits == 71 && bytes == 72);
    assert(dppd_rte_flow_backend_shared_count_create(&backend, &counter, &error) == -EUCLEAN);
    assert(dppd_rte_flow_backend_shared_count_query(&backend, 900, 8, &hits, &bytes, &error) == -EUCLEAN);
    assert(dppd_rte_flow_backend_shared_count_destroy(&backend, 900, 8, &error) == -EUCLEAN);
    assert(dppd_rte_flow_backend_simulate_reset(&backend, &error) == -EUCLEAN);
    uint32_t residual = 77;
    assert(dppd_rte_flow_backend_reconcile(&backend, &residual) == -EUCLEAN && residual == 77);
    assert(dppd_rte_flow_backend_fini(&backend) == -EUCLEAN);
    assert(dppd_rte_flow_backend_resume_empty(&backend) == -EUCLEAN);
    /** 本地计数若被意外清零，真实槽位和引用仍不能被空仓库恢复接口跳过 */
    backend.count = backend.shared_count_count = 0;
    assert(dppd_rte_flow_backend_resume_empty(&backend) == -EUCLEAN);
    backend.count = before.count;
    backend.shared_count_count = before.shared_count_count;
    assert(dppd_rte_flow_backend_shared_count_get(&backend, 900, 8, &info) == 0 && info.references == 4);
    struct dppd_shared_count_spec specs[1];
    uint32_t count = 99;
    assert(
        dppd_rte_flow_backend_shared_count_list(&backend, specs, 1, &count) == 0 && count == 1 && specs[0].id == 900);
    struct dppd_rule_install_info installation = {.rule_id = 999}, preserved = installation;
    assert(dppd_rte_flow_backend_install_info(&backend, 999, 999, &installation) == -ENOENT);
    assert(memcmp(&installation, &preserved, sizeof(installation)) == 0);
    assert(dppd_rte_flow_backend_install_info(&backend, 10, 11, &installation) == 0 && installation.has_count);
    assert(memcmp(dppd_rte_flow_backend_find(&backend, 10), &previous, sizeof(previous)) == 0);
    assert(memcmp(&backend, &before, sizeof(before)) == 0);
    assert(pmd_calls == calls && journal_calls == records && ready_calls == checks && hits == 71 && bytes == 72);
    release_test_memory(&backend);
}

static void readiness_and_empty_resume(void)
{
    struct dppd_rte_flow_backend backend;
    configure(&backend, false);
    struct dppd_transaction_backend ops = dppd_rte_flow_transaction_backend(&backend);
    struct dppd_transaction_item rule = item(10, false);
    uintptr_t token;
    assert(ops.prepare(ops.context, &rule, &token) == 0 && ops.commit(ops.context, &rule, token) == 0);
    unsigned int calls = pmd_calls;
    uint64_t hits = 71, bytes = 72;
    ready_error = -ENODEV;
    driver_allowed = false;
    assert(dppd_rte_flow_backend_query_count(&backend, 10, 11, &hits, &bytes, NULL) == -ENODEV);
    assert(dppd_rte_flow_backend_is_quarantined(&backend) && pmd_calls == calls && hits == 71 && bytes == 72);
    assert(dppd_rte_flow_backend_fini(&backend) == -EUCLEAN);
    release_test_memory(&backend);
    configure(&backend, false);
    assert(dppd_rte_flow_backend_resume_empty(&backend) == -EALREADY);
    dppd_rte_flow_backend_quarantine(&backend);
    ready_error = -ESTALE;
    assert(dppd_rte_flow_backend_resume_empty(&backend) == -ESTALE && dppd_rte_flow_backend_is_quarantined(&backend));
    ready_error = 0;
    assert(dppd_rte_flow_backend_resume_empty(&backend) == 0 && !dppd_rte_flow_backend_is_quarantined(&backend));
    assert(ops.context == &backend);
    assert(ops.prepare(ops.context, &rule, &token) == 0 && ops.commit(ops.context, &rule, token) == 0);
    assert(dppd_rte_flow_backend_fini(&backend) == 0);
}

/** 创建前日志可能耗时，事件发生在日志之后时仍必须在实际 create 前阻断 */
static void event_during_intent(void)
{
    for (unsigned int shared = 0; shared < 2; ++shared) {
        struct dppd_rte_flow_backend backend;
        configure(&backend, true);
        event_during_journal = true;
        unsigned int calls = pmd_calls;
        if (shared != 0)
            assert(dppd_rte_flow_backend_shared_count_create(&backend, &counter, NULL) == -ENODEV);
        else {
            struct dppd_transaction_backend ops = dppd_rte_flow_transaction_backend(&backend);
            struct dppd_transaction_item rule = item(10, false);
            uintptr_t token;
            assert(ops.prepare(ops.context, &rule, &token) == 0);
            assert(ops.commit(ops.context, &rule, token) == -ENODEV);
            assert(backend.count == 1 && ops.rollback(ops.context, &rule, token, true) == -EUCLEAN);
        }
        assert(pmd_calls == calls && dppd_rte_flow_backend_is_quarantined(&backend));
        assert(dppd_rte_flow_backend_fini(&backend) == -EUCLEAN);
        release_test_memory(&backend);
    }
}

int main(void)
{
    assert(!dppd_rte_flow_backend_is_quarantined(NULL));
    assert(dppd_rte_flow_backend_driver_ready(NULL) == -EINVAL);
    assert(dppd_rte_flow_backend_resume_empty(NULL) == -EINVAL);
    dppd_rte_flow_backend_quarantine(NULL);
    quarantine_all_entries();
    readiness_and_empty_resume();
    event_during_intent();
    return 0;
}
