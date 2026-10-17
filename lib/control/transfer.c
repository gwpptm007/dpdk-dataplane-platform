#include "dppd/transfer.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int fail(int rc, const char *text, char *message, size_t size)
{
    if (message != NULL && size != 0)
        snprintf(message, size, "%s", text);
    return rc;
}

/** pair 只依赖已发现的交换域，命名场景还必须有可信角色，不能按端口顺序补猜 */
static bool roles_match(uint8_t scenario, enum dppd_endpoint_role source, enum dppd_endpoint_role destination)
{
    switch (scenario) {
    case DPPD_TRANSFER_PAIR:
        return true;
    case DPPD_TRANSFER_UPLINK_VF:
        return source == DPPD_ENDPOINT_ROLE_UPLINK && destination == DPPD_ENDPOINT_ROLE_VF;
    case DPPD_TRANSFER_UPLINK_SF:
        return source == DPPD_ENDPOINT_ROLE_UPLINK && destination == DPPD_ENDPOINT_ROLE_SF;
    case DPPD_TRANSFER_PF_VF:
        return source == DPPD_ENDPOINT_ROLE_PF && destination == DPPD_ENDPOINT_ROLE_VF;
    case DPPD_TRANSFER_PF_SF:
        return source == DPPD_ENDPOINT_ROLE_PF && destination == DPPD_ENDPOINT_ROLE_SF;
    case DPPD_TRANSFER_VF_VF:
        return source == DPPD_ENDPOINT_ROLE_VF && destination == DPPD_ENDPOINT_ROLE_VF;
    case DPPD_TRANSFER_SF_SF:
        return source == DPPD_ENDPOINT_ROLE_SF && destination == DPPD_ENDPOINT_ROLE_SF;
    default:
        return false;
    }
}

static bool verified_role(const struct dppd_endpoint *endpoint)
{
    if (endpoint->role_source != DPPD_ENDPOINT_ROLE_SOURCE_MLX5_PHYS_PORT_NAME || endpoint->role_error != 0 ||
        endpoint->role == DPPD_ENDPOINT_ROLE_UNKNOWN)
        return false;
    return endpoint->role == DPPD_ENDPOINT_ROLE_UPLINK ? endpoint->kind == DPPD_ENDPOINT_ETHDEV
                                                       : endpoint->kind == DPPD_ENDPOINT_REPRESENTOR;
}

/** 未知控制器不能直接判为相同，只有同一已核对 PCI 设备的局部编号才能继续比较 */
static bool matching_parent(const struct dppd_endpoint *source, const struct dppd_endpoint *destination)
{
    if (!source->has_pf || !destination->has_pf || source->pf_id != destination->pf_id ||
        source->has_controller != destination->has_controller)
        return false;
    if (source->has_controller)
        return source->controller_id == destination->controller_id;
    return source->device_name[0] != '\0' && destination->device_name[0] != '\0' &&
        memchr(source->device_name, '\0', sizeof(source->device_name)) != NULL &&
        memchr(destination->device_name, '\0', sizeof(destination->device_name)) != NULL &&
        strcmp(source->device_name, destination->device_name) == 0;
}

int dppd_transfer_graph_build(const struct dppd_planner_context *context, const struct dppd_transfer_spec *spec,
    struct dppd_transfer_graph *graph, char *message, size_t message_size)
{
    static const uint8_t zero[7] = {0};
    if (context == NULL || context->topology == NULL || spec == NULL || graph == NULL ||
        context->topology->nb_endpoints > DPPD_MAX_PORTS || spec->first_rule_id == 0 ||
        spec->first_rule_id == UINT64_MAX || spec->scenario >= DPPD_TRANSFER_TEMPLATE_COUNT ||
        spec->bidirectional > 1 || spec->with_count > 1 || spec->age_timeout_seconds > 0xffffffU ||
        memcmp(spec->reserved, zero, sizeof(zero)) != 0)
        return fail(-EINVAL, "invalid transfer specification", message, message_size);
    if ((spec->shared_count_id == 0) != (spec->shared_count_generation == 0) || spec->shared_count_id == UINT64_MAX ||
        spec->shared_count_generation == UINT64_MAX || (spec->with_count && spec->shared_count_id != 0))
        return fail(-EINVAL, "shared count requires an exact object and cannot be combined with count", message,
            message_size);
    if (spec->source_port_id == spec->destination_port_id)
        return fail(-EINVAL, "source and destination must differ", message, message_size);
    const struct dppd_endpoint *source = dppd_topology_find(context->topology, spec->source_port_id);
    const struct dppd_endpoint *destination = dppd_topology_find(context->topology, spec->destination_port_id);
    if (source == NULL || destination == NULL)
        return fail(-ENOENT, "source or destination is absent from topology", message, message_size);
    if (!roles_match(spec->scenario, source->role, destination->role))
        return fail(-ENOTSUP, "template requires verified endpoint roles", message, message_size);
    if (spec->scenario != DPPD_TRANSFER_PAIR && (!verified_role(source) || !verified_role(destination)))
        return fail(-ENOTSUP, "endpoint role evidence is missing or contradictory", message, message_size);
    if (source->has_switch_domain && destination->has_switch_domain &&
        source->switch_domain_id == destination->switch_domain_id &&
        source->switch_port_id == destination->switch_port_id)
        return fail(-EINVAL, "source and destination alias the same switch port", message, message_size);
    if (spec->scenario == DPPD_TRANSFER_PF_VF || spec->scenario == DPPD_TRANSFER_PF_SF) {
        if (!matching_parent(source, destination))
            return fail(-EXDEV, "PF and function do not have matching controller/PF evidence", message, message_size);
    }
    struct dppd_transfer_graph next = {.count = spec->bidirectional ? 2 : 1};
    struct dppd_planner_context planner = *context;
    planner.install_port_id = spec->install_port_id;
    planner.software_equivalent = false;
    for (uint16_t i = 0; i < next.count; ++i) {
        struct dppd_rule *rule = &next.rules[i];
        rule->id = spec->first_rule_id + i;
        if (rule->id == UINT64_MAX)
            return fail(-EOVERFLOW, "rule ID range exhausted", message, message_size);
        rule->install_port_id = spec->install_port_id;
        rule->domain = DPPD_RULE_DOMAIN_TRANSFER;
        rule->fallback = DPPD_FALLBACK_REQUIRE_HARDWARE;
        rule->priority = spec->priority;
        rule->nb_matches = 2;
        rule->matches[0].type = DPPD_MATCH_REPRESENTED_PORT;
        rule->matches[0].spec.ethdev_port_id = i == 0 ? spec->source_port_id : spec->destination_port_id;
        rule->matches[1].type = DPPD_MATCH_ETH;
        rule->nb_actions = 1;
        uint16_t action = 0;
        if (spec->with_count) {
            rule->actions[action++].type = DPPD_ACTION_COUNT;
            ++rule->nb_actions;
        }
        if (spec->shared_count_id != 0) {
            rule->actions[action].type = DPPD_ACTION_SHARED_COUNT;
            rule->actions[action].conf.shared_count.id = spec->shared_count_id;
            rule->actions[action++].conf.shared_count.generation = spec->shared_count_generation;
            ++rule->nb_actions;
        }
        if (spec->age_timeout_seconds != 0) {
            rule->actions[action].type = DPPD_ACTION_AGE;
            rule->actions[action++].conf.age_timeout_seconds = spec->age_timeout_seconds;
            ++rule->nb_actions;
        }
        rule->actions[rule->nb_actions - 1].type = DPPD_ACTION_REPRESENTED_PORT;
        rule->actions[rule->nb_actions - 1].conf.ethdev_port_id =
            i == 0 ? spec->destination_port_id : spec->source_port_id;
        int rc = dppd_plan_rule(&planner, rule, &next.plans[i]);
        if (rc != 0)
            return fail(rc, dppd_plan_reason_name(next.plans[i].reason), message, message_size);
        if (next.plans[i].backend != DPPD_PLAN_BACKEND_RTE_FLOW || !next.plans[i].requires_validation)
            return fail(-ENOTSUP, "transfer cannot use software execution", message, message_size);
    }
    next.switch_domain_id = source->switch_domain_id;
    *graph = next;
    return fail(0, "topology plan only; PMD validation and hardware acceptance remain required", message, message_size);
}

/** 不同 ethdev 编号可能指向同一个交换端口，冲突检测必须核对实际端点而不是只比较编号 */
static bool same_switch_endpoint(const struct dppd_topology *topology, uint16_t first, uint16_t second)
{
    if (first == second)
        return true;
    const struct dppd_endpoint *left = dppd_topology_find(topology, first);
    const struct dppd_endpoint *right = dppd_topology_find(topology, second);
    return left != NULL && right != NULL && left->has_switch_domain && right->has_switch_domain &&
        left->switch_domain_id != UINT16_MAX && left->switch_domain_id == right->switch_domain_id &&
        left->switch_port_id == right->switch_port_id;
}

/** 新图的入口是通配以太网，同组同优先级的已有源端口条件只要相交就存在歧义 */
static bool overlaps(const struct dppd_topology *topology, const struct dppd_transfer_graph *graph,
    const struct dppd_rule *existing)
{
    if (existing->domain != DPPD_RULE_DOMAIN_TRANSFER || existing->group != 0 ||
        existing->priority != graph->rules[0].priority)
        return false;
    const struct dppd_endpoint *owner = dppd_topology_find(topology, existing->install_port_id);
    if (owner == NULL || !owner->has_switch_domain || owner->switch_domain_id != graph->switch_domain_id)
        return false;
    for (uint16_t item = 0; item < existing->nb_matches; ++item)
        if (existing->matches[item].type == DPPD_MATCH_REPRESENTED_PORT) {
            for (uint16_t edge = 0; edge < graph->count; ++edge)
                if (same_switch_endpoint(topology, existing->matches[item].spec.ethdev_port_id,
                        graph->rules[edge].matches[0].spec.ethdev_port_id))
                    return true;
            return false;
        }
    return true;
}

int dppd_control_transfer(struct dppd_control_service *control, const struct dppd_transfer_spec *spec, bool apply,
    struct dppd_transfer_graph *graph, struct dppd_control_apply_result results[DPPD_TRANSFER_MAX_EDGES], char *message,
    size_t message_size)
{
    if (control == NULL || spec == NULL || graph == NULL || results == NULL)
        return fail(-EINVAL, "invalid transfer request", message, message_size);
    if (apply && control->recovery_state != DPPD_CONTROL_RECOVERY_READY)
        return fail(-EUCLEAN, "recovery isolation blocks transfer installation", message, message_size);
    uint64_t generation = dppd_rule_repository_generation(&control->rules);
    if ((apply && spec->expected_repository_generation == DPPD_RULE_GENERATION_ANY) ||
        (spec->expected_repository_generation != DPPD_RULE_GENERATION_ANY &&
            spec->expected_repository_generation != generation))
        return fail(-ESTALE, "repository changed; build a fresh transfer plan", message, message_size);
    struct dppd_planner_context context = {.topology = control->topology,
        .hardware_available = true,
        .ipv4_routing = control->routing != NULL,
        .connection_tracking = control->conntrack != NULL,
        .neighbor_discovery = control->nd6 != NULL};
    struct dppd_transfer_graph next;
    int rc = dppd_transfer_graph_build(&context, spec, &next, message, message_size);
    if (rc != 0)
        return rc;
    /** 规划只核对内存中的对象身份，不向设备创建、查询或增加引用 */
    if (spec->shared_count_id != 0) {
        struct dppd_shared_count_info shared;
        rc = dppd_control_shared_count_get(control, spec->shared_count_id, spec->shared_count_generation, &shared);
        if (rc != 0)
            return fail(rc, "shared count is missing or has a different generation", message, message_size);
        if (shared.spec.install_port_id != spec->install_port_id || shared.spec.domain != DPPD_RULE_DOMAIN_TRANSFER)
            return fail(-EXDEV, "shared count belongs to a different install port or domain", message, message_size);
    }
    if (generation >= UINT64_MAX - next.count)
        return fail(-EOVERFLOW, "repository generation space exhausted", message, message_size);
    next.repository_generation = generation;
    for (uint16_t i = 0; i < next.count; ++i) {
        struct dppd_rule existing;
        rc = dppd_rule_repository_get(&control->rules, next.rules[i].id, &existing);
        if (rc != -ENOENT)
            return fail(rc == 0 ? -EEXIST : rc, "rule ID is already present or unavailable", message, message_size);
    }
    struct dppd_rule page[4];
    uint64_t after = 0, observed;
    uint32_t count;
    bool more;
    do {
        rc = dppd_rule_repository_list(&control->rules, after, generation, page, 4, &count, &more, &observed);
        if (rc != 0)
            return fail(rc, "repository changed during transfer planning", message, message_size);
        for (uint32_t i = 0; i < count; ++i) {
            if (overlaps(control->topology, &next, &page[i]))
                return fail(-EEXIST, "same-priority transfer ingress overlaps an existing rule", message, message_size);
            after = page[i].id;
        }
    } while (more);
    if (!apply) {
        *graph = next;
        return 0;
    }
    struct dppd_control_batch_create_request requests[DPPD_TRANSFER_MAX_EDGES] = {0};
    struct dppd_control_apply_result created[DPPD_TRANSFER_MAX_EDGES];
    for (uint16_t i = 0; i < next.count; ++i) {
        requests[i].install_port_id = spec->install_port_id;
        requests[i].rule = next.rules[i];
    }
    rc = dppd_control_create_batch(control, requests, next.count, created);
    if (rc != 0)
        return fail(rc,
            rc == -EUCLEAN ? "state uncertain; inspect recovery and persistence before retry"
                           : "transfer batch failed; inspect rule history and PMD diagnostics",
            message, message_size);
    for (uint16_t i = 0; i < next.count; ++i) {
        next.rules[i].generation = created[i].generation;
        next.plans[i] = created[i].plan;
    }
    next.repository_generation = dppd_rule_repository_generation(&control->rules);
    *graph = next;
    memcpy(results, created, next.count * sizeof(*created));
    return fail(0, "hardware handles created; traffic and CPU bypass have not been verified", message, message_size);
}
