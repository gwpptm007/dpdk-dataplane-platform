#include <errno.h>
#include <string.h>
#include "dppd/recovery_plan.h"

static bool inspection_complete(const struct dppd_recovery_inspection *inspection, int error)
{
    return error == 0 && (inspection->state == DPPD_INSPECT_PRESENT || inspection->state == DPPD_INSPECT_ABSENT);
}

static bool token_equal(const struct dppd_recovery_filter *filter, const struct dppd_recovery_attempt *attempt)
{
    return attempt->kind == DPPD_RECOVERY_OBJECT_FLOW && attempt->shared_id == 0 && attempt->shared_generation == 0 &&
        filter->cookie_valid && dppd_tap_cookie_present(attempt->owner_cookie) &&
        memcmp(filter->cookie, attempt->owner_cookie, DPPD_TAP_COOKIE_SIZE) == 0;
}

/** 跨全部已登记端口查重，复制到另一个端口的标识也不能被当作唯一候选 */
static void locate_token(const struct dppd_recovery_record *record, const struct dppd_recovery_inspection *inspections,
    const int *errors, const struct dppd_recovery_attempt *attempt, struct dppd_recovery_plan_entry *entry)
{
    for (uint32_t port = 0; port < record->count; ++port) {
        if (!inspection_complete(&inspections[port], errors[port]))
            continue;
        for (uint32_t row = 0; row < inspections[port].count; ++row) {
            if (!token_equal(&inspections[port].filters[row], attempt))
                continue;
            ++entry->token_matches;
            entry->matched_port = port;
            entry->matched_filter = row;
        }
    }
    if (entry->token_matches != 1)
        entry->matched_port = entry->matched_filter = UINT32_MAX;
}

/** 先检查能否定位，再解释内容和范围，主拒绝原因按这个顺序选择 */
static enum dppd_recovery_plan_reason classify(const struct dppd_recovery_record *record,
    const struct dppd_recovery_inspection *inspections, const struct dppd_recovery_attempt *attempt, bool complete,
    struct dppd_recovery_plan_entry *entry)
{
    if (attempt->phase == DPPD_RECOVERY_REMOVED)
        return entry->token_matches != 0 ? DPPD_PLAN_REMOVED_TOKEN_PRESENT : DPPD_PLAN_REMOVED;
    /** TC 列表无法证明间接对象不存在，带共享引用的规则也不能进入独立 TAP 清理 */
    if (attempt->kind != DPPD_RECOVERY_OBJECT_FLOW)
        return DPPD_PLAN_INDIRECT_OBJECT;
    if (attempt->shared_id != 0 || attempt->shared_generation != 0)
        return DPPD_PLAN_SHARED_REFERENCE;
    if (!dppd_tap_cookie_present(attempt->owner_cookie))
        return DPPD_PLAN_NO_TOKEN;
    if (attempt->expected.version != 1)
        return DPPD_PLAN_NO_EXPECTED;
    if (!complete)
        return DPPD_PLAN_INCOMPLETE;
    if (entry->token_matches == 0)
        return DPPD_PLAN_TOKEN_ABSENT;
    if (entry->token_matches != 1)
        return DPPD_PLAN_TOKEN_AMBIGUOUS;
    if (record->ports[entry->matched_port].port_id != attempt->port_id)
        return DPPD_PLAN_WRONG_PORT;

    const struct dppd_recovery_filter *filter = &inspections[entry->matched_port].filters[entry->matched_filter];
    entry->content_error = dppd_recovery_verify(&attempt->expected, filter, false);
    entry->scope_error = dppd_recovery_verify(&attempt->expected, filter, true);
    if (entry->content_error != 0)
        return entry->content_error == -ESTALE ? DPPD_PLAN_CONTENT_MISMATCH : DPPD_PLAN_CONTENT_UNSUPPORTED;
    if (entry->scope_error == -EMLINK)
        return DPPD_PLAN_SHARED_ACTION;
    if (entry->scope_error != 0)
        return entry->scope_error == -ESTALE ? DPPD_PLAN_SCOPE_MISMATCH : DPPD_PLAN_SCOPE_UNSUPPORTED;
    return DPPD_PLAN_MATCH;
}

int dppd_recovery_plan_build(const struct dppd_recovery_record *record,
    const struct dppd_recovery_inspection *inspections, const int *errors, int observation_error,
    struct dppd_recovery_plan *plan)
{
    if (record == NULL || plan == NULL || record->count > DPPD_MAX_PORTS ||
        record->attempt_count > DPPD_RECOVERY_ATTEMPT_LIMIT ||
        (record->count != 0 && (inspections == NULL || errors == NULL)))
        return -EINVAL;
    memset(plan, 0, sizeof(*plan));
    plan->inventory_complete = observation_error == 0;
    for (uint32_t port = 0; port < record->count; ++port) {
        const struct dppd_recovery_inspection *inspection = &inspections[port];
        if (inspection->count > DPPD_RECOVERY_FILTER_LIMIT ||
            (inspection->state == DPPD_INSPECT_ABSENT && inspection->count != 0))
            return -EINVAL;
        bool complete = inspection_complete(inspection, errors[port]);
        plan->inventory_complete &= complete;
        bool has_attempt = false;
        for (uint32_t index = 0; index < record->attempt_count; ++index)
            has_attempt |= record->attempts[index].port_id == record->ports[port].port_id;
        plan->ports_without_attempts += !has_attempt;
        if (!complete)
            continue;
        /* 无标识或不属于任何保留记录的对象只计数，不借用相似坐标把它们纳入候选 */
        for (uint32_t row = 0; row < inspection->count; ++row) {
            bool associated = false;
            for (uint32_t index = 0; index < record->attempt_count; ++index)
                associated |= token_equal(&inspection->filters[row], &record->attempts[index]);
            plan->unassociated_filters += !associated;
        }
    }
    for (uint32_t index = 0; index < record->attempt_count; ++index) {
        const struct dppd_recovery_attempt *attempt = &record->attempts[index];
        struct dppd_recovery_plan_entry *entry = &plan->entries[index];
        bool known_port = false;
        for (uint32_t port = 0; port < record->count; ++port)
            known_port |= record->ports[port].port_id == attempt->port_id;
        if (!known_port)
            return -EINVAL;
        entry->content_error = entry->scope_error = -ENODATA;
        locate_token(record, inspections, errors, attempt, entry);
        entry->reason = classify(record, inspections, attempt, plan->inventory_complete, entry);
        if (entry->reason == DPPD_PLAN_MATCH)
            ++plan->candidates;
        else if (entry->reason == DPPD_PLAN_REMOVED)
            ++plan->excluded;
        else
            ++plan->rejected;
    }
    return 0;
}

const char *dppd_recovery_plan_reason_name(enum dppd_recovery_plan_reason reason)
{
    switch (reason) {
    case DPPD_PLAN_MATCH:
        return "observed-match";
    case DPPD_PLAN_REMOVED:
        return "removed-record";
    case DPPD_PLAN_REMOVED_TOKEN_PRESENT:
        return "removed-token-present";
    case DPPD_PLAN_NO_TOKEN:
        return "missing-token";
    case DPPD_PLAN_NO_EXPECTED:
        return "missing-expected-content";
    case DPPD_PLAN_INCOMPLETE:
        return "inventory-incomplete";
    case DPPD_PLAN_TOKEN_ABSENT:
        return "token-absent";
    case DPPD_PLAN_TOKEN_AMBIGUOUS:
        return "token-ambiguous";
    case DPPD_PLAN_WRONG_PORT:
        return "wrong-port";
    case DPPD_PLAN_CONTENT_MISMATCH:
        return "content-mismatch";
    case DPPD_PLAN_CONTENT_UNSUPPORTED:
        return "content-unsupported";
    case DPPD_PLAN_SCOPE_MISMATCH:
        return "scope-mismatch";
    case DPPD_PLAN_SHARED_ACTION:
        return "shared-action";
    case DPPD_PLAN_SCOPE_UNSUPPORTED:
        return "scope-unsupported";
    case DPPD_PLAN_INDIRECT_OBJECT:
        return "indirect-object-requires-external-cleanup";
    case DPPD_PLAN_SHARED_REFERENCE:
        return "shared-reference-requires-external-cleanup";
    default:
        return "unknown";
    }
}
