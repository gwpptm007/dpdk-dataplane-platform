#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dppd/transfer.h"

/** 这里只替换驱动接口，图构造、正式控制事务、账本和恢复隔离均使用产品代码 */
static struct {
    unsigned int validations, creates, removes, queries;
    int fail_validate_at, fail_create_at, fail_remove_at;
    struct dppd_rule created[16];
    uint64_t removed[16];
} fake;

static int fake_validate(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_error *error)
{
    (void)error;
    assert((port == 10 || port == 17) && rule->install_port_id == port);
    assert(rule->domain == DPPD_RULE_DOMAIN_TRANSFER && rule->fallback == DPPD_FALLBACK_REQUIRE_HARDWARE);
    assert(rule->generation != 0 && rule->generation != DPPD_RULE_GENERATION_ANY);
    return (int)fake.validations++ == fake.fail_validate_at ? -ENOTSUP : 0;
}

static int fake_create(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_handle *handle,
    struct dppd_flow_error *error)
{
    (void)error;
    unsigned int call = fake.creates++;
    assert(call < 16 && (port == 10 || port == 17));
    fake.created[call] = *rule;
    memset(handle, 0, sizeof(*handle));
    if ((int)call == fake.fail_create_at)
        return -ENOSPC;
    handle->rule_id = rule->id;
    handle->rule_generation = rule->generation;
    handle->port_id = port;
    handle->flow = (struct rte_flow *)(uintptr_t)(call + 1U);
    for (uint16_t i = 0; i < rule->nb_actions; ++i)
        handle->has_count |= rule->actions[i].type == DPPD_ACTION_COUNT;
    return 0;
}

static int fake_remove(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    unsigned int call = fake.removes++;
    assert(call < 16 && handle->flow != NULL);
    fake.removed[call] = handle->rule_id;
    if ((int)call == fake.fail_remove_at)
        return -EBUSY;
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static int fake_query(const struct dppd_flow_handle *handle, uint64_t *hits, uint64_t *bytes,
    struct dppd_flow_error *error)
{
    (void)error;
    assert(handle->has_count && handle->flow != NULL && handle->port_id == 10);
    fake.queries++;
    *hits = UINT64_C(0x100000003);
    *bytes = UINT64_C(0x200000007);
    return 0;
}

static const struct dppd_flow_api api = {.validate = fake_validate,
    .create = fake_create,
    .remove = fake_remove,
    .query_count = fake_query};

static void reset_fake(void)
{
    memset(&fake, 0, sizeof(fake));
    fake.fail_validate_at = fake.fail_create_at = fake.fail_remove_at = -1;
}

/** 名称通过正式解析器生成角色来源，拓扑仍是夹具，不代表机器上存在这些设备 */
static struct dppd_topology make_topology(void)
{
    static const char *const names[] = {"p0", "c0pf0", "c0pf0vf1", "c0pf0vf2", "c0pf0sf1", "c0pf0sf2", "", "p1",
        "c1pf1vf3"};
    struct dppd_topology topology = {.nb_endpoints = 9};
    for (uint16_t i = 0; i < topology.nb_endpoints; ++i) {
        struct dppd_endpoint *endpoint = &topology.endpoints[i];
        bool representor = i != 0 && i != 6 && i != 7;
        endpoint->ethdev_port_id = 10 + i;
        endpoint->kind = representor ? DPPD_ENDPOINT_REPRESENTOR : DPPD_ENDPOINT_ETHDEV;
        endpoint->has_switch_domain = true;
        endpoint->switch_domain_id = i == 7 ? 88 : 77;
        endpoint->switch_port_id = 100 + i;
        if (i == 6)
            endpoint->role_error = -ENOTSUP;
        else
            assert(dppd_topology_resolve_role(endpoint, "net_mlx5", representor, names[i]) == 0);
    }
    return topology;
}

static struct dppd_transfer_spec make_spec(void)
{
    return (struct dppd_transfer_spec){.first_rule_id = 100,
        .expected_repository_generation = DPPD_RULE_GENERATION_ANY,
        .priority = 9,
        .install_port_id = 10,
        .source_port_id = 10,
        .destination_port_id = 12,
        .scenario = DPPD_TRANSFER_PAIR,
        .bidirectional = 1,
        .with_count = 1};
}

static void check_graph(const struct dppd_transfer_graph *graph, const struct dppd_transfer_spec *spec)
{
    assert(graph->count == (spec->bidirectional ? 2 : 1) && graph->switch_domain_id == 77);
    for (uint16_t i = 0; i < graph->count; ++i) {
        const struct dppd_rule *rule = &graph->rules[i];
        assert(rule->id == spec->first_rule_id + i && rule->install_port_id == spec->install_port_id);
        assert(rule->domain == DPPD_RULE_DOMAIN_TRANSFER && rule->fallback == DPPD_FALLBACK_REQUIRE_HARDWARE);
        assert(rule->group == 0 && rule->priority == spec->priority && rule->nb_matches == 2);
        assert(rule->matches[0].type == DPPD_MATCH_REPRESENTED_PORT && rule->matches[1].type == DPPD_MATCH_ETH);
        assert(rule->matches[0].spec.ethdev_port_id == (i == 0 ? spec->source_port_id : spec->destination_port_id));
        assert(rule->nb_actions == (spec->with_count ? 2 : 1));
        if (spec->with_count)
            assert(rule->actions[0].type == DPPD_ACTION_COUNT);
        assert(rule->actions[rule->nb_actions - 1].type == DPPD_ACTION_REPRESENTED_PORT);
        assert(rule->actions[rule->nb_actions - 1].conf.ethdev_port_id ==
            (i == 0 ? spec->destination_port_id : spec->source_port_id));
        assert(graph->plans[i].rule_id == rule->id && graph->plans[i].rule_generation == rule->generation);
        assert(graph->plans[i].backend == DPPD_PLAN_BACKEND_RTE_FLOW && graph->plans[i].requires_validation);
        assert(!graph->plans[i].fallback_used);
    }
}

/** 每个场景分别验证单向和双向，规划没有驱动副作用，安装回执与实际账本版本一致 */
static void templates(void)
{
    static const uint16_t source[] = {16, 10, 10, 11, 11, 12, 14};
    static const uint16_t destination[] = {12, 12, 14, 12, 14, 13, 15};
    struct dppd_topology topology = make_topology();
    for (uint8_t scenario = 0; scenario < DPPD_TRANSFER_TEMPLATE_COUNT; ++scenario)
        for (uint8_t bidirectional = 0; bidirectional < 2; ++bidirectional) {
            struct dppd_control_service control;
            struct dppd_transfer_graph graph;
            struct dppd_control_apply_result results[2];
            struct dppd_transfer_spec spec = make_spec();
            struct dppd_control_count_result count;
            char message[160];
            spec.scenario = scenario;
            spec.source_port_id = source[scenario];
            spec.destination_port_id = destination[scenario];
            spec.bidirectional = bidirectional;
            spec.with_count = bidirectional;
            reset_fake();
            assert(dppd_control_init(&control, &topology, 8, &api) == 0);
            assert(dppd_control_transfer(&control, &spec, false, &graph, results, message, sizeof(message)) == 0);
            check_graph(&graph, &spec);
            assert(graph.repository_generation == 0 && fake.validations == 0 && fake.creates == 0);
            assert(dppd_rule_repository_generation(&control.rules) == 0);
            spec.expected_repository_generation = graph.repository_generation;
            assert(dppd_control_transfer(&control, &spec, true, &graph, results, message, sizeof(message)) == 0);
            check_graph(&graph, &spec);
            assert(graph.repository_generation == graph.count);
            assert(fake.validations == graph.count && fake.creates == graph.count && fake.removes == 0);
            assert(dppd_rule_repository_count(&control.rules) == graph.count);
            assert(dppd_rte_flow_backend_count(&control.rte_flow) == graph.count);
            for (uint16_t i = 0; i < graph.count; ++i) {
                struct dppd_rule stored;
                assert(dppd_rule_repository_get(&control.rules, spec.first_rule_id + i, &stored) == 0);
                assert(stored.generation == i + 1U && graph.rules[i].generation == stored.generation);
                assert(dppd_rule_equal(&stored, &fake.created[i]));
                assert(results[i].generation == stored.generation && results[i].status == DPPD_RULE_CREATED);
                assert(results[i].transaction_id == 1);
                int rc = dppd_control_query_count(&control, stored.id, stored.generation, &count);
                if (spec.with_count)
                    assert(rc == 0 && count.hits == UINT64_C(0x100000003) && count.bytes == UINT64_C(0x200000007));
                else
                    assert(rc == -ENODATA);
            }
            assert(fake.queries == (spec.with_count ? graph.count : 0));
            unsigned int creates = fake.creates, validations = fake.validations;
            spec.first_rule_id = 200;
            assert(dppd_control_transfer(&control, &spec, true, &graph, results, message, sizeof(message)) == -ESTALE);
            assert(fake.creates == creates && fake.validations == validations);
            assert(dppd_control_fini(&control) == 0);
        }
}

/** 非法图失败时保留调用方的旧输出，不能把只构造到一半的图冒充新结果 */
static void expect_invalid_graph(const struct dppd_planner_context *context, const struct dppd_transfer_spec *spec,
    int expected_error)
{
    struct dppd_transfer_graph graph, saved;
    char message[160];
    memset(&graph, 0xa5, sizeof(graph));
    saved = graph;
    int rc = dppd_transfer_graph_build(context, spec, &graph, message, sizeof(message));
    assert(expected_error == 0 ? rc != 0 : rc == expected_error);
    assert(memcmp(&graph, &saved, sizeof(graph)) == 0 && message[0] != '\0');
}

/** 只允许明确的端点和可信角色，同名模板不能靠 UNKNOWN 或矛盾的代表标志凑出来 */
static void invalid_graphs(void)
{
    struct dppd_topology topology = make_topology();
    struct dppd_planner_context context = {.topology = &topology, .hardware_available = true};
    struct dppd_transfer_spec spec = make_spec(), changed;
    changed = spec;
    changed.source_port_id = changed.destination_port_id;
    expect_invalid_graph(&context, &changed, -EINVAL);
    changed = spec;
    changed.destination_port_id = 99;
    expect_invalid_graph(&context, &changed, -ENOENT);
    changed = spec;
    changed.install_port_id = 99;
    expect_invalid_graph(&context, &changed, -ENOENT);
    changed = spec;
    changed.destination_port_id = 17;
    expect_invalid_graph(&context, &changed, -EXDEV);
    changed = spec;
    changed.first_rule_id = 0;
    expect_invalid_graph(&context, &changed, -EINVAL);
    changed.first_rule_id = UINT64_MAX;
    expect_invalid_graph(&context, &changed, 0);
    changed.first_rule_id = UINT64_MAX - 1U;
    expect_invalid_graph(&context, &changed, -EOVERFLOW);
    changed = spec;
    changed.scenario = DPPD_TRANSFER_TEMPLATE_COUNT;
    expect_invalid_graph(&context, &changed, -EINVAL);
    changed = spec;
    changed.bidirectional = 2;
    expect_invalid_graph(&context, &changed, -EINVAL);
    changed = spec;
    changed.with_count = 2;
    expect_invalid_graph(&context, &changed, -EINVAL);
    for (unsigned int i = 0; i < sizeof(spec.reserved); ++i) {
        changed = spec;
        changed.reserved[i] = 1;
        expect_invalid_graph(&context, &changed, -EINVAL);
    }
    changed = spec;
    changed.scenario = DPPD_TRANSFER_UPLINK_VF;
    changed.source_port_id = 16;
    expect_invalid_graph(&context, &changed, -ENOTSUP);
    changed.source_port_id = 10;
    topology.endpoints[0].role_source = DPPD_ENDPOINT_ROLE_SOURCE_UNKNOWN;
    expect_invalid_graph(&context, &changed, -ENOTSUP);
    topology = make_topology();
    topology.endpoints[0].role_source = (enum dppd_endpoint_role_source)99;
    expect_invalid_graph(&context, &changed, -ENOTSUP);
    topology = make_topology();
    topology.endpoints[2].role_source = (enum dppd_endpoint_role_source) - 1;
    expect_invalid_graph(&context, &changed, -ENOTSUP);
    topology = make_topology();
    topology.endpoints[2].role_error = -EIO;
    expect_invalid_graph(&context, &changed, -ENOTSUP);
    topology = make_topology();
    topology.endpoints[2].kind = DPPD_ENDPOINT_ETHDEV;
    expect_invalid_graph(&context, &changed, -ENOTSUP);
    topology = make_topology();
    topology.endpoints[0].kind = DPPD_ENDPOINT_REPRESENTOR;
    expect_invalid_graph(&context, &changed, -ENOTSUP);
    topology = make_topology();
    changed.scenario = DPPD_TRANSFER_PF_VF;
    changed.source_port_id = 11;
    topology.endpoints[2].pf_id = 1;
    expect_invalid_graph(&context, &changed, 0);
    topology = make_topology();
    topology.endpoints[2].controller_id = 1;
    expect_invalid_graph(&context, &changed, 0);
    topology = make_topology();
    topology.endpoints[2].has_switch_domain = false;
    expect_invalid_graph(&context, &spec, -ENOTSUP);
    topology = make_topology();
    topology.endpoints[2].switch_port_id = topology.endpoints[0].switch_port_id;
    expect_invalid_graph(&context, &spec, -EINVAL);
    topology = make_topology();
    changed.scenario = DPPD_TRANSFER_PF_SF;
    changed.destination_port_id = 14;
    topology.endpoints[4].pf_id = 1;
    expect_invalid_graph(&context, &changed, -EXDEV);
    topology = make_topology();
    context.hardware_available = false;
    expect_invalid_graph(&context, &spec, -ENOTSUP);
    context.hardware_available = true;
    context.ipv4_routing = true;
    expect_invalid_graph(&context, &spec, -ENOTSUP);
    context.ipv4_routing = false;
    context.connection_tracking = true;
    expect_invalid_graph(&context, &spec, -ENOTSUP);
    context.connection_tracking = false;
    context.neighbor_discovery = true;
    expect_invalid_graph(&context, &spec, -ENOTSUP);
    assert(fake.validations == 0 && fake.creates == 0 && fake.removes == 0);
}

/** 版本条件必须先于驱动调用，耗尽时不能先安装对象再发现版本号已经不能发布 */
static void versions(void)
{
    struct dppd_topology topology = make_topology();
    struct dppd_control_service control;
    struct dppd_transfer_spec spec = make_spec();
    struct dppd_transfer_graph graph;
    struct dppd_control_apply_result results[2];
    char message[160];
    reset_fake();
    assert(dppd_control_init(&control, &topology, 8, &api) == 0);
    assert(dppd_control_transfer(&control, &spec, true, &graph, results, message, sizeof(message)) == -ESTALE);
    spec.expected_repository_generation = 1;
    assert(dppd_control_transfer(&control, &spec, true, &graph, results, message, sizeof(message)) == -ESTALE);
    assert(dppd_control_transfer(&control, &spec, false, &graph, results, message, sizeof(message)) == -ESTALE);
    for (uint64_t remaining = 1; remaining <= 2; ++remaining) {
        control.rules.generation = UINT64_MAX - remaining;
        spec.expected_repository_generation = control.rules.generation;
        assert(dppd_control_transfer(&control, &spec, true, &graph, results, message, sizeof(message)) == -EOVERFLOW);
        assert(dppd_rule_repository_count(&control.rules) == 0);
        assert(control.rules.generation == UINT64_MAX - remaining);
    }
    assert(fake.validations == 0 && fake.creates == 0 && fake.removes == 0);
    assert(dppd_control_fini(&control) == 0);
}

/** 全批校验先于创建，创建中途失败才撤销首条，撤销失败则留下 handle 并封锁后续写入 */
static void failures(void)
{
    struct dppd_topology topology = make_topology();
    for (unsigned int mode = 0; mode < 3; ++mode) {
        struct dppd_control_service control;
        struct dppd_transfer_spec spec = make_spec();
        struct dppd_transfer_graph graph;
        struct dppd_control_apply_result results[2];
        struct dppd_control_recovery_status recovery;
        char message[160];
        reset_fake();
        if (mode == 0)
            fake.fail_validate_at = 1;
        else
            fake.fail_create_at = 1;
        if (mode == 2)
            fake.fail_remove_at = 0;
        spec.expected_repository_generation = 0;
        assert(dppd_control_init(&control, &topology, 8, &api) == 0);
        int expected_error = mode == 0 ? -ENOTSUP : mode == 1 ? -ENOSPC : -EUCLEAN;
        assert(
            dppd_control_transfer(&control, &spec, true, &graph, results, message, sizeof(message)) == expected_error);
        assert(dppd_rule_repository_count(&control.rules) == 0 && control.rules.generation == 0);
        assert(fake.validations == 2 && fake.creates == (mode == 0 ? 0U : 2U));
        assert(fake.removes == (mode == 0 ? 0U : 1U));
        if (mode != 0)
            assert(fake.removed[0] == spec.first_rule_id);
        dppd_control_recovery_status(&control, &recovery);
        if (mode == 2) {
            assert(recovery.state == DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED);
            assert(recovery.residual_objects == 1 && recovery.last_error == -EBUSY);
            assert(dppd_rte_flow_backend_find_version(&control.rte_flow, spec.first_rule_id, 1) != NULL);
            unsigned int creates = fake.creates, validates = fake.validations;
            spec.first_rule_id = 200;
            assert(dppd_control_transfer(&control, &spec, true, &graph, results, message, sizeof(message)) == -EUCLEAN);
            assert(fake.creates == creates && fake.validations == validates);
            assert(dppd_control_reconciliation_retry(&control) == 0);
            assert(fake.removes == 2 && fake.removed[1] == 100);
            dppd_control_recovery_status(&control, &recovery);
            assert(recovery.state == DPPD_CONTROL_RECOVERY_RESTART_REQUIRED && recovery.residual_objects == 0);
            assert(dppd_control_transfer(&control, &spec, true, &graph, results, message, sizeof(message)) == -EUCLEAN);
        } else
            assert(recovery.state == DPPD_CONTROL_RECOVERY_READY && recovery.residual_objects == 0);
        assert(dppd_control_fini(&control) == 0);
    }
}

/** 同优先级重叠必须在驱动前拒绝，不同优先级和没有交集的源端口可以明确共存 */
static void overlap(void)
{
    struct dppd_topology topology = make_topology();
    struct dppd_control_service control;
    struct dppd_transfer_spec spec = make_spec();
    struct dppd_transfer_graph graph;
    struct dppd_control_apply_result results[2];
    char message[160];
    reset_fake();
    assert(dppd_control_init(&control, &topology, 12, &api) == 0);
    spec.bidirectional = 0;
    spec.expected_repository_generation = 0;
    assert(dppd_control_transfer(&control, &spec, true, &graph, results, message, sizeof(message)) == 0);
    spec.first_rule_id = 200;
    spec.expected_repository_generation = graph.repository_generation;
    assert(dppd_control_transfer(&control, &spec, false, &graph, results, message, sizeof(message)) == -EEXIST);
    assert(dppd_control_transfer(&control, &spec, true, &graph, results, message, sizeof(message)) == -EEXIST);
    assert(fake.creates == 1 && fake.validations == 1);
    spec.priority++;
    assert(dppd_control_transfer(&control, &spec, true, &graph, results, message, sizeof(message)) == 0);
    spec.first_rule_id = 300;
    spec.priority--;
    spec.source_port_id = 13;
    spec.destination_port_id = 14;
    spec.expected_repository_generation = graph.repository_generation;
    assert(dppd_control_transfer(&control, &spec, true, &graph, results, message, sizeof(message)) == 0);
    spec.first_rule_id = 400;
    spec.bidirectional = 1;
    spec.source_port_id = 12;
    spec.destination_port_id = 10;
    spec.expected_repository_generation = graph.repository_generation;
    assert(dppd_control_transfer(&control, &spec, true, &graph, results, message, sizeof(message)) == -EEXIST);
    assert(fake.creates == 3 && fake.validations == 3 && control.rules.generation == 3);
    spec.first_rule_id = 100;
    spec.priority = 99;
    assert(dppd_control_transfer(&control, &spec, true, &graph, results, message, sizeof(message)) == -EEXIST);
    assert(dppd_control_fini(&control) == 0);
}

/** 没有源端口条件的已有规则会覆盖整域，其他组或交换域则没有这份优先级歧义 */
static void wildcard_overlap(void)
{
    struct dppd_topology topology = make_topology();
    for (unsigned int variant = 0; variant < 3; ++variant) {
        struct dppd_control_service control;
        struct dppd_transfer_spec spec = make_spec();
        struct dppd_transfer_graph graph;
        struct dppd_control_apply_result result, results[2];
        struct dppd_rule existing = {.id = 50,
            .domain = DPPD_RULE_DOMAIN_TRANSFER,
            .fallback = DPPD_FALLBACK_REQUIRE_HARDWARE,
            .group = variant == 1 ? 1 : 0,
            .priority = spec.priority,
            .nb_matches = 1,
            .nb_actions = 1,
            .matches = {{.type = DPPD_MATCH_ETH}},
            .actions = {{.type = DPPD_ACTION_DROP}}};
        char message[160];
        reset_fake();
        assert(dppd_control_init(&control, &topology, 8, &api) == 0);
        assert(dppd_control_apply(&control, variant == 2 ? 17 : 10, &existing, 0, &result) == 0);
        spec.expected_repository_generation = 1;
        int rc = dppd_control_transfer(&control, &spec, true, &graph, results, message, sizeof(message));
        if (variant == 0) {
            assert(rc == -EEXIST && fake.creates == 1 && fake.validations == 1);
            assert(control.rules.generation == 1);
        } else {
            assert(rc == 0 && fake.creates == 3 && fake.validations == 3);
            assert(control.rules.generation == 3);
        }
        assert(dppd_control_fini(&control) == 0);
    }
}

/** PF 号属于局部命名域，双方控制器都未知时还必须证明来自同一个已发现 PCI 设备 */
static void parent_evidence(void)
{
    for (unsigned int sf = 0; sf < 2; ++sf) {
        struct dppd_topology topology = make_topology();
        struct dppd_planner_context context = {.topology = &topology, .hardware_available = true};
        struct dppd_transfer_spec spec = make_spec();
        struct dppd_transfer_graph graph;
        struct dppd_endpoint *parent = &topology.endpoints[1];
        struct dppd_endpoint *child = &topology.endpoints[sf ? 4 : 2];
        char message[160];
        spec.scenario = sf ? DPPD_TRANSFER_PF_SF : DPPD_TRANSFER_PF_VF;
        spec.source_port_id = parent->ethdev_port_id;
        spec.destination_port_id = child->ethdev_port_id;
        assert(dppd_topology_resolve_role(parent, "net_mlx5", true, "pf0") == 0);
        assert(dppd_topology_resolve_role(child, "net_mlx5", true, sf ? "pf0sf1" : "pf0vf1") == 0);
        assert(!parent->has_controller && !child->has_controller);
        expect_invalid_graph(&context, &spec, -EXDEV);
        memcpy(parent->device_name, "0000:03:00.0", sizeof("0000:03:00.0"));
        expect_invalid_graph(&context, &spec, -EXDEV);
        memcpy(child->device_name, parent->device_name, sizeof(child->device_name));
        assert(dppd_transfer_graph_build(&context, &spec, &graph, message, sizeof(message)) == 0);
        check_graph(&graph, &spec);
        memcpy(child->device_name, "0000:04:00.0", sizeof("0000:04:00.0"));
        expect_invalid_graph(&context, &spec, -EXDEV);
        memset(child->device_name, 'x', sizeof(child->device_name));
        expect_invalid_graph(&context, &spec, -EXDEV);
        memcpy(child->device_name, parent->device_name, sizeof(child->device_name));
        parent->has_controller = true;
        parent->controller_id = 0;
        expect_invalid_graph(&context, &spec, -EXDEV);
        child->has_controller = true;
        child->controller_id = 1;
        expect_invalid_graph(&context, &spec, -EXDEV);
        child->controller_id = 0;
        child->has_pf = false;
        expect_invalid_graph(&context, &spec, -EXDEV);
        child->has_pf = true;
        memcpy(child->device_name, "0000:04:00.0", sizeof("0000:04:00.0"));
        assert(dppd_transfer_graph_build(&context, &spec, &graph, message, sizeof(message)) == 0);
    }
    assert(fake.validations == 0 && fake.creates == 0 && fake.removes == 0);
}

/** 同域源端口别名必须在任何驱动调用前拒绝，反向边和已有别名入口也遵守同一规则 */
static void alias_overlap(void)
{
    for (unsigned int variant = 0; variant < 3; ++variant) {
        struct dppd_topology topology = make_topology();
        struct dppd_control_service control;
        struct dppd_transfer_spec spec = make_spec();
        struct dppd_transfer_graph graph;
        struct dppd_control_apply_result results[2];
        char message[160];
        topology.endpoints[6].switch_port_id = topology.endpoints[0].switch_port_id;
        reset_fake();
        assert(dppd_control_init(&control, &topology, 8, &api) == 0);
        spec.bidirectional = 0;
        spec.expected_repository_generation = 0;
        if (variant == 2)
            spec.source_port_id = 16;
        assert(dppd_control_transfer(&control, &spec, true, &graph, results, message, sizeof(message)) == 0);
        spec.first_rule_id = 200;
        spec.expected_repository_generation = graph.repository_generation;
        spec.source_port_id = variant == 2 ? 10 : variant == 1 ? 14 : 16;
        spec.destination_port_id = variant == 1 ? 16 : 14;
        spec.bidirectional = variant == 1;
        assert(dppd_control_transfer(&control, &spec, false, &graph, results, message, sizeof(message)) == -EEXIST);
        assert(dppd_control_transfer(&control, &spec, true, &graph, results, message, sizeof(message)) == -EEXIST);
        assert(fake.creates == 1 && fake.validations == 1 && control.rules.generation == 1);
        spec.priority++;
        assert(dppd_control_transfer(&control, &spec, true, &graph, results, message, sizeof(message)) == 0);
        assert(fake.creates == (variant == 1 ? 3U : 2U));
        assert(dppd_control_fini(&control) == 0);
    }
    /** 交换端口编号只在各自交换域内有意义，其他域碰巧同号仍能独立使用 */
    struct dppd_topology topology = make_topology();
    topology.endpoints[7].switch_port_id = topology.endpoints[0].switch_port_id;
    topology.endpoints[8].switch_domain_id = topology.endpoints[7].switch_domain_id;
    struct dppd_control_service control;
    struct dppd_transfer_spec spec = make_spec();
    struct dppd_transfer_graph graph;
    struct dppd_control_apply_result results[2];
    char message[160];
    reset_fake();
    assert(dppd_control_init(&control, &topology, 8, &api) == 0);
    spec.bidirectional = 0;
    spec.expected_repository_generation = 0;
    assert(dppd_control_transfer(&control, &spec, true, &graph, results, message, sizeof(message)) == 0);
    spec.first_rule_id = 200;
    spec.expected_repository_generation = graph.repository_generation;
    spec.install_port_id = spec.source_port_id = 17;
    spec.destination_port_id = 18;
    assert(dppd_control_transfer(&control, &spec, true, &graph, results, message, sizeof(message)) == 0);
    assert(fake.creates == 2 && fake.validations == 2 && control.rules.generation == 2);
    assert(dppd_control_fini(&control) == 0);
}

int main(void)
{
    templates();
    reset_fake();
    invalid_graphs();
    parent_evidence();
    versions();
    failures();
    overlap();
    wildcard_overlap();
    alias_overlap();
    puts("PASS transfer graph and control transactions with simulated driver; hardware acceptance remains pending");
    return 0;
}
