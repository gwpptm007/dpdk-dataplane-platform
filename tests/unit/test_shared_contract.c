#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <rte_errno.h>
#include <rte_flow.h>
#include "dppd/offload.h"

static struct rte_flow_action_handle *const indirect = (struct rte_flow_action_handle *)(uintptr_t)0x1234;
static struct rte_flow *const flow = (struct rte_flow *)(uintptr_t)0x5678;
static struct dppd_shared_count_spec expected = {.id = UINT64_C(0x100000001), .generation = 19, .install_port_id = 7};
static unsigned int indirect_creates, indirect_queries, indirect_destroys, validates, creates;
static int create_errno, query_result, destroy_result, validate_result, flow_errno;
static unsigned int query_flags = 3;

struct rte_flow_action_handle *__wrap_rte_flow_action_handle_create(uint16_t port,
    const struct rte_flow_indir_action_conf *conf, const struct rte_flow_action *action, struct rte_flow_error *error);
int __wrap_rte_flow_action_handle_query(uint16_t port, const struct rte_flow_action_handle *handle, void *data,
    struct rte_flow_error *error);
int __wrap_rte_flow_action_handle_destroy(uint16_t port, struct rte_flow_action_handle *handle,
    struct rte_flow_error *error);
int __wrap_rte_flow_validate(uint16_t port, const struct rte_flow_attr *attr, const struct rte_flow_item *pattern,
    const struct rte_flow_action *actions, struct rte_flow_error *error);
struct rte_flow *__wrap_rte_flow_create(uint16_t port, const struct rte_flow_attr *attr,
    const struct rte_flow_item *pattern, const struct rte_flow_action *actions, struct rte_flow_error *error);

/** 在真正的 DPDK 入口核对参数，逻辑对象 ID 不能被截断后冒充驱动计数器编号 */
struct rte_flow_action_handle *__wrap_rte_flow_action_handle_create(uint16_t port,
    const struct rte_flow_indir_action_conf *conf, const struct rte_flow_action *action, struct rte_flow_error *error)
{
    ++indirect_creates;
    assert(port == expected.install_port_id);
    assert(conf->ingress == (expected.domain != DPPD_RULE_DOMAIN_EGRESS));
    assert(conf->egress == (expected.domain == DPPD_RULE_DOMAIN_EGRESS));
    assert(conf->transfer == (expected.domain == DPPD_RULE_DOMAIN_TRANSFER));
    assert(action->type == RTE_FLOW_ACTION_TYPE_COUNT && action->conf != NULL);
    assert(((const struct rte_flow_action_count *)action->conf)->id == 0);
    error->message = "indirect create unavailable";
    rte_errno = create_errno;
    return create_errno == 0 ? indirect : NULL;
}

int __wrap_rte_flow_action_handle_query(uint16_t port, const struct rte_flow_action_handle *handle, void *data,
    struct rte_flow_error *error)
{
    ++indirect_queries;
    assert(port == expected.install_port_id && handle == indirect);
    struct rte_flow_query_count *query = data;
    assert(!query->reset && !query->hits_set && !query->bytes_set && query->reserved == 0);
    assert(query->hits == 0 && query->bytes == 0);
    query->hits_set = (query_flags & 1) != 0;
    query->bytes_set = (query_flags & 2) != 0;
    query->hits = UINT64_C(0x100000003);
    query->bytes = UINT64_C(0x200000004);
    error->message = "indirect query rejected";
    return query_result;
}

int __wrap_rte_flow_action_handle_destroy(uint16_t port, struct rte_flow_action_handle *handle,
    struct rte_flow_error *error)
{
    ++indirect_destroys;
    assert(port == expected.install_port_id && handle == indirect);
    error->message = "indirect still referenced";
    return destroy_result;
}

static void check_rule(uint16_t port, const struct rte_flow_attr *attr, const struct rte_flow_item *pattern,
    const struct rte_flow_action *actions)
{
    assert(port == expected.install_port_id && attr->ingress && attr->transfer && !attr->egress);
    assert(pattern[0].type == RTE_FLOW_ITEM_TYPE_ETH && pattern[1].type == RTE_FLOW_ITEM_TYPE_END);
    assert(actions[0].type == RTE_FLOW_ACTION_TYPE_AGE);
    assert(((const struct rte_flow_action_age *)actions[0].conf)->timeout == 13);
    /** INDIRECT 的 conf 就是 opaque 本身，不能指向保存 opaque 的临时变量 */
    assert(actions[1].type == RTE_FLOW_ACTION_TYPE_INDIRECT && actions[1].conf == indirect);
    assert(actions[2].type == RTE_FLOW_ACTION_TYPE_DROP && actions[3].type == RTE_FLOW_ACTION_TYPE_END);
}

int __wrap_rte_flow_validate(uint16_t port, const struct rte_flow_attr *attr, const struct rte_flow_item *pattern,
    const struct rte_flow_action *actions, struct rte_flow_error *error)
{
    ++validates;
    check_rule(port, attr, pattern, actions);
    error->message = "bound validate rejected";
    return validate_result;
}

struct rte_flow *__wrap_rte_flow_create(uint16_t port, const struct rte_flow_attr *attr,
    const struct rte_flow_item *pattern, const struct rte_flow_action *actions, struct rte_flow_error *error)
{
    ++creates;
    check_rule(port, attr, pattern, actions);
    error->message = "bound create exhausted";
    rte_errno = flow_errno;
    return flow_errno == 0 ? flow : NULL;
}

static struct dppd_rule shared_rule(void)
{
    return (struct dppd_rule){.id = 1,
        .generation = 2,
        .install_port_id = 7,
        .domain = DPPD_RULE_DOMAIN_TRANSFER,
        .fallback = DPPD_FALLBACK_REQUIRE_HARDWARE,
        .nb_matches = 1,
        .matches = {{.type = DPPD_MATCH_ETH}},
        .nb_actions = 3,
        .actions = {{.type = DPPD_ACTION_AGE, .conf.age_timeout_seconds = 13},
            {.type = DPPD_ACTION_SHARED_COUNT, .conf.shared_count = {.id = UINT64_C(0x100000001), .generation = 19}},
            {.type = DPPD_ACTION_DROP}}};
}

static void object_contract(void)
{
    struct dppd_flow_error error;
    struct dppd_shared_count_handle handle = {0};
    for (int domain = DPPD_RULE_DOMAIN_INGRESS; domain <= DPPD_RULE_DOMAIN_TRANSFER; ++domain) {
        expected.domain = domain;
        assert(dppd_flow_shared_count_create(&expected, &handle, &error) == 0);
        assert(handle.opaque == indirect && handle.spec.id == expected.id && handle.spec.generation == 19);
    }
    struct dppd_shared_count_handle saved = handle;
    create_errno = ENOTSUP;
    assert(dppd_flow_shared_count_create(&expected, &handle, &error) == -ENOTSUP);
    assert(memcmp(&handle, &saved, sizeof(handle)) == 0 && strstr(error.message, "unavailable") != NULL);
    create_errno = 0;
    struct dppd_shared_count_spec invalid = expected;
    const uint64_t bad_generation[] = {0, UINT64_MAX};
    unsigned int calls = indirect_creates;
    for (unsigned int i = 0; i < 2; ++i) {
        invalid.generation = bad_generation[i];
        assert(dppd_flow_shared_count_create(&invalid, &handle, &error) == -EINVAL);
    }
    invalid = expected;
    invalid.id = 0;
    assert(dppd_flow_shared_count_create(&invalid, &handle, &error) == -EINVAL);
    invalid = expected;
    invalid.install_port_id = UINT16_MAX;
    assert(dppd_flow_shared_count_create(&invalid, &handle, &error) == -EINVAL);
    assert(indirect_creates == calls && memcmp(&handle, &saved, sizeof(handle)) == 0);
    for (query_flags = 0; query_flags < 4; ++query_flags) {
        uint64_t hits = 11, bytes = 22;
        int rc = dppd_flow_shared_count_query(&handle, &hits, &bytes, &error);
        assert(rc == (query_flags == 3 ? 0 : -ENODATA));
        assert(hits == (query_flags == 3 ? UINT64_C(0x100000003) : 11));
        assert(bytes == (query_flags == 3 ? UINT64_C(0x200000004) : 22));
    }
    uint64_t hits = 11, bytes = 22;
    query_result = -EIO;
    assert(dppd_flow_shared_count_query(&handle, &hits, &bytes, &error) == -EIO);
    assert(hits == 11 && bytes == 22 && strstr(error.message, "rejected") != NULL);
    destroy_result = -EBUSY;
    assert(dppd_flow_shared_count_destroy(&handle, &error) == -EBUSY);
    assert(memcmp(&handle, &saved, sizeof(handle)) == 0 && strstr(error.message, "referenced") != NULL);
    destroy_result = 0;
    assert(dppd_flow_shared_count_destroy(&handle, &error) == 0 && handle.opaque == NULL);
}

static void binding_contract(void)
{
    struct dppd_rule rule = shared_rule(), changed = rule;
    char reason[128];
    assert(dppd_rule_validate(&rule, reason, sizeof(reason)) == 0);
    changed.actions[1].conf.shared_count.generation++;
    assert(!dppd_rule_equal(&rule, &changed));
    changed = rule;
    changed.actions[1].conf.shared_count.id = 0;
    assert(dppd_rule_validate(&changed, reason, sizeof(reason)) != 0);
    changed.actions[1].conf.shared_count.id = UINT64_MAX;
    assert(dppd_rule_validate(&changed, reason, sizeof(reason)) != 0);
    changed = rule;
    changed.actions[1].conf.shared_count.generation = UINT64_MAX;
    assert(dppd_rule_validate(&changed, reason, sizeof(reason)) != 0);
    changed = rule;
    changed.actions[0].type = DPPD_ACTION_COUNT;
    assert(dppd_rule_validate(&changed, reason, sizeof(reason)) != 0);
    changed.actions[0] = rule.actions[1];
    assert(dppd_rule_validate(&changed, reason, sizeof(reason)) != 0);
    struct dppd_flow_binding binding = {.actions = {[1] = indirect}};
    struct dppd_flow_error error;
    assert(dppd_flow_validate(7, &rule, &error) == -ENOTSUP && validates == 0);
    assert(dppd_flow_validate_bound(7, &rule, NULL, &error) == -ENOTSUP && validates == 0);
    binding.actions[0] = indirect;
    assert(dppd_flow_validate_bound(7, &rule, &binding, &error) == -EINVAL && validates == 0);
    binding.actions[0] = NULL;
    binding.actions[DPPD_RULE_MAX_ACTIONS - 1] = indirect;
    assert(dppd_flow_validate_bound(7, &rule, &binding, &error) == -EINVAL && validates == 0);
    binding.actions[DPPD_RULE_MAX_ACTIONS - 1] = NULL;
    assert(dppd_flow_validate_bound(7, &rule, &binding, &error) == 0 && validates == 1);
    validate_result = -ENOTSUP;
    assert(dppd_flow_validate_bound(7, &rule, &binding, &error) == -ENOTSUP && creates == 0);
    struct dppd_flow_handle handle = {.rule_id = 99}, saved = handle;
    flow_errno = ENOSPC;
    assert(dppd_flow_create_bound(7, &rule, &binding, &handle, &error) == -ENOSPC);
    assert(memcmp(&handle, &saved, sizeof(handle)) == 0 && strstr(error.message, "exhausted") != NULL);
    flow_errno = 0;
    assert(dppd_flow_create_bound(7, &rule, &binding, &handle, &error) == 0);
    assert(handle.flow == flow && handle.has_age && !handle.has_count && handle.rule_generation == rule.generation);
}

int main(void)
{
    object_contract();
    binding_contract();
    assert(indirect_creates == 4 && indirect_queries == 5 && indirect_destroys == 2 && creates == 2);
    return 0;
}
