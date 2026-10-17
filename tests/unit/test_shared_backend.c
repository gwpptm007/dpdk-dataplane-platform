#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dppd/rte_flow_backend.h"

static unsigned int shared_creates, shared_queries, shared_destroys, bound_validates, bound_creates, removes;
static int shared_create_error, query_error, shared_destroy_error, remove_error;
static unsigned int fail_bound_create;
static bool partial_shared_create;
static char order[256];
static unsigned int order_count;

static int ordinary_validate(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_error *error)
{
    (void)port;
    (void)rule;
    (void)error;
    assert(false);
    return -EIO;
}

static int ordinary_create(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_handle *handle,
    struct dppd_flow_error *error)
{
    (void)handle;
    return ordinary_validate(port, rule, error);
}

static int shared_create(const struct dppd_shared_count_spec *spec, struct dppd_shared_count_handle *handle,
    struct dppd_flow_error *error)
{
    ++shared_creates;
    if (shared_create_error == 0 || partial_shared_create)
        *handle = (struct dppd_shared_count_handle){.spec = *spec,
            .opaque = (struct rte_flow_action_handle *)(uintptr_t)(spec->id + 100)};
    if (error != NULL) {
        error->code = shared_create_error;
        snprintf(error->message, sizeof(error->message), "fake indirect create");
    }
    return shared_create_error;
}

static int shared_query(const struct dppd_shared_count_handle *handle, uint64_t *hits, uint64_t *bytes,
    struct dppd_flow_error *error)
{
    ++shared_queries;
    assert(handle->opaque == (struct rte_flow_action_handle *)(uintptr_t)(handle->spec.id + 100));
    *hits = 13;
    *bytes = 1300;
    if (error != NULL) {
        error->code = query_error;
        snprintf(error->message, sizeof(error->message), "fake indirect query");
    }
    return query_error;
}

/** 故意先破坏参数再报错，验证仓库仍持有原来的清理句柄 */
static int shared_destroy(struct dppd_shared_count_handle *handle, struct dppd_flow_error *error)
{
    ++shared_destroys;
    order[order_count++] = 'S';
    assert(handle->opaque == (struct rte_flow_action_handle *)(uintptr_t)(handle->spec.id + 100));
    memset(handle, 0, sizeof(*handle));
    if (error != NULL)
        error->code = shared_destroy_error;
    return shared_destroy_error;
}

static void check_binding(uint16_t port, const struct dppd_rule *rule, const struct dppd_flow_binding *binding)
{
    assert(port == 7 && rule->actions[0].type == DPPD_ACTION_SHARED_COUNT);
    assert(binding->actions[0] ==
        (struct rte_flow_action_handle *)(uintptr_t)(rule->actions[0].conf.shared_count.id + 100));
    for (uint16_t i = 1; i < DPPD_RULE_MAX_ACTIONS; ++i)
        assert(binding->actions[i] == NULL);
}

static int bound_validate(uint16_t port, const struct dppd_rule *rule, const struct dppd_flow_binding *binding,
    struct dppd_flow_error *error)
{
    (void)error;
    ++bound_validates;
    check_binding(port, rule, binding);
    return 0;
}

static int bound_create(uint16_t port, const struct dppd_rule *rule, const struct dppd_flow_binding *binding,
    struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    ++bound_creates;
    check_binding(port, rule, binding);
    if (bound_creates == fail_bound_create)
        return -ENOSPC;
    *handle = (struct dppd_flow_handle){.flow = (struct rte_flow *)(uintptr_t)rule->id,
        .rule_id = rule->id,
        .rule_generation = rule->generation,
        .port_id = port};
    return 0;
}

static int remove_flow(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    ++removes;
    order[order_count++] = 'F';
    if (remove_error != 0)
        return remove_error;
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static const struct dppd_flow_api api = {.validate = ordinary_validate,
    .create = ordinary_create,
    .remove = remove_flow,
    .shared_count_create = shared_create,
    .shared_count_query = shared_query,
    .shared_count_destroy = shared_destroy,
    .validate_bound = bound_validate,
    .create_bound = bound_create};

static struct dppd_shared_count_spec spec_for(uint64_t id)
{
    return (struct dppd_shared_count_spec){.id = id,
        .generation = id + 10,
        .install_port_id = 7,
        .domain = DPPD_RULE_DOMAIN_TRANSFER};
}

static struct dppd_transaction_item item_for(uint64_t id, const struct dppd_shared_count_spec *spec)
{
    return (struct dppd_transaction_item){
        .rule = {.id = id,
            .generation = id + 1,
            .install_port_id = 7,
            .domain = DPPD_RULE_DOMAIN_TRANSFER,
            .fallback = DPPD_FALLBACK_REQUIRE_HARDWARE,
            .nb_matches = 1,
            .matches = {{.type = DPPD_MATCH_ETH}},
            .nb_actions = 2,
            .actions = {{.type = DPPD_ACTION_SHARED_COUNT,
                            .conf.shared_count = {.id = spec->id, .generation = spec->generation}},
                {.type = DPPD_ACTION_DROP}}},
        .plan = {.rule_id = id,
            .rule_generation = id + 1,
            .backend = DPPD_PLAN_BACKEND_RTE_FLOW,
            .install_port_id = 7}};
}

static uint32_t references(const struct dppd_rte_flow_backend *backend, const struct dppd_shared_count_spec *spec)
{
    struct dppd_shared_count_info info;
    assert(dppd_rte_flow_backend_shared_count_get(backend, spec->id, spec->generation, &info) == 0);
    return info.references;
}

/** 无副作用检查覆盖精确版本、范围和驱动能力，失败不能发布半份结果 */
static void registry_contract(void)
{
    struct dppd_rte_flow_backend backend;
    assert(dppd_rte_flow_backend_init(&backend, 8, &api) == 0);
    struct dppd_shared_count_spec a = spec_for(9), b = spec_for(2);
    assert(dppd_rte_flow_backend_shared_count_create(&backend, &a, NULL) == 0);
    assert(dppd_rte_flow_backend_shared_count_create(&backend, &b, NULL) == 0);
    assert(dppd_rte_flow_backend_shared_count_create(&backend, &a, NULL) == -EEXIST);
    struct dppd_shared_count_spec list[2] = {{.id = 999}};
    uint32_t count = 99;
    assert(dppd_rte_flow_backend_shared_count_list(&backend, list, 1, &count) == -ENOSPC);
    assert(list[0].id == 999 && count == 99);
    assert(dppd_rte_flow_backend_shared_count_list(&backend, list, 2, &count) == 0);
    assert(count == 2 && list[0].id == 2 && list[1].id == 9);
    struct dppd_shared_count_info info = {.references = 99};
    assert(dppd_rte_flow_backend_shared_count_get(&backend, a.id, a.generation + 1, &info) == -ESTALE);
    assert(info.references == 99);
    assert(dppd_rte_flow_backend_shared_count_get(&backend, a.id, UINT64_MAX, &info) == 0);
    uint64_t hits = 71, bytes = 72;
    unsigned int queries = shared_queries;
    assert(dppd_rte_flow_backend_shared_count_query(&backend, a.id, a.generation + 1, &hits, &bytes, NULL) == -ESTALE);
    assert(shared_queries == queries && hits == 71 && bytes == 72);
    backend.api.shared_count_query = NULL;
    assert(dppd_rte_flow_backend_shared_count_query(&backend, a.id, a.generation, &hits, &bytes, NULL) == -ENOTSUP);
    assert(shared_queries == queries && hits == 71 && bytes == 72);
    backend.api.shared_count_query = shared_query;
    query_error = -EIO;
    assert(dppd_rte_flow_backend_shared_count_query(&backend, a.id, a.generation, &hits, &bytes, NULL) == -EIO);
    assert(hits == 71 && bytes == 72);
    query_error = 0;
    assert(dppd_rte_flow_backend_shared_count_query(&backend, a.id, UINT64_MAX, &hits, &bytes, NULL) == 0);
    assert(hits == 13 && bytes == 1300);
    unsigned int calls = shared_destroys;
    assert(dppd_rte_flow_backend_shared_count_destroy(&backend, a.id, UINT64_MAX, NULL) == -EINVAL);
    assert(dppd_rte_flow_backend_shared_count_destroy(&backend, a.id, a.generation + 1, NULL) == -ESTALE);
    assert(shared_destroys == calls);
    shared_destroy_error = -EIO;
    assert(dppd_rte_flow_backend_shared_count_destroy(&backend, a.id, a.generation, NULL) == -EIO);
    assert(references(&backend, &a) == 0);
    shared_destroy_error = 0;
    assert(dppd_rte_flow_backend_shared_count_destroy(&backend, a.id, a.generation, NULL) == 0);
    assert(dppd_rte_flow_backend_fini(&backend) == 0);
}

static void reference_contract(void)
{
    struct dppd_rte_flow_backend backend;
    assert(dppd_rte_flow_backend_init(&backend, 8, &api) == 0);
    struct dppd_shared_count_spec spec = spec_for(3);
    assert(dppd_rte_flow_backend_shared_count_create(&backend, &spec, NULL) == 0);
    struct dppd_transaction_backend ops = dppd_rte_flow_transaction_backend(&backend);
    struct dppd_transaction_item first = item_for(100, &spec), second = item_for(200, &spec), bad = first;
    uintptr_t one = 0, two = 0, untouched = 999;
    unsigned int calls = bound_validates;
    bad.rule.actions[0].conf.shared_count.generation++;
    assert(ops.validate(ops.context, &bad) == -ESTALE);
    assert(ops.prepare(ops.context, &bad, &untouched) == -ESTALE && untouched == 999);
    bad = first;
    bad.plan.install_port_id++;
    assert(ops.validate(ops.context, &bad) == -EXDEV);
    bad = first;
    bad.rule.domain = DPPD_RULE_DOMAIN_INGRESS;
    assert(ops.validate(ops.context, &bad) == -EXDEV && bound_validates == calls);
    backend.api.validate_bound = NULL;
    assert(ops.validate(ops.context, &first) == -ENOTSUP);
    backend.api.validate_bound = bound_validate;
    backend.api.create_bound = NULL;
    assert(ops.validate(ops.context, &first) == -ENOTSUP && bound_validates == calls);
    backend.api.create_bound = bound_create;
    assert(ops.validate(ops.context, &first) == 0);
    assert(references(&backend, &spec) == 0);
    assert(ops.prepare(ops.context, &first, &one) == 0 && references(&backend, &spec) == 1);
    calls = shared_destroys;
    assert(dppd_rte_flow_backend_shared_count_destroy(&backend, spec.id, spec.generation, NULL) == -EBUSY);
    assert(shared_destroys == calls);
    assert(ops.rollback(ops.context, &first, one, false) == 0 && references(&backend, &spec) == 0);
    assert(ops.prepare(ops.context, &first, &one) == 0 && ops.commit(ops.context, &first, one) == 0);
    assert(ops.prepare(ops.context, &second, &two) == 0 && ops.commit(ops.context, &second, two) == 0);
    assert(references(&backend, &spec) == 2);
    uint64_t hits = 0, bytes = 0;
    assert(dppd_rte_flow_backend_query_count(&backend, first.rule.id, first.rule.generation, &hits, &bytes, NULL) ==
        -ENODATA);
    assert(dppd_rte_flow_backend_remove_version(&backend, first.rule.id, first.rule.generation, NULL) == 0);
    assert(references(&backend, &spec) == 1);
    remove_error = -EIO;
    assert(dppd_rte_flow_backend_remove_version(&backend, second.rule.id, second.rule.generation, NULL) == -EIO);
    assert(references(&backend, &spec) == 1);
    uint32_t residual = 0;
    assert(dppd_rte_flow_backend_reconcile(&backend, &residual) == -EIO && residual == 2);
    remove_error = 0;
    shared_destroy_error = -EBUSY;
    order_count = 0;
    assert(dppd_rte_flow_backend_reconcile(&backend, &residual) == -EBUSY && residual == 1);
    assert(order_count == 2 && order[0] == 'F' && order[1] == 'S');
    assert(backend.objects != NULL && references(&backend, &spec) == 0);
    shared_destroy_error = 0;
    assert(dppd_rte_flow_backend_reconcile(&backend, &residual) == 0 && residual == 0);
    assert(dppd_rte_flow_backend_fini(&backend) == 0);
}

/** 第二条失败时从真实事务入口回滚，释放引用只发生在相应规则清理成功之后 */
static void rollback_contract(void)
{
    struct dppd_rte_flow_backend backend;
    assert(dppd_rte_flow_backend_init(&backend, 8, &api) == 0);
    struct dppd_shared_count_spec spec = spec_for(5);
    assert(dppd_rte_flow_backend_shared_count_create(&backend, &spec, NULL) == 0);
    struct dppd_transaction_item items[] = {item_for(100, &spec), item_for(200, &spec)};
    struct dppd_transaction transaction;
    struct dppd_transaction_backends backends = {.rte_flow = dppd_rte_flow_transaction_backend(&backend)};
    fail_bound_create = bound_creates + 2;
    assert(dppd_transaction_init(&transaction, 1, items, 2) == 0);
    assert(dppd_transaction_run(&transaction, &backends) == -ENOSPC);
    assert(references(&backend, &spec) == 0 && backend.count == 0);
    fail_bound_create = 0;
    assert(dppd_rte_flow_backend_fini(&backend) == 0);
    assert(dppd_rte_flow_backend_init(&backend, 8, &api) == 0);
    assert(dppd_rte_flow_backend_shared_count_create(&backend, &spec, NULL) == 0);
    struct dppd_transaction_backend ops = dppd_rte_flow_transaction_backend(&backend);
    uintptr_t token;
    struct dppd_transaction_item item = item_for(100, &spec);
    assert(ops.prepare(ops.context, &item, &token) == 0);
    /** 退出也必须释放尚未 commit 的预留引用，才能删除共享对象 */
    assert(dppd_rte_flow_backend_fini(&backend) == 0 && backend.objects == NULL);
    assert(dppd_rte_flow_backend_init(&backend, 8, &api) == 0);
    assert(dppd_rte_flow_backend_shared_count_create(&backend, &spec, NULL) == 0);
    backends.rte_flow = dppd_rte_flow_transaction_backend(&backend);
    assert(dppd_transaction_init(&transaction, 2, items, 2) == 0);
    fail_bound_create = bound_creates + 2;
    remove_error = -EIO;
    assert(dppd_transaction_run(&transaction, &backends) == -ENOSPC && transaction.rollback_code == -EIO);
    assert(references(&backend, &spec) == 1 && backend.count == 1);
    unsigned int destroys = shared_destroys;
    assert(dppd_rte_flow_backend_fini(&backend) == -EIO && backend.objects != NULL);
    assert(shared_destroys == destroys && references(&backend, &spec) == 1);
    remove_error = 0;
    fail_bound_create = 0;
    order_count = 0;
    assert(dppd_rte_flow_backend_fini(&backend) == 0);
    assert(order_count == 2 && order[0] == 'F' && order[1] == 'S');
}

static int recovery_guard(void *context, uint16_t port, const struct dppd_rule *rule, uint64_t *attempt,
    uint8_t cookie[DPPD_TAP_COOKIE_SIZE])
{
    (void)context;
    (void)port;
    (void)rule;
    (void)attempt;
    (void)cookie;
    assert(false);
    return -EIO;
}

static void limits_and_failures(void)
{
    struct dppd_rte_flow_backend backend;
    assert(dppd_rte_flow_backend_init(&backend, 8, &api) == 0);
    struct dppd_shared_count_spec spec = spec_for(1);
    backend.before_create = recovery_guard;
    unsigned int calls = shared_creates;
    assert(dppd_rte_flow_backend_shared_count_create(&backend, &spec, NULL) == -ENOTSUP && shared_creates == calls);
    backend.before_create = NULL;
    struct dppd_shared_count_spec invalid = spec;
    invalid.id = UINT64_MAX;
    assert(dppd_rte_flow_backend_shared_count_create(&backend, &invalid, NULL) == -EINVAL && shared_creates == calls);
    backend.api.shared_count_create = NULL;
    assert(dppd_rte_flow_backend_shared_count_create(&backend, &spec, NULL) == -ENOTSUP);
    backend.api.shared_count_create = shared_create;
    backend.api.shared_count_destroy = NULL;
    assert(dppd_rte_flow_backend_shared_count_create(&backend, &spec, NULL) == -ENOTSUP && shared_creates == calls);
    backend.api.shared_count_destroy = shared_destroy;
    assert(dppd_rte_flow_backend_shared_count_create(&backend, &spec, NULL) == 0);
    backend.before_create = recovery_guard;
    struct dppd_transaction_item guarded = item_for(100, &spec);
    struct dppd_transaction_backend ops = dppd_rte_flow_transaction_backend(&backend);
    uintptr_t untouched = 77;
    assert(ops.validate(ops.context, &guarded) == -ENOTSUP);
    assert(ops.prepare(ops.context, &guarded, &untouched) == -ENOTSUP && untouched == 77);
    backend.before_create = NULL;
    backend.api.shared_count_destroy = NULL;
    assert(dppd_rte_flow_backend_shared_count_destroy(&backend, spec.id, spec.generation, NULL) == -ENOTSUP);
    backend.api.shared_count_destroy = shared_destroy;
    assert(dppd_rte_flow_backend_shared_count_destroy(&backend, spec.id, spec.generation, NULL) == 0);
    for (uint64_t id = 1; id <= DPPD_SHARED_COUNT_MAX; ++id) {
        spec = spec_for(id);
        assert(dppd_rte_flow_backend_shared_count_create(&backend, &spec, NULL) == 0);
    }
    spec = spec_for(100);
    calls = shared_creates;
    assert(dppd_rte_flow_backend_shared_count_create(&backend, &spec, NULL) == -ENOSPC && shared_creates == calls);
    assert(dppd_rte_flow_backend_shared_count_count(&backend) == DPPD_SHARED_COUNT_MAX);
    assert(dppd_rte_flow_backend_fini(&backend) == 0);
    assert(dppd_rte_flow_backend_init(&backend, 8, &api) == 0);
    shared_create_error = -EIO;
    assert(dppd_rte_flow_backend_shared_count_create(&backend, &spec, NULL) == -EIO && backend.shared_count_count == 0);
    partial_shared_create = true;
    assert(dppd_rte_flow_backend_shared_count_create(&backend, &spec, NULL) == -EUCLEAN &&
        backend.shared_count_count == 1);
    struct dppd_shared_count_info info = {.references = 77};
    assert(dppd_rte_flow_backend_shared_count_get(&backend, spec.id, spec.generation, &info) == -EUCLEAN);
    assert(info.references == 77);
    shared_create_error = 0;
    partial_shared_create = false;
    shared_destroy_error = -EIO;
    assert(dppd_rte_flow_backend_fini(&backend) == -EIO && backend.shared_counts != NULL);
    shared_destroy_error = 0;
    assert(dppd_rte_flow_backend_fini(&backend) == 0 && backend.shared_counts == NULL);
}

int main(void)
{
    struct dppd_rte_flow_backend defaults;
    assert(dppd_rte_flow_backend_init(&defaults, 1, NULL) == 0);
    assert(defaults.api.shared_count_create == dppd_flow_shared_count_create);
    assert(defaults.api.shared_count_query == dppd_flow_shared_count_query);
    assert(defaults.api.shared_count_destroy == dppd_flow_shared_count_destroy);
    assert(
        defaults.api.validate_bound == dppd_flow_validate_bound && defaults.api.create_bound == dppd_flow_create_bound);
    assert(dppd_rte_flow_backend_fini(&defaults) == 0);
    registry_contract();
    reference_contract();
    rollback_contract();
    limits_and_failures();
    return 0;
}
