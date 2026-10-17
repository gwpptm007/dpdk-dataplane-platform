#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dppd/control.h"
#include "dppd/persistence.h"
#include "dppd/reset.h"

struct fake_counter {
    bool active;
    uint64_t token, hits, bytes;
    uint32_t references;
    struct dppd_shared_count_spec spec;
};

struct fake_flow {
    bool active, aged;
    uint64_t token, hits, bytes;
    uint32_t idle_seconds;
    struct dppd_rule rule;
    struct fake_counter *counter;
};

/** 每次创建只向后分配新 token，复位失效的旧指针永不复用，误用旧句柄会立即触发断言 */
static struct {
    struct fake_flow flows[128];
    struct fake_counter counters[64];
    uint32_t next_flow, next_counter;
    uint64_t next_token;
    unsigned int validates, creates, removes, shared_creates, shared_removes, resets, age_queries;
    unsigned int fail_create, fail_shared_create;
    int reset_error, remove_error, shared_remove_error, age_error;
} fake;

static struct fake_flow *checked_flow(const struct dppd_flow_handle *handle)
{
    struct fake_flow *flow = (struct fake_flow *)handle->flow;
    assert(flow != NULL && flow->active && flow->token != 0);
    assert(flow->rule.id == handle->rule_id && flow->rule.generation == handle->rule_generation);
    return flow;
}

static int fake_validate(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_error *error)
{
    (void)error;
    char reason[128];
    ++fake.validates;
    assert(port == 0 && dppd_rule_validate(rule, reason, sizeof(reason)) == 0);
    return 0;
}

static struct fake_counter *bound_counter(const struct dppd_rule *rule, const struct dppd_flow_binding *binding)
{
    struct fake_counter *counter = NULL;
    for (uint16_t i = 0; i < rule->nb_actions; ++i) {
        if (rule->actions[i].type != DPPD_ACTION_SHARED_COUNT) {
            assert(binding == NULL || binding->actions[i] == NULL);
            continue;
        }
        assert(binding != NULL && binding->actions[i] != NULL && counter == NULL);
        counter = (struct fake_counter *)binding->actions[i];
        assert(counter->active && counter->spec.id == rule->actions[i].conf.shared_count.id &&
            counter->spec.generation == rule->actions[i].conf.shared_count.generation);
    }
    return counter;
}

static int fake_validate_bound(uint16_t port, const struct dppd_rule *rule, const struct dppd_flow_binding *binding,
    struct dppd_flow_error *error)
{
    assert(bound_counter(rule, binding) != NULL);
    return fake_validate(port, rule, error);
}

static int fake_create_bound(uint16_t port, const struct dppd_rule *rule, const struct dppd_flow_binding *binding,
    struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    struct fake_counter *counter = bound_counter(rule, binding);
    if (++fake.creates == fake.fail_create)
        return -ENOSPC;
    assert(fake.next_flow < sizeof(fake.flows) / sizeof(*fake.flows));
    struct fake_flow *flow = &fake.flows[fake.next_flow++];
    *flow = (struct fake_flow){.active = true, .token = ++fake.next_token, .rule = *rule, .counter = counter};
    if (counter != NULL)
        ++counter->references;
    *handle = (struct dppd_flow_handle){.rule_id = rule->id,
        .rule_generation = rule->generation,
        .port_id = port,
        .flow = (struct rte_flow *)flow};
    for (uint16_t i = 0; i < rule->nb_actions; ++i) {
        handle->has_count |= rule->actions[i].type == DPPD_ACTION_COUNT;
        handle->has_age |= rule->actions[i].type == DPPD_ACTION_AGE;
    }
    return 0;
}

static int fake_create(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_handle *handle,
    struct dppd_flow_error *error)
{
    return fake_create_bound(port, rule, NULL, handle, error);
}

static int fake_remove(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    struct fake_flow *flow = checked_flow(handle);
    ++fake.removes;
    if (fake.remove_error != 0)
        return fake.remove_error;
    if (flow->counter != NULL) {
        assert(flow->counter->active && flow->counter->references != 0);
        --flow->counter->references;
    }
    flow->active = false;
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static int fake_query_count(const struct dppd_flow_handle *handle, uint64_t *hits, uint64_t *bytes,
    struct dppd_flow_error *error)
{
    (void)error;
    struct fake_flow *flow = checked_flow(handle);
    assert(handle->has_count);
    *hits = flow->hits;
    *bytes = flow->bytes;
    return 0;
}

static int fake_query_age(const struct dppd_flow_handle *handle, struct dppd_flow_age_result *result,
    struct dppd_flow_error *error)
{
    (void)error;
    struct fake_flow *flow = checked_flow(handle);
    assert(handle->has_age);
    ++fake.age_queries;
    if (fake.age_error != 0)
        return fake.age_error;
    *result = (struct dppd_flow_age_result){.aged = flow->aged,
        .sec_since_last_hit_valid = true,
        .sec_since_last_hit = flow->idle_seconds};
    return 0;
}

static int fake_shared_create(const struct dppd_shared_count_spec *spec, struct dppd_shared_count_handle *handle,
    struct dppd_flow_error *error)
{
    (void)error;
    if (++fake.shared_creates == fake.fail_shared_create)
        return -ENOSPC;
    assert(fake.next_counter < sizeof(fake.counters) / sizeof(*fake.counters));
    struct fake_counter *counter = &fake.counters[fake.next_counter++];
    *counter = (struct fake_counter){.active = true, .token = ++fake.next_token, .spec = *spec};
    *handle = (struct dppd_shared_count_handle){.spec = *spec, .opaque = (struct rte_flow_action_handle *)counter};
    return 0;
}

static int fake_shared_destroy(struct dppd_shared_count_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    struct fake_counter *counter = (struct fake_counter *)handle->opaque;
    assert(counter != NULL && counter->active && counter->references == 0);
    ++fake.shared_removes;
    if (fake.shared_remove_error != 0)
        return fake.shared_remove_error;
    counter->active = false;
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static int fake_shared_query(const struct dppd_shared_count_handle *handle, uint64_t *hits, uint64_t *bytes,
    struct dppd_flow_error *error)
{
    (void)error;
    struct fake_counter *counter = (struct fake_counter *)handle->opaque;
    assert(counter != NULL && counter->active);
    *hits = counter->hits;
    *bytes = counter->bytes;
    return 0;
}

/** 失败保持全部实际对象，成功一次性失效全部句柄，不经由逐项删除制造半复位 */
static int fake_reset(struct dppd_flow_error *error)
{
    (void)error;
    ++fake.resets;
    if (fake.reset_error != 0)
        return fake.reset_error;
    for (uint32_t i = 0; i < fake.next_flow; ++i)
        fake.flows[i].active = false;
    for (uint32_t i = 0; i < fake.next_counter; ++i) {
        fake.counters[i].active = false;
        fake.counters[i].references = 0;
    }
    return 0;
}

static struct dppd_topology topology(void)
{
    struct dppd_topology result = {.nb_endpoints = 1};
    result.endpoints[0] = (struct dppd_endpoint){.ethdev_port_id = 0,
        .kind = DPPD_ENDPOINT_ETHDEV,
        .has_switch_domain = true,
        .switch_domain_id = 3,
        .switch_port_id = 7,
        .role = DPPD_ENDPOINT_ROLE_UPLINK,
        .role_source = DPPD_ENDPOINT_ROLE_SOURCE_MLX5_PHYS_PORT_NAME,
        .if_index = 17,
        .has_controller = true,
        .has_pf = true,
        .has_function = true,
        .has_physical_port = true,
        .controller_id = 1,
        .pf_id = 2,
        .physical_port_id = 4};
    strcpy(result.endpoints[0].switch_name, "reset-test-switch");
    strcpy(result.endpoints[0].driver_name, "reset-test-driver");
    strcpy(result.endpoints[0].interface_name, "test0");
    strcpy(result.endpoints[0].device_name, "0000:03:00.0");
    strcpy(result.endpoints[0].phys_port_name, "c1pf2p4");
    return result;
}

static void initialize(struct dppd_control_service *control, const struct dppd_topology *ports)
{
    const struct dppd_flow_api api = {.validate = fake_validate,
        .create = fake_create,
        .remove = fake_remove,
        .query_count = fake_query_count,
        .query_age = fake_query_age,
        .shared_count_create = fake_shared_create,
        .shared_count_query = fake_shared_query,
        .shared_count_destroy = fake_shared_destroy,
        .validate_bound = fake_validate_bound,
        .create_bound = fake_create_bound,
        .simulate_reset = fake_reset};
    memset(&fake, 0, sizeof(fake));
    assert(dppd_control_init(control, ports, 8, &api) == 0);
}

static uint64_t add_shared(struct dppd_control_service *control, uint64_t id)
{
    struct dppd_shared_count_spec spec = {.id = id, .install_port_id = 0, .domain = DPPD_RULE_DOMAIN_INGRESS};
    struct dppd_shared_count_info result;
    assert(dppd_control_shared_count_create(control, &spec, &result) == 0);
    return result.spec.generation;
}

static uint64_t install(struct dppd_control_service *control, uint64_t id, uint64_t shared_generation, bool age)
{
    struct dppd_rule rule = {.id = id,
        .fallback = DPPD_FALLBACK_REQUIRE_HARDWARE,
        .nb_matches = 1,
        .nb_actions = age ? 3 : 2,
        .matches = {{.type = DPPD_MATCH_ETH}}};
    rule.actions[0].type = shared_generation == 0 ? DPPD_ACTION_COUNT : DPPD_ACTION_SHARED_COUNT;
    if (shared_generation != 0) {
        rule.actions[0].conf.shared_count.id = 100;
        rule.actions[0].conf.shared_count.generation = shared_generation;
    }
    if (age)
        rule.actions[1] = (struct dppd_action){.type = DPPD_ACTION_AGE, .conf.age_timeout_seconds = 5};
    rule.actions[rule.nb_actions - 1U].type = DPPD_ACTION_DROP;
    struct dppd_control_apply_result result;
    assert(dppd_control_apply(control, 0, &rule, 0, &result) == 0);
    return result.generation;
}

static struct fake_flow *installed(struct dppd_control_service *control, uint64_t id, uint64_t generation)
{
    const struct dppd_flow_handle *handle = dppd_rte_flow_backend_find_version(&control->rte_flow, id, generation);
    assert(handle != NULL);
    return checked_flow(handle);
}

static struct dppd_reset_status reset_status(const struct dppd_control_service *control)
{
    struct dppd_reset_status status;
    dppd_control_reset_status(control, &status);
    return status;
}

struct file_stamp {
    uint64_t hash;
    struct stat metadata;
};

/** 同时检查内容和 inode，恢复不能把相同内容重新写盘后冒充快照未变 */
static struct file_stamp stamp(const char *path)
{
    struct file_stamp result = {.hash = UINT64_C(14695981039346656037)};
    assert(stat(path, &result.metadata) == 0);
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    int byte;
    while ((byte = fgetc(file)) != EOF) {
        result.hash ^= (uint8_t)byte;
        result.hash *= UINT64_C(1099511628211);
    }
    assert(!ferror(file) && fclose(file) == 0);
    return result;
}

static void file_unchanged(const char *path, const struct file_stamp *before)
{
    struct file_stamp after = stamp(path);
    assert(after.hash == before->hash && after.metadata.st_ino == before->metadata.st_ino &&
        after.metadata.st_size == before->metadata.st_size &&
        after.metadata.st_mtim.tv_sec == before->metadata.st_mtim.tv_sec &&
        after.metadata.st_mtim.tv_nsec == before->metadata.st_mtim.tv_nsec);
}

static void desired_unchanged(struct dppd_control_service *control, const struct dppd_rule *rules, uint32_t count,
    uint64_t generation, uint64_t revision)
{
    assert(control->rules.count == count && control->rules.generation == generation &&
        control->shared_count_revision == revision);
    for (uint32_t i = 0; i < count; ++i) {
        struct dppd_rule current;
        assert(dppd_rule_repository_get(&control->rules, rules[i].id, &current) == 0);
        assert(current.generation == rules[i].generation && dppd_rule_equal(&current, &rules[i]));
    }
}

/** 真实默认接口和驱动拒绝均保持 ready，规则、引用和旧句柄继续可用 */
static void rejected_reset_preserves_actual(void)
{
    struct dppd_topology ports = topology();
    struct dppd_control_service control;
    initialize(&control, &ports);
    uint64_t shared = add_shared(&control, 100);
    uint64_t generation = install(&control, 10, shared, true);
    struct fake_flow *before = installed(&control, 10, generation);
    before->counter->hits = 9;
    before->counter->bytes = 126;
    control.rte_flow.api.simulate_reset = NULL;
    assert(dppd_control_simulate_reset(&control) == -ENOTSUP);
    assert(!reset_status(&control).pending && fake.resets == 0 && before->active);
    assert(control.recovery_state == DPPD_CONTROL_RECOVERY_READY);
    control.rte_flow.api.simulate_reset = fake_reset;
    fake.reset_error = -EIO;
    assert(dppd_control_simulate_reset(&control) == -EIO);
    assert(!reset_status(&control).pending && fake.resets == 1);
    assert(installed(&control, 10, generation) == before && before->counter->references == 1);
    assert(control.rules.count == 1 && control.shared_count_revision == shared);
    assert(control.recovery_state == DPPD_CONTROL_RECOVERY_READY);
    struct dppd_control_shared_count_result count;
    assert(
        dppd_control_shared_count_query(&control, 100, shared, &count) == 0 && count.hits == 9 && count.bytes == 126);
    assert(fake.removes == 0 && fake.shared_removes == 0);
    fake.reset_error = 0;
    assert(dppd_control_fini(&control) == 0);
}

static int before_create(void *context, uint16_t port, const struct dppd_rule *rule, uint64_t *attempt,
    uint8_t cookie[DPPD_TAP_COOKIE_SIZE])
{
    (void)context;
    (void)port;
    (void)rule;
    (void)attempt;
    (void)cookie;
    assert(!"reset eligibility must not invoke the native journal hook");
    return -ENOTSUP;
}

/** 有软件依赖、脏快照或原生恢复日志时在驱动调用前拒绝，不能复位到一半再发现不支持 */
static void eligibility_preflight(void)
{
    struct dppd_topology ports = topology();
    struct dppd_control_service control;
    initialize(&control, &ports);
    uint64_t generation = install(&control, 10, 0, false);
    struct fake_flow *flow = installed(&control, 10, generation);
    control.persistence_dirty = true;
    assert(dppd_control_simulate_reset(&control) < 0 && fake.resets == 0 && flow->active);
    control.persistence_dirty = false;
    control.rte_flow.before_create = before_create;
    assert(dppd_control_simulate_reset(&control) == -ENOTSUP && fake.resets == 0 && flow->active);
    control.rte_flow.before_create = NULL;
    control.conntrack = (struct dppd_conntrack *)&control;
    assert(dppd_control_simulate_reset(&control) == -ENOTSUP && fake.resets == 0);
    control.conntrack = NULL;
    control.nd6 = (struct dppd_nd6 *)&control;
    assert(dppd_control_simulate_reset(&control) == -ENOTSUP && fake.resets == 0);
    control.nd6 = NULL;
    control.routing = (struct dppd_routing_service *)&control;
    assert(dppd_control_simulate_reset(&control) == -ENOTSUP && fake.resets == 0);
    control.routing = NULL;
    assert(dppd_control_fini(&control) == 0);

    initialize(&control, &ports);
    struct dppd_rule software = {.id = 20,
        .fallback = DPPD_FALLBACK_SOFTWARE_ONLY,
        .nb_matches = 1,
        .nb_actions = 1,
        .matches = {{.type = DPPD_MATCH_ETH}},
        .actions = {{.type = DPPD_ACTION_DROP}}};
    struct dppd_control_apply_result applied;
    assert(dppd_control_apply(&control, 0, &software, 0, &applied) == 0);
    assert(dppd_control_simulate_reset(&control) == -ENOTSUP && fake.resets == 0 && control.rules.count == 1);
    assert(dppd_control_fini(&control) == 0);
}

/** 已知无法重建时连模拟驱动也不能调用，旧实际对象和共享引用始终保持可用 */
static void rebuild_capability_preflight(void)
{
    struct dppd_topology ports = topology();
    struct dppd_control_service control;
    initialize(&control, &ports);
    uint64_t shared = add_shared(&control, 100);
    uint64_t generation = install(&control, 10, shared, false);
    struct fake_flow *flow = installed(&control, 10, generation);
    uint64_t next_transaction = control.next_transaction_id;
    const uint64_t invalid_transaction_ids[] = {0, UINT64_MAX};
    for (uint32_t i = 0; i < sizeof(invalid_transaction_ids) / sizeof(invalid_transaction_ids[0]); ++i) {
        control.next_transaction_id = invalid_transaction_ids[i];
        assert(dppd_control_simulate_reset(&control) == -EOVERFLOW && fake.resets == 0 && flow->active &&
            flow->counter->references == 1 && !reset_status(&control).pending);
    }
    control.next_transaction_id = next_transaction;
    struct dppd_flow_api original = control.rte_flow.api;
    for (uint32_t missing = 0; missing < 7; ++missing) {
        control.rte_flow.api = original;
        switch (missing) {
        case 0:
            control.rte_flow.api.validate = NULL;
            break;
        case 1:
            control.rte_flow.api.create = NULL;
            break;
        case 2:
            control.rte_flow.api.remove = NULL;
            break;
        case 3:
            control.rte_flow.api.shared_count_create = NULL;
            break;
        case 4:
            control.rte_flow.api.shared_count_destroy = NULL;
            break;
        case 5:
            control.rte_flow.api.validate_bound = NULL;
            break;
        case 6:
            control.rte_flow.api.create_bound = NULL;
            break;
        }
        assert(dppd_control_simulate_reset(&control) == -ENOTSUP && fake.resets == 0 &&
            installed(&control, 10, generation) == flow && flow->counter->references == 1);
        assert(control.rules.count == 1 && control.rules.generation == generation &&
            control.shared_count_revision == shared && control.next_transaction_id == next_transaction &&
            control.recovery_state == DPPD_CONTROL_RECOVERY_READY && !reset_status(&control).pending);
    }
    control.rte_flow.api = original;
    assert(fake.removes == 0 && fake.shared_removes == 0);
    assert(dppd_control_fini(&control) == 0);
}

/** 正式快照和代次都保持不变，恢复先重建共享对象再绑定规则，COUNT 与 AGE 从新对象开始 */
static void persisted_recovery(void)
{
    char directory[] = "/tmp/dppd-reset-unit-XXXXXX", path[256];
    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/rules.bin", directory);
    struct dppd_topology ports = topology();
    struct dppd_control_service control;
    initialize(&control, &ports);
    assert(dppd_control_persistence_restore(&control, path) == 0);
    uint64_t shared = add_shared(&control, 100);
    (void)add_shared(&control, 200);
    uint64_t generations[] = {install(&control, 10, shared, true), install(&control, 20, shared, false),
        install(&control, 30, 0, true)};
    struct dppd_rule desired[3];
    struct fake_flow *old[3];
    for (uint32_t i = 0; i < 3; ++i) {
        old[i] = installed(&control, 10U * (i + 1U), generations[i]);
        old[i]->hits = 17;
        old[i]->bytes = 238;
        old[i]->idle_seconds = 4;
        assert(dppd_rule_repository_get(&control.rules, old[i]->rule.id, &desired[i]) == 0);
    }
    old[0]->counter->hits = 29;
    old[0]->counter->bytes = 406;
    fake.age_error = -EIO;
    assert(dppd_control_age_poll(&control, 0, 8) == -EIO && control.age.last_error == -EIO);
    uint64_t repository_generation = control.rules.generation, revision = control.shared_count_revision;
    struct file_stamp before = stamp(path);
    assert(dppd_control_reset_recover(&control) == -EALREADY);
    assert(dppd_control_simulate_reset(&control) == 0);
    struct dppd_reset_status status = reset_status(&control);
    assert(status.pending && status.resets == 1 && status.desired_rules == 3 && status.desired_shared == 2 &&
        status.actual_rules == 0 && status.actual_shared == 0);
    assert(control.recovery_state != DPPD_CONTROL_RECOVERY_READY);
    for (uint32_t i = 0; i < 3; ++i)
        assert(!old[i]->active);
    assert(fake.removes == 0 && fake.shared_removes == 0);
    assert(dppd_control_reconciliation_retry(&control) == -EBUSY);
    assert(dppd_control_simulate_reset(&control) < 0 && fake.resets == 1);
    assert(dppd_control_age_poll(&control, UINT64_C(5000000000), 8) == -EUCLEAN);
    desired_unchanged(&control, desired, 3, repository_generation, revision);
    file_unchanged(path, &before);
    fake.age_error = 0;
    assert(dppd_control_reset_recover(&control) == 0);
    status = reset_status(&control);
    assert(!status.pending && status.recoveries == 1 && status.attempts == 1 && status.last_error == 0 &&
        status.cleanup_error == 0 && status.actual_rules == 3 && status.actual_shared == 2);
    assert(control.recovery_state == DPPD_CONTROL_RECOVERY_READY && control.age.last_error == 0 &&
        control.age_cycle_error == 0);
    for (uint32_t i = 0; i < 3; ++i) {
        struct fake_flow *current = installed(&control, desired[i].id, generations[i]);
        assert(current != old[i] && current->token > old[i]->token && current->hits == 0 && current->idle_seconds == 0);
    }
    struct dppd_control_shared_count_result count;
    assert(dppd_control_shared_count_query(&control, 100, shared, &count) == 0 && count.hits == 0 && count.bytes == 0 &&
        count.object.references == 2 && count.object.spec.generation == shared);
    struct dppd_control_count_result private_count;
    assert(dppd_control_query_count(&control, 30, generations[2], &private_count) == 0 && private_count.hits == 0 &&
        private_count.bytes == 0);
    struct dppd_control_age_result age;
    assert(dppd_control_query_age(&control, 10, generations[0], &age) == 0 && !age.state.aged &&
        age.state.sec_since_last_hit_valid && age.state.sec_since_last_hit == 0);
    desired_unchanged(&control, desired, 3, repository_generation, revision);
    file_unchanged(path, &before);
    struct dppd_persisted_snapshot snapshot;
    assert(dppd_persistence_load(path, &snapshot) == 0 && snapshot.count == 3 && snapshot.shared_count == 2 &&
        snapshot.repository_generation == repository_generation && snapshot.shared_revision == revision);
    dppd_persisted_snapshot_destroy(&snapshot);
    assert(dppd_control_fini(&control) == 0);
    file_unchanged(path, &before);
    assert(unlink(path) == 0 && rmdir(directory) == 0);
}

/** 拓扑核对必须发生在创建前，端点的每个身份字段改变都不能继续使用旧计划 */
static void topology_mismatch(void)
{
    struct dppd_topology original = topology(), current = original;
    struct dppd_control_service control;
    initialize(&control, &current);
    uint64_t generation = install(&control, 10, 0, false);
    assert(dppd_control_simulate_reset(&control) == 0);
    unsigned int creates = fake.creates, shared_creates = fake.shared_creates;
    current.nb_endpoints = 0;
    assert(dppd_control_reset_recover(&control) == -ENOENT);
    assert(fake.creates == creates && fake.shared_creates == shared_creates && reset_status(&control).pending);
    for (uint32_t field = 0; field < 23; ++field) {
        current = original;
        struct dppd_endpoint *port = &current.endpoints[0];
        switch (field) {
        case 0:
            port->kind = DPPD_ENDPOINT_REPRESENTOR;
            break;
        case 1:
            port->has_switch_domain = false;
            break;
        case 2:
            port->switch_domain_id++;
            break;
        case 3:
            port->switch_port_id++;
            break;
        case 4:
            port->switch_name[0] ^= 1;
            break;
        case 5:
            port->driver_name[0] ^= 1;
            break;
        case 6:
            port->role = DPPD_ENDPOINT_ROLE_PF;
            break;
        case 7:
            port->role_source = DPPD_ENDPOINT_ROLE_SOURCE_UNKNOWN;
            break;
        case 8:
            port->role_error = -EIO;
            break;
        case 9:
            port->if_index++;
            break;
        case 10:
            port->has_controller = false;
            break;
        case 11:
            port->has_pf = false;
            break;
        case 12:
            port->has_function = false;
            break;
        case 13:
            port->has_physical_port = false;
            break;
        case 14:
            port->controller_id++;
            break;
        case 15:
            port->pf_id++;
            break;
        case 16:
            port->function_id++;
            break;
        case 17:
            port->physical_port_id++;
            break;
        case 18:
            port->interface_name[0] ^= 1;
            break;
        case 19:
            port->device_name[0] ^= 1;
            break;
        case 20:
            port->phys_port_name[0] ^= 1;
            break;
        case 21:
            port->ethdev_port_id = 1;
            break;
        case 22:
            current.nb_endpoints = 2;
            current.endpoints[1] = *port;
            break;
        }
        int rc = dppd_control_reset_recover(&control);
        assert(rc == (field == 21 ? -ENOENT : field == 22 ? -EUCLEAN : -EXDEV));
        assert(fake.creates == creates && fake.shared_creates == shared_creates && reset_status(&control).pending);
    }
    /* 额外未使用端点和发现顺序不属于旧计划身份，合法变化仍能恢复 */
    current = original;
    current.nb_endpoints = 2;
    current.endpoints[1] = original.endpoints[0];
    current.endpoints[0].ethdev_port_id = 7;
    strcpy(current.endpoints[0].device_name, "unused-device");
    assert(dppd_control_reset_recover(&control) == 0);
    assert(installed(&control, 10, generation)->active && control.persistence_path == NULL);
    assert(dppd_control_fini(&control) == 0);

    /* 未声明的数值和字符串结尾后的存储字节没有身份含义，不能按整块内存误判变化 */
    current = original;
    current.endpoints[0].has_switch_domain = false;
    current.endpoints[0].has_controller = false;
    current.endpoints[0].has_pf = false;
    current.endpoints[0].has_function = false;
    current.endpoints[0].has_physical_port = false;
    initialize(&control, &current);
    generation = install(&control, 10, 0, false);
    assert(dppd_control_simulate_reset(&control) == 0);
    current.endpoints[0].switch_domain_id++;
    current.endpoints[0].switch_port_id++;
    current.endpoints[0].controller_id++;
    current.endpoints[0].pf_id++;
    current.endpoints[0].function_id++;
    current.endpoints[0].physical_port_id++;
    current.endpoints[0].device_name[sizeof(current.endpoints[0].device_name) - 1U] = 'x';
    assert(dppd_control_reset_recover(&control) == 0 && installed(&control, 10, generation)->active);
    assert(dppd_control_fini(&control) == 0);
}

/** 创建失败清理完后仍保留恢复计划，补偿失败则保留真实引用，下一次先清理再重建 */
static void failed_recovery_retries(void)
{
    for (uint32_t failure = 0; failure < 4; ++failure) {
        struct dppd_topology ports = topology();
        struct dppd_control_service control;
        initialize(&control, &ports);
        uint64_t shared = add_shared(&control, 100);
        uint64_t first = install(&control, 10, shared, true), second = install(&control, 20, shared, false);
        uint64_t revision = control.shared_count_revision, generation = control.rules.generation;
        assert(dppd_control_simulate_reset(&control) == 0);
        if (failure == 0)
            fake.fail_shared_create = fake.shared_creates + 1U;
        else
            fake.fail_create = fake.creates + 2U;
        if (failure == 2)
            fake.remove_error = -EBUSY;
        if (failure == 3)
            fake.shared_remove_error = -EIO;
        int rc = dppd_control_reset_recover(&control);
        assert(rc == (failure >= 2 ? -EUCLEAN : -ENOSPC));
        struct dppd_reset_status status = reset_status(&control);
        assert(status.pending && status.last_error == -ENOSPC && status.recoveries == 0);
        assert(control.rules.count == 2 && control.rules.generation == generation &&
            control.shared_count_revision == revision && control.persistence_path == NULL);
        assert(dppd_control_reconciliation_retry(&control) == -EBUSY);
        if (failure == 2) {
            assert(status.cleanup_error == -EBUSY && status.actual_rules == 1 && status.actual_shared == 1);
            struct fake_flow *residual = installed(&control, 10, first);
            assert(residual->counter->references == 1);
            struct dppd_shared_count_info info;
            assert(dppd_rte_flow_backend_shared_count_get(&control.rte_flow, 100, shared, &info) == 0 &&
                info.references == 1);
        }
        if (failure == 3)
            assert(status.cleanup_error == -EIO && status.actual_rules == 0 && status.actual_shared == 1);
        if (failure < 2)
            assert(status.cleanup_error == 0 && status.actual_rules == 0 && status.actual_shared == 0);
        fake.remove_error = fake.shared_remove_error = 0;
        fake.fail_create = fake.fail_shared_create = 0;
        assert(dppd_control_reset_recover(&control) == 0);
        status = reset_status(&control);
        assert(!status.pending && status.attempts == 2 && status.recoveries == 1 && status.last_error == 0 &&
            status.cleanup_error == 0 && status.actual_rules == 2 && status.actual_shared == 1);
        assert(installed(&control, 10, first)->active && installed(&control, 20, second)->active);
        struct dppd_control_shared_count_result count;
        assert(dppd_control_shared_count_query(&control, 100, shared, &count) == 0 && count.object.references == 2 &&
            count.hits == 0);
        assert(control.rules.generation == generation && control.shared_count_revision == revision);
        assert(dppd_control_fini(&control) == 0);
    }
}

/** 安装端口没变也要核对 represented 源与目标，不能把旧转发计划接到另一块设备 */
static void transfer_endpoint_preflight(void)
{
    struct dppd_topology ports = topology();
    ports.nb_endpoints = 3;
    for (uint16_t i = 1; i < 3; ++i) {
        ports.endpoints[i] = ports.endpoints[0];
        ports.endpoints[i].ethdev_port_id = i;
        ports.endpoints[i].kind = DPPD_ENDPOINT_REPRESENTOR;
        ports.endpoints[i].switch_port_id = 10U + i;
        ports.endpoints[i].function_id = i;
        ports.endpoints[i].role = DPPD_ENDPOINT_ROLE_VF;
    }
    struct dppd_topology original = ports;
    struct dppd_control_service control;
    initialize(&control, &ports);
    struct dppd_rule rule = {.id = 10,
        .domain = DPPD_RULE_DOMAIN_TRANSFER,
        .fallback = DPPD_FALLBACK_REQUIRE_HARDWARE,
        .nb_matches = 2,
        .nb_actions = 1,
        .matches = {{.type = DPPD_MATCH_ETH}, {.type = DPPD_MATCH_REPRESENTED_PORT, .spec.ethdev_port_id = 1}},
        .actions = {{.type = DPPD_ACTION_REPRESENTED_PORT, .conf.ethdev_port_id = 2}}};
    struct dppd_control_apply_result applied;
    assert(dppd_control_apply(&control, 0, &rule, 0, &applied) == 0);
    uint64_t generation = applied.generation;
    assert(dppd_control_simulate_reset(&control) == 0);
    unsigned int creates = fake.creates;
    ports.endpoints[1].device_name[0] ^= 1;
    assert(dppd_control_reset_recover(&control) == -EXDEV && fake.creates == creates);
    ports = original;
    ports.endpoints[2].device_name[0] ^= 1;
    assert(dppd_control_reset_recover(&control) == -EXDEV && fake.creates == creates);
    ports = original;
    ports.nb_endpoints = 2;
    assert(dppd_control_reset_recover(&control) == -ENOENT && fake.creates == creates);
    ports = original;
    assert(dppd_control_reset_recover(&control) == 0 && installed(&control, 10, generation)->active);
    assert(dppd_control_fini(&control) == 0);

    /* 接口缺失在共享对象创建前拒绝，恢复接口后同一计划仍然可以重试 */
    ports = topology();
    initialize(&control, &ports);
    uint64_t shared = add_shared(&control, 100);
    generation = install(&control, 10, shared, false);
    assert(dppd_control_simulate_reset(&control) == 0);
    unsigned int shared_creates = fake.shared_creates;
    creates = fake.creates;
    control.rte_flow.api.validate_bound = NULL;
    assert(dppd_control_reset_recover(&control) == -ENOTSUP && fake.shared_creates == shared_creates &&
        fake.creates == creates);
    control.rte_flow.api.validate_bound = fake_validate_bound;
    assert(dppd_control_reset_recover(&control) == 0 && installed(&control, 10, generation)->active);
    assert(dppd_control_fini(&control) == 0);
}

/** 隔离期间正常退出只释放内存计划，不能对复位前已失效的句柄逐项删除 */
static void pending_finalization(void)
{
    struct dppd_topology ports = topology();
    struct dppd_control_service control;
    initialize(&control, &ports);
    uint64_t shared = add_shared(&control, 100);
    (void)install(&control, 10, shared, true);
    assert(dppd_control_simulate_reset(&control) == 0);
    assert(dppd_control_fini(&control) == 0);
    assert(fake.removes == 0 && fake.shared_removes == 0);
}

int main(void)
{
    rejected_reset_preserves_actual();
    eligibility_preflight();
    rebuild_capability_preflight();
    persisted_recovery();
    topology_mismatch();
    failed_recovery_retries();
    transfer_endpoint_preflight();
    pending_finalization();
    return 0;
}
