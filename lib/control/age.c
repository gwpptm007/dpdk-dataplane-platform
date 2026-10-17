#include "dppd/age.h"
#include "dppd/control.h"

#include <errno.h>
#include <string.h>

static bool has_age(const struct dppd_rule *rule)
{
    for (uint16_t i = 0; i < rule->nb_actions; ++i)
        if (rule->actions[i].type == DPPD_ACTION_AGE)
            return true;
    return false;
}

static void increment(uint64_t *value)
{
    if (*value != UINT64_MAX)
        ++*value;
}

int dppd_control_query_age(const struct dppd_control_service *service, uint64_t id, uint64_t expected,
    struct dppd_control_age_result *result)
{
    if (service == NULL || result == NULL || id == 0 || expected == 0)
        return -EINVAL;
    if (service->recovery_state != DPPD_CONTROL_RECOVERY_READY)
        return -EUCLEAN;
    struct dppd_rule rule;
    int rc = dppd_rule_repository_get(&service->rules, id, &rule);
    if (rc != 0)
        return rc;
    if (expected != DPPD_RULE_GENERATION_ANY && expected != rule.generation)
        return -ESTALE;
    if (!has_age(&rule))
        return -ENODATA;
    struct dppd_control_age_result next = {.rule_id = id, .generation = rule.generation};
    struct dppd_flow_error error;
    rc = dppd_rte_flow_backend_query_age(&service->rte_flow, id, rule.generation, &next.state, &error);
    if (rc == 0)
        *result = next;
    return rc;
}

int dppd_control_age_status(const struct dppd_control_service *service, struct dppd_age_status *status)
{
    if (service == NULL || status == NULL)
        return -EINVAL;
    struct dppd_age_status next = service->age;
    next.tracked = 0;
    uint64_t after = 0, observed;
    bool more;
    do {
        struct dppd_rule rules[DPPD_AGE_POLL_BUDGET];
        uint32_t count;
        int rc = dppd_rule_repository_list(&service->rules, after, DPPD_RULE_GENERATION_ANY, rules,
            DPPD_AGE_POLL_BUDGET, &count, &more, &observed);
        if (rc != 0)
            return rc;
        for (uint32_t i = 0; i < count; ++i) {
            next.tracked += has_age(&rules[i]);
            after = rules[i].id;
        }
    } while (more);
    *status = next;
    return 0;
}

int dppd_control_age_poll(struct dppd_control_service *service, uint64_t now_ns, uint32_t budget)
{
    if (service == NULL || budget == 0 || budget > DPPD_AGE_POLL_BUDGET)
        return -EINVAL;
    if (service->recovery_state != DPPD_CONTROL_RECOVERY_READY || service->persistence_dirty)
        return -EUCLEAN;
    if (now_ns < service->age.next_poll_ns)
        return 0;
    struct dppd_rule rules[DPPD_AGE_POLL_BUDGET];
    uint32_t count;
    uint64_t observed;
    bool more;
    int rc = dppd_rule_repository_list(&service->rules, service->age.cursor, DPPD_RULE_GENERATION_ANY, rules, budget,
        &count, &more, &observed);
    if (rc != 0)
        return rc;
    increment(&service->age.polls);
    int first_error = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const struct dppd_rule *rule = &rules[i];
        service->age.cursor = rule->id;
        if (!has_age(rule))
            continue;
        struct dppd_control_age_result state;
        increment(&service->age.queries);
        rc = dppd_control_query_age(service, rule->id, rule->generation, &state);
        if (rc != 0) {
            increment(&service->age.query_errors);
        } else if (state.state.aged) {
            bool removed;
            uint64_t generation;
            /** 复用正式删除和持久化，后续共享对象引用也沿同一条删除路径释放 */
            rc = dppd_control_remove(service, rule->id, rule->generation, &removed, &generation);
            if (rc == 0 && removed)
                increment(&service->age.expired);
            else if (rc == 0)
                rc = -EUCLEAN;
            if (rc != 0)
                increment(&service->age.delete_errors);
        }
        if (rc != 0) {
            service->age.last_error = service->age_cycle_error = rc;
            service->age.last_rule_id = rule->id;
            if (first_error == 0)
                first_error = rc;
        }
        if (service->recovery_state != DPPD_CONTROL_RECOVERY_READY || service->persistence_dirty)
            break;
    }
    /** 错误跨分页保留，只有完整一轮没有错误才能撤销当前错误，避免漏查失败的规则 */
    if (!more) {
        service->age.cursor = 0;
        if (service->age_cycle_error == 0)
            service->age.last_error = 0;
        service->age_cycle_error = 0;
    }
    uint64_t interval = service->age.last_error == 0 ? DPPD_AGE_POLL_INTERVAL_NS : DPPD_AGE_RETRY_INTERVAL_NS;
    service->age.next_poll_ns = now_ns > UINT64_MAX - interval ? UINT64_MAX : now_ns + interval;
    return first_error;
}
