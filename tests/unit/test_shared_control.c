#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dppd/control.h"

struct counter {
    bool used;
    struct dppd_shared_count_spec spec;
    uint32_t references;
};

struct flow {
    bool used, aged;
    struct counter *counter;
};

/** 驱动夹具独立统计真实引用，用来核对控制层预留、删除和回滚没有漏掉对象 */
static struct {
    struct counter counters[8];
    struct flow flows[16];
    int query_error;
    unsigned int create_calls, fail_create;
} fake;

static int validate(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_error *error)
{
    (void)error;
    char reason[128];
    assert(port == 0 && dppd_rule_validate(rule, reason, sizeof(reason)) == 0);
    return 0;
}

static int validate_bound(uint16_t port, const struct dppd_rule *rule, const struct dppd_flow_binding *binding,
    struct dppd_flow_error *error)
{
    assert(binding != NULL && binding->actions[0] != NULL);
    struct counter *counter = (struct counter *)binding->actions[0];
    assert(counter->used && counter->spec.id == rule->actions[0].conf.shared_count.id);
    return validate(port, rule, error);
}

static int create_bound(uint16_t port, const struct dppd_rule *rule, const struct dppd_flow_binding *binding,
    struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    assert(validate_bound(port, rule, binding, error) == 0);
    if (++fake.create_calls == fake.fail_create)
        return -ENOSPC;
    for (size_t i = 0; i < sizeof(fake.flows) / sizeof(*fake.flows); ++i) {
        struct flow *flow = &fake.flows[i];
        if (flow->used)
            continue;
        *flow = (struct flow){.used = true, .counter = (struct counter *)binding->actions[0]};
        ++flow->counter->references;
        *handle = (struct dppd_flow_handle){.rule_id = rule->id,
            .rule_generation = rule->generation,
            .port_id = port,
            .flow = (struct rte_flow *)flow};
        for (uint16_t j = 0; j < rule->nb_actions; ++j)
            handle->has_age |= rule->actions[j].type == DPPD_ACTION_AGE;
        return 0;
    }
    return -ENOSPC;
}

static int create(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_handle *handle,
    struct dppd_flow_error *error)
{
    (void)port;
    (void)rule;
    (void)handle;
    (void)error;
    assert(!"shared rules must use bound create");
    return -ENOTSUP;
}

static int remove_flow(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    struct flow *flow = (struct flow *)handle->flow;
    assert(flow != NULL && flow->used && flow->counter->references != 0);
    --flow->counter->references;
    memset(flow, 0, sizeof(*flow));
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static int create_counter(const struct dppd_shared_count_spec *spec, struct dppd_shared_count_handle *handle,
    struct dppd_flow_error *error)
{
    (void)error;
    for (size_t i = 0; i < sizeof(fake.counters) / sizeof(*fake.counters); ++i) {
        if (fake.counters[i].used)
            continue;
        fake.counters[i] = (struct counter){.used = true, .spec = *spec};
        *handle = (struct dppd_shared_count_handle){.spec = *spec,
            .opaque = (struct rte_flow_action_handle *)&fake.counters[i]};
        return 0;
    }
    return -ENOSPC;
}

static int destroy_counter(struct dppd_shared_count_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    struct counter *counter = (struct counter *)handle->opaque;
    assert(counter != NULL && counter->used && counter->references == 0);
    memset(counter, 0, sizeof(*counter));
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static int query_counter(const struct dppd_shared_count_handle *handle, uint64_t *hits, uint64_t *bytes,
    struct dppd_flow_error *error)
{
    (void)error;
    assert(handle->opaque != NULL);
    *hits = 9;
    *bytes = 126;
    return fake.query_error;
}

static int query_age(const struct dppd_flow_handle *handle, struct dppd_flow_age_result *result,
    struct dppd_flow_error *error)
{
    (void)error;
    struct flow *flow = (struct flow *)handle->flow;
    *result = (struct dppd_flow_age_result){.aged = flow->aged};
    return 0;
}

static void initialize(struct dppd_control_service *service)
{
    static const struct dppd_topology topology = {.nb_endpoints = 1,
        .endpoints = {{.ethdev_port_id = 0, .kind = DPPD_ENDPOINT_ETHDEV}}};
    const struct dppd_flow_api api = {.validate = validate,
        .create = create,
        .remove = remove_flow,
        .validate_bound = validate_bound,
        .create_bound = create_bound,
        .query_age = query_age,
        .shared_count_create = create_counter,
        .shared_count_query = query_counter,
        .shared_count_destroy = destroy_counter};
    memset(&fake, 0, sizeof(fake));
    assert(dppd_control_init(service, &topology, 8, &api) == 0);
}

static uint64_t new_counter(struct dppd_control_service *service, uint64_t id)
{
    struct dppd_shared_count_spec spec = {.id = id, .domain = DPPD_RULE_DOMAIN_INGRESS};
    struct dppd_shared_count_info info;
    assert(dppd_control_shared_count_create(service, &spec, &info) == 0);
    assert(info.spec.id == id && info.references == 0);
    return info.spec.generation;
}

static struct dppd_rule rule(uint64_t id, uint64_t shared_generation, bool age)
{
    struct dppd_rule next = {.id = id,
        .fallback = DPPD_FALLBACK_REQUIRE_HARDWARE,
        .nb_matches = 1,
        .nb_actions = age ? 3 : 2};
    next.matches[0].type = DPPD_MATCH_ETH;
    next.actions[0].type = DPPD_ACTION_SHARED_COUNT;
    next.actions[0].conf.shared_count.id = 100;
    next.actions[0].conf.shared_count.generation = shared_generation;
    if (age)
        next.actions[1] = (struct dppd_action){.type = DPPD_ACTION_AGE, .conf.age_timeout_seconds = 5};
    next.actions[next.nb_actions - 1U].type = DPPD_ACTION_DROP;
    return next;
}

/** 精确对象身份和独立修订号阻止旧删除请求碰到同 ID 重建的对象，分页也检测修改 */
static void identity_and_paging(void)
{
    struct dppd_control_service service;
    initialize(&service);
    uint64_t first = new_counter(&service, 100), revision;
    struct dppd_control_shared_count_result result = {.hits = 77, .bytes = 88};
    fake.query_error = -EIO;
    assert(dppd_control_shared_count_query(&service, 100, first, &result) == -EIO);
    assert(result.hits == 77 && result.bytes == 88);
    fake.query_error = 0;
    assert(dppd_control_shared_count_query(&service, 100, first, &result) == 0);
    assert(result.hits == 9 && result.bytes == 126 && result.object.spec.generation == first);
    assert(dppd_control_shared_count_delete(&service, 100, 0, &revision) == -EINVAL);
    assert(dppd_control_shared_count_delete(&service, 100, UINT64_MAX, &revision) == -EINVAL);
    assert(dppd_control_shared_count_delete(&service, 100, first, &revision) == 0 && revision == first + 1U);
    uint64_t replacement = new_counter(&service, 100);
    assert(replacement > revision);
    assert(dppd_control_shared_count_delete(&service, 100, first, &revision) == -ESTALE);
    for (uint64_t id = 1; id <= 5; ++id)
        (void)new_counter(&service, id);
    struct dppd_shared_count_page page;
    assert(dppd_control_shared_count_list(&service, 0, UINT64_MAX, &page) == 0);
    assert(page.total == 6 && page.count == 4 && page.next_id == 4 && page.more);
    assert(dppd_control_shared_count_list(&service, 4, page.revision, &page) == 0);
    assert(page.count == 2 && page.objects[0].spec.id == 5 && page.objects[1].spec.id == 100 && !page.more);
    uint64_t old_revision = page.revision;
    assert(dppd_control_shared_count_delete(&service, 100, replacement, &revision) == 0);
    assert(dppd_control_shared_count_list(&service, 0, old_revision, &page) == -ESTALE);
    service.shared_count_revision = UINT64_MAX - 1U;
    assert(dppd_control_shared_count_delete(&service, 1, 4, &revision) == -EOVERFLOW);
    assert(dppd_control_fini(&service) == 0);
}

/** 自动老化复用正式删除，因此共享对象的引用也必须同时归还 */
static void age_releases_reference(void)
{
    struct dppd_control_service service;
    initialize(&service);
    uint64_t generation = new_counter(&service, 100), revision;
    struct dppd_rule next = rule(10, generation, true);
    struct dppd_control_apply_result applied;
    assert(dppd_control_apply(&service, 0, &next, 0, &applied) == 0);
    assert(dppd_control_shared_count_delete(&service, 100, generation, &revision) == -EBUSY);
    fake.flows[0].aged = true;
    assert(dppd_control_age_poll(&service, 0, DPPD_AGE_POLL_BUDGET) == 0 && service.age.expired == 1);
    struct dppd_shared_count_info info;
    assert(dppd_control_shared_count_get(&service, 100, generation, &info) == 0 && info.references == 0);
    assert(dppd_control_shared_count_delete(&service, 100, generation, &revision) == 0);
    assert(dppd_control_fini(&service) == 0);
}

/** 文件保存的是共享定义和引用规则，重放失败先撤规则再撤对象，成功才发布完整账本 */
static void persisted_replay(void)
{
    char directory[] = "/tmp/dppd-shared-control-XXXXXX", path[256];
    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/rules.bin", directory);
    struct dppd_control_service service;
    initialize(&service);
    assert(dppd_control_persistence_restore(&service, path) == 0);
    uint64_t generation = new_counter(&service, 100);
    struct dppd_rule next = rule(10, generation, false);
    struct dppd_control_apply_result applied;
    assert(dppd_control_apply(&service, 0, &next, 0, &applied) == 0);
    next.id = 20;
    assert(dppd_control_apply(&service, 0, &next, 0, &applied) == 0);
    assert(dppd_control_fini(&service) == 0);
    initialize(&service);
    fake.fail_create = 2;
    assert(dppd_control_persistence_restore(&service, path) == -ENOSPC);
    assert(service.recovery_state == DPPD_CONTROL_RECOVERY_READY && service.rules.count == 0);
    assert(dppd_rte_flow_backend_count(&service.rte_flow) == 0 &&
        dppd_rte_flow_backend_shared_count_count(&service.rte_flow) == 0);
    fake.fail_create = 0;
    assert(dppd_control_persistence_restore(&service, path) == 0);
    struct dppd_shared_count_info info;
    assert(dppd_control_shared_count_get(&service, 100, generation, &info) == 0 && info.references == 2);
    assert(service.rules.count == 2 && service.shared_count_revision == generation);
    assert(dppd_control_fini(&service) == 0);
    assert(unlink(path) == 0 && rmdir(directory) == 0);
}

/** 对象已创建后写盘失败必须保持 dirty，修复前不能继续追加实际对象 */
static void storage_failure(void)
{
    char directory[] = "/tmp/dppd-shared-dirty-XXXXXX", path[256];
    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/rules.bin", directory);
    struct dppd_control_service service;
    initialize(&service);
    assert(dppd_control_persistence_restore(&service, path) == 0);
    assert(unlink(path) == 0 && rmdir(directory) == 0);
    struct dppd_shared_count_spec spec = {.id = 100};
    struct dppd_shared_count_info info;
    assert(dppd_control_shared_count_create(&service, &spec, &info) == -EUCLEAN);
    assert(service.persistence_dirty && service.shared_count_revision == 1);
    assert(dppd_control_shared_count_get(&service, 100, 1, &info) == 0 && info.references == 0);
    spec.id = 200;
    assert(dppd_control_shared_count_create(&service, &spec, &info) == -EUCLEAN);
    assert(dppd_rte_flow_backend_shared_count_count(&service.rte_flow) == 1);
    assert(mkdir(directory, 0700) == 0 && dppd_control_persistence_flush(&service) == 0);
    assert(!service.persistence_dirty && dppd_control_fini(&service) == 0);
    initialize(&service);
    assert(dppd_control_persistence_restore(&service, path) == 0);
    assert(dppd_control_shared_count_get(&service, 100, 1, &info) == 0);
    assert(dppd_control_fini(&service) == 0);
    assert(unlink(path) == 0 && rmdir(directory) == 0);
}

int main(void)
{
    identity_and_paging();
    age_releases_reference();
    persisted_replay();
    storage_failure();
    return 0;
}
