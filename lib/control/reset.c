#include "dppd/control.h"
#include "reset_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/** 期望配置在复位前一次保存，规则和对象的版本不变，重试不从可能变化的磁盘重新取数 */
struct dppd_reset_plan {
    struct dppd_transaction_item *items;
    struct dppd_shared_count_spec shared[DPPD_SHARED_COUNT_MAX];
    struct dppd_topology topology;
    uint64_t repository_generation, shared_revision;
    uint32_t count, shared_count;
};

static void increment(uint64_t *value)
{
    if (*value != UINT64_MAX)
        ++*value;
}

void dppd_control_reset_plan_destroy(struct dppd_control_service *service)
{
    if (service == NULL || service->reset_plan == NULL)
        return;
    free(service->reset_plan->items);
    free(service->reset_plan);
    service->reset_plan = NULL;
}

static int failed(struct dppd_control_service *service, int error, int cleanup)
{
    service->reset.last_error = error;
    service->reset.cleanup_error = cleanup;
    if (service->reset_plan != NULL) {
        service->recovery_state = DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED;
        service->recovery_last_error = cleanup != 0 ? cleanup : error;
    }
    return cleanup != 0 ? -EUCLEAN : error;
}

/** 字符串按有效内容比较，结构体填充和发现顺序不属于端点身份 */
static bool same_text(const char *left, const char *right, size_t size)
{
    return memchr(left, '\0', size) != NULL && memchr(right, '\0', size) != NULL && strcmp(left, right) == 0;
}

static bool same_endpoint(const struct dppd_endpoint *left, const struct dppd_endpoint *right)
{
    return left->kind == right->kind && left->has_switch_domain == right->has_switch_domain &&
        (!left->has_switch_domain ||
            (left->switch_domain_id == right->switch_domain_id && left->switch_port_id == right->switch_port_id)) &&
        left->role == right->role && left->role_source == right->role_source && left->role_error == right->role_error &&
        left->if_index == right->if_index && left->has_controller == right->has_controller &&
        (!left->has_controller || left->controller_id == right->controller_id) && left->has_pf == right->has_pf &&
        (!left->has_pf || left->pf_id == right->pf_id) && left->has_function == right->has_function &&
        (!left->has_function || left->function_id == right->function_id) &&
        left->has_physical_port == right->has_physical_port &&
        (!left->has_physical_port || left->physical_port_id == right->physical_port_id) &&
        same_text(left->switch_name, right->switch_name, sizeof(left->switch_name)) &&
        same_text(left->driver_name, right->driver_name, sizeof(left->driver_name)) &&
        same_text(left->interface_name, right->interface_name, sizeof(left->interface_name)) &&
        same_text(left->device_name, right->device_name, sizeof(left->device_name)) &&
        same_text(left->phys_port_name, right->phys_port_name, sizeof(left->phys_port_name));
}

/** 同一个 ethdev 编号若已对应另一端点也要拒绝，不能把新设备当作旧规则的安装目标 */
static int check_endpoint(const struct dppd_reset_plan *plan, const struct dppd_topology *current, uint16_t port)
{
    const struct dppd_endpoint *before = dppd_topology_find(&plan->topology, port);
    const struct dppd_endpoint *after = dppd_topology_find(current, port);
    if (before == NULL || after == NULL)
        return -ENOENT;
    return same_endpoint(before, after) ? 0 : -EXDEV;
}

static int check_topology(const struct dppd_reset_plan *plan, const struct dppd_topology *current)
{
    if (current == NULL || current->nb_endpoints > DPPD_MAX_PORTS)
        return -EINVAL;
    for (uint16_t i = 0; i < current->nb_endpoints; ++i)
        for (uint16_t j = 0; j < i; ++j)
            if (current->endpoints[i].ethdev_port_id == current->endpoints[j].ethdev_port_id)
                return -EUCLEAN;
    for (uint32_t i = 0; i < plan->shared_count; ++i) {
        int rc = check_endpoint(plan, current, plan->shared[i].install_port_id);
        if (rc != 0)
            return rc;
    }
    for (uint32_t i = 0; i < plan->count; ++i) {
        const struct dppd_rule *rule = &plan->items[i].rule;
        int rc = check_endpoint(plan, current, rule->install_port_id);
        if (rc != 0)
            return rc;
        for (uint16_t j = 0; j < rule->nb_matches; ++j)
            if (rule->matches[j].type == DPPD_MATCH_REPRESENTED_PORT &&
                (rc = check_endpoint(plan, current, rule->matches[j].spec.ethdev_port_id)) != 0)
                return rc;
        for (uint16_t j = 0; j < rule->nb_actions; ++j)
            if (rule->actions[j].type == DPPD_ACTION_REPRESENTED_PORT &&
                (rc = check_endpoint(plan, current, rule->actions[j].conf.ethdev_port_id)) != 0)
                return rc;
    }
    return 0;
}

static int capture_rules(struct dppd_control_service *service, struct dppd_reset_plan *plan)
{
    struct dppd_rule page[DPPD_AGE_POLL_BUDGET];
    uint64_t after = 0, observed;
    uint32_t copied = 0, count;
    bool more;
    do {
        int rc = dppd_rule_repository_list(&service->rules, after, plan->repository_generation, page,
            DPPD_AGE_POLL_BUDGET, &count, &more, &observed);
        if (rc != 0)
            return rc;
        for (uint32_t i = 0; i < count; ++i) {
            struct dppd_rule_install_info info;
            if (copied == plan->count ||
                dppd_rte_flow_backend_install_info(&service->rte_flow, page[i].id, page[i].generation, &info) != 0)
                return -EUCLEAN;
            plan->items[copied++].rule = page[i];
            after = page[i].id;
        }
    } while (more);
    return copied == plan->count ? 0 : -EUCLEAN;
}

/** 已知没有重建能力或事务编号时，先拒绝复位，不能先丢掉仍正常工作的实际对象 */
static int rebuild_preflight(const struct dppd_control_service *service, const struct dppd_reset_plan *plan)
{
    if (plan->count != 0 && (service->next_transaction_id == 0 || service->next_transaction_id == UINT64_MAX))
        return -EOVERFLOW;
    if (plan->count != 0 &&
        (service->rte_flow.api.validate == NULL || service->rte_flow.api.create == NULL ||
            service->rte_flow.api.remove == NULL))
        return -ENOTSUP;
    if (plan->shared_count != 0 &&
        (service->rte_flow.api.shared_count_create == NULL || service->rte_flow.api.shared_count_destroy == NULL))
        return -ENOTSUP;
    for (uint32_t i = 0; i < plan->count; ++i)
        for (uint16_t j = 0; j < plan->items[i].rule.nb_actions; ++j)
            if (plan->items[i].rule.actions[j].type == DPPD_ACTION_SHARED_COUNT &&
                (service->rte_flow.api.validate_bound == NULL || service->rte_flow.api.create_bound == NULL))
                return -ENOTSUP;
    return 0;
}

int dppd_control_simulate_reset(struct dppd_control_service *service)
{
    if (service == NULL || service->topology == NULL || service->rules.records == NULL)
        return -EINVAL;
    if (service->reset_plan != NULL)
        return -EALREADY;
    if (service->recovery_state != DPPD_CONTROL_RECOVERY_READY || service->persistence_dirty)
        return failed(service, -EUCLEAN, 0);
    /** 模拟不会停止真实 worker 或协调三层阶段，这些组合必须在失效任何对象前拒绝 */
    if (service->rte_flow.api.simulate_reset == NULL || service->rte_flow.before_create != NULL ||
        service->routing != NULL || service->conntrack != NULL || service->nd6 != NULL ||
        dppd_software_backend_count(&service->software) != 0)
        return failed(service, -ENOTSUP, 0);
    if (service->topology->nb_endpoints > DPPD_MAX_PORTS)
        return failed(service, -EINVAL, 0);
    if (dppd_rte_flow_backend_count(&service->rte_flow) != dppd_rule_repository_count(&service->rules))
        return failed(service, -EUCLEAN, 0);
    struct dppd_reset_plan *plan = calloc(1, sizeof(*plan));
    if (plan == NULL)
        return failed(service, -ENOMEM, 0);
    plan->count = dppd_rule_repository_count(&service->rules);
    plan->repository_generation = dppd_rule_repository_generation(&service->rules);
    plan->shared_revision = service->shared_count_revision;
    plan->topology = *service->topology;
    if (plan->count != 0) {
        plan->items = calloc(plan->count, sizeof(*plan->items));
        if (plan->items == NULL) {
            free(plan);
            return failed(service, -ENOMEM, 0);
        }
    }
    int rc = capture_rules(service, plan);
    if (rc == 0)
        rc = dppd_rte_flow_backend_shared_count_list(&service->rte_flow, plan->shared, DPPD_SHARED_COUNT_MAX,
            &plan->shared_count);
    if (rc == 0)
        rc = check_topology(plan, service->topology);
    if (rc == 0)
        rc = rebuild_preflight(service, plan);
    struct dppd_flow_error error;
    if (rc == 0)
        rc = dppd_rte_flow_backend_simulate_reset(&service->rte_flow, &error);
    if (rc != 0) {
        free(plan->items);
        free(plan);
        return failed(service, rc, 0);
    }
    /** 模拟回调成功代表旧句柄全部失效，随后普通写入和注入必须一直保持隔离 */
    service->reset_plan = plan;
    service->recovery_state = DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED;
    service->recovery_last_error = 0;
    service->reset.last_error = service->reset.cleanup_error = 0;
    increment(&service->reset.resets);
    return 0;
}

/** 只清理本次已重建的有效句柄，失效旧句柄已由模拟复位回调从仓库移除 */
static int cleanup_failed_rebuild(struct dppd_control_service *service, int original)
{
    uint32_t residual;
    int rc = dppd_rte_flow_backend_reconcile(&service->rte_flow, &residual);
    return failed(service, original, rc != 0 ? rc : residual != 0 ? -EUCLEAN : 0);
}

int dppd_control_reset_recover(struct dppd_control_service *service)
{
    if (service == NULL)
        return -EINVAL;
    struct dppd_reset_plan *plan = service->reset_plan;
    if (plan == NULL)
        return -EALREADY;
    increment(&service->reset.attempts);
    if (service->persistence_dirty || service->shared_count_revision != plan->shared_revision ||
        dppd_rule_repository_generation(&service->rules) != plan->repository_generation ||
        dppd_rule_repository_count(&service->rules) != plan->count)
        return failed(service, -EUCLEAN, 0);
    if (service->routing != NULL || service->conntrack != NULL || service->nd6 != NULL ||
        dppd_software_backend_count(&service->software) != 0 || service->rte_flow.before_create != NULL)
        return failed(service, -ENOTSUP, 0);
    int rc = check_topology(plan, service->topology);
    if (rc != 0)
        return failed(service, rc, 0);
    rc = rebuild_preflight(service, plan);
    if (rc != 0)
        return failed(service, rc, 0);
    /** 先把全部规则的本地规划做完，不让后面的拓扑或能力错误留下前面新建的共享对象 */
    for (uint32_t i = 0; i < plan->count; ++i) {
        struct dppd_transaction_item *item = &plan->items[i];
        struct dppd_planner_context context = {.topology = service->topology,
            .install_port_id = item->rule.install_port_id,
            .hardware_available = true};
        rc = dppd_plan_rule(&context, &item->rule, &item->plan);
        if (rc != 0 || item->plan.backend != DPPD_PLAN_BACKEND_RTE_FLOW)
            return failed(service, rc != 0 ? rc : -ENOTSUP, 0);
    }
    /** 上次补偿失败留下的有效对象先清理，清理不成功就不追加另一批新对象 */
    if (dppd_rte_flow_backend_count(&service->rte_flow) != 0 ||
        dppd_rte_flow_backend_shared_count_count(&service->rte_flow) != 0) {
        uint32_t residual;
        rc = dppd_rte_flow_backend_reconcile(&service->rte_flow, &residual);
        if (rc != 0 || residual != 0)
            return failed(service, rc != 0 ? rc : -EUCLEAN, rc != 0 ? rc : -EUCLEAN);
    }
    for (uint32_t i = 0; i < plan->shared_count; ++i) {
        struct dppd_flow_error error;
        rc = dppd_rte_flow_backend_shared_count_create(&service->rte_flow, &plan->shared[i], &error);
        if (rc != 0)
            return cleanup_failed_rebuild(service, rc);
    }
    if (plan->count != 0) {
        struct dppd_transaction transaction = {0};
        const struct dppd_transaction_backends backends = {
            .rte_flow = dppd_rte_flow_transaction_backend(&service->rte_flow)};
        rc = dppd_transaction_init(&transaction, service->next_transaction_id++, plan->items, plan->count);
        if (rc == 0)
            rc = dppd_transaction_run(&transaction, &backends);
        if (rc != 0) {
            if (transaction.rollback_code != 0)
                return failed(service, rc, transaction.rollback_code);
            return cleanup_failed_rebuild(service, rc);
        }
        rc = dppd_transaction_finalize(&transaction, &backends);
        if (rc != 0)
            return cleanup_failed_rebuild(service, rc);
    }
    /** 账本与磁盘没有改写，只有整套实际对象重建成功才恢复 READY */
    dppd_control_reset_plan_destroy(service);
    service->recovery_state = DPPD_CONTROL_RECOVERY_READY;
    service->recovery_last_error = 0;
    service->reset.last_error = service->reset.cleanup_error = 0;
    increment(&service->reset.recoveries);
    service->age.cursor = service->age.next_poll_ns = 0;
    service->age.last_error = service->age_cycle_error = 0;
    return 0;
}

void dppd_control_reset_status(const struct dppd_control_service *service, struct dppd_reset_status *status)
{
    if (status == NULL)
        return;
    memset(status, 0, sizeof(*status));
    if (service == NULL)
        return;
    *status = service->reset;
    status->pending = service->reset_plan != NULL;
    status->desired_rules = dppd_rule_repository_count(&service->rules);
    status->desired_shared = status->pending ? service->reset_plan->shared_count
                                             : dppd_rte_flow_backend_shared_count_count(&service->rte_flow);
    status->actual_rules = dppd_rte_flow_backend_count(&service->rte_flow);
    status->actual_shared = dppd_rte_flow_backend_shared_count_count(&service->rte_flow);
}
