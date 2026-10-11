#include <assert.h>
#include <errno.h>
#include <string.h>
#include "dppd/recovery_plan.h"

static struct dppd_recovery_record record, saved_record;
static struct dppd_recovery_inspection inspections[2], saved_inspections[2];
static struct dppd_recovery_plan plan;
static int errors[2];

/** 端口编号故意不同于数组下标，两个有效对象外再保留一条已删除记录 */
static void reset_fixture(void)
{
    memset(&record, 0, sizeof(record));
    memset(inspections, 0, sizeof(inspections));
    memset(errors, 0, sizeof(errors));
    record.format = 5;
    record.count = 2;
    record.attempt_count = 3;
    struct dppd_rule rule = {.priority = 9, .nb_matches = 1, .nb_actions = 1};
    rule.matches[0].type = DPPD_MATCH_ETH;
    rule.actions[0].type = DPPD_ACTION_DROP;
    for (unsigned int index = 0; index < 2; ++index) {
        record.ports[index].port_id = 7 + 2 * index;
        struct dppd_recovery_attempt *attempt = &record.attempts[index];
        attempt->id = index + 1;
        attempt->port_id = record.ports[index].port_id;
        attempt->phase = index == 0 ? DPPD_RECOVERY_INTENT : DPPD_RECOVERY_CREATED;
        attempt->owner_cookie[0] = index + 1;
        assert(dppd_tap_content_from_rule(&rule, 16, &attempt->expected) == 0);
        inspections[index].state = DPPD_INSPECT_PRESENT;
        inspections[index].count = 1;
        struct dppd_recovery_filter *filter = &inspections[index].filters[0];
        filter->cookie_valid = true;
        memcpy(filter->cookie, attempt->owner_cookie, DPPD_TAP_COOKIE_SIZE);
        filter->content = attempt->expected;
        strcpy(filter->kind, "flower");
        filter->parent = attempt->expected.parent;
        filter->priority = attempt->expected.priority;
        filter->protocol = attempt->expected.protocol;
        filter->handle = index + 11;
    }
    record.attempts[2] = record.attempts[0];
    record.attempts[2].id = 3;
    record.attempts[2].owner_cookie[0] = 3;
    record.attempts[2].phase = DPPD_RECOVERY_REMOVED;
}

/** 每次分类同时确认输入字节完全没变，计划不得反向修补记录或内核观察 */
static void expect_reason(enum dppd_recovery_plan_reason reason)
{
    saved_record = record;
    memcpy(saved_inspections, inspections, sizeof(inspections));
    assert(dppd_recovery_plan_build(&record, inspections, errors, &plan) == 0);
    assert(plan.entries[0].reason == reason);
    assert(memcmp(&record, &saved_record, sizeof(record)) == 0);
    assert(memcmp(inspections, saved_inspections, sizeof(inspections)) == 0);
    assert(plan.candidates + plan.rejected + plan.excluded == record.attempt_count);
}

static void test_locations(void)
{
    reset_fixture();
    expect_reason(DPPD_PLAN_MATCH);
    assert(plan.candidates == 2 && plan.excluded == 1 && plan.rejected == 0);
    assert(plan.inventory_complete && plan.unassociated_filters == 0 && plan.ports_without_attempts == 0);
    assert(plan.entries[0].matched_port == 0 && plan.entries[0].matched_filter == 0);
    /* 创建结果没来得及落盘或创建报告失败，都不能抹掉已经可见的同标识对象 */
    record.attempts[0].phase = DPPD_RECOVERY_CREATE_FAILED;
    record.attempts[0].create_error = -EIO;
    expect_reason(DPPD_PLAN_MATCH);
    record.attempts[0].phase = DPPD_RECOVERY_REMOVED;
    expect_reason(DPPD_PLAN_REMOVED_TOKEN_PRESENT);
    inspections[0].count = 0;
    expect_reason(DPPD_PLAN_REMOVED);
    reset_fixture();
    inspections[0].filters[1] = inspections[0].filters[0];
    inspections[0].filters[1].handle = 99;
    inspections[0].count = 2;
    expect_reason(DPPD_PLAN_TOKEN_AMBIGUOUS);
    assert(plan.entries[0].token_matches == 2 && plan.entries[0].matched_port == UINT32_MAX);
    /* 第二个端口的副本也要计入歧义，原端口缺失时则单独报告端口不符 */
    inspections[1].filters[1] = inspections[0].filters[1];
    inspections[0].count = 1;
    inspections[1].count = 2;
    expect_reason(DPPD_PLAN_TOKEN_AMBIGUOUS);
    inspections[0].count = 0;
    expect_reason(DPPD_PLAN_WRONG_PORT);
    assert(plan.entries[0].matched_port == 1 && plan.entries[0].matched_filter == 1);
    inspections[1].count = 1;
    expect_reason(DPPD_PLAN_TOKEN_ABSENT);
    assert(plan.entries[0].matched_filter == UINT32_MAX);
    inspections[0].state = DPPD_INSPECT_ABSENT;
    expect_reason(DPPD_PLAN_TOKEN_ABSENT);
}

static void test_verification(void)
{
    reset_fixture();
    struct dppd_recovery_filter *filter = &inspections[0].filters[0];
    filter->content.action = 2; /* 规范内容中的 QUEUE 编号 */
    expect_reason(DPPD_PLAN_CONTENT_MISMATCH);
    assert(plan.entries[0].content_error == -ESTALE && plan.entries[0].scope_error == 0);
    filter->content = record.attempts[0].expected;
    filter->content.priority++;
    expect_reason(DPPD_PLAN_SCOPE_MISMATCH);
    assert(plan.entries[0].content_error == 0 && plan.entries[0].scope_error == -ESTALE);
    filter->content = record.attempts[0].expected;
    filter->scope_error = -EMLINK;
    expect_reason(DPPD_PLAN_SHARED_ACTION);
    filter->scope_error = -ENOTSUP;
    expect_reason(DPPD_PLAN_SCOPE_UNSUPPORTED);
    filter->content_error = -ENOTSUP;
    expect_reason(DPPD_PLAN_CONTENT_UNSUPPORTED);
    filter->content_error = filter->scope_error = 0;
    filter->content.version = 0;
    expect_reason(DPPD_PLAN_CONTENT_UNSUPPORTED);
    reset_fixture();
    record.format = 4;
    memset(&record.attempts[0].expected, 0, sizeof(record.attempts[0].expected));
    expect_reason(DPPD_PLAN_NO_EXPECTED);
    memset(record.attempts[0].owner_cookie, 0, DPPD_TAP_COOKIE_SIZE);
    record.attempts[0].evidence = DPPD_EVIDENCE_SINGLE_ADDITION;
    record.attempts[0].candidate = inspections[0].filters[0];
    expect_reason(DPPD_PLAN_NO_TOKEN);
    assert(plan.unassociated_filters == 1);
}

static void test_partial_inventory(void)
{
    const enum dppd_recovery_inspection_state unavailable[] = {DPPD_INSPECT_UNAVAILABLE, DPPD_INSPECT_UNSUPPORTED,
        DPPD_INSPECT_NO_IDENTITY, DPPD_INSPECT_CONTEXT_MISMATCH, DPPD_INSPECT_IDENTITY_MISMATCH};
    for (unsigned int index = 0; index < sizeof(unavailable) / sizeof(unavailable[0]); ++index) {
        reset_fixture();
        inspections[1].state = unavailable[index];
        expect_reason(DPPD_PLAN_INCOMPLETE);
        assert(!plan.inventory_complete && plan.candidates == 0 && plan.rejected == 2);
    }
    reset_fixture();
    errors[1] = -EINTR;
    expect_reason(DPPD_PLAN_INCOMPLETE);
    assert(!plan.inventory_complete && plan.candidates == 0);
    reset_fixture();
    inspections[0].filters[1] = inspections[0].filters[0];
    inspections[0].filters[1].cookie_valid = false;
    inspections[0].count = 2;
    expect_reason(DPPD_PLAN_MATCH);
    assert(plan.unassociated_filters == 1);
    record.attempt_count = 1;
    expect_reason(DPPD_PLAN_MATCH);
    assert(plan.ports_without_attempts == 1 && plan.unassociated_filters == 2);
    /* 旧端口线索没有逐次记录时仍保留缺口，不能把零候选解释成已经清理 */
    record.attempt_count = 0;
    record.format = 2;
    assert(dppd_recovery_plan_build(&record, inspections, errors, &plan) == 0);
    assert(plan.ports_without_attempts == 2 && plan.unassociated_filters == 3 && plan.candidates == 0);
    record.count = 0;
    assert(dppd_recovery_plan_build(&record, NULL, NULL, &plan) == 0);
    assert(plan.inventory_complete && plan.candidates == 0 && plan.ports_without_attempts == 0);
}

static void test_invalid_input(void)
{
    reset_fixture();
    assert(dppd_recovery_plan_build(NULL, inspections, errors, &plan) == -EINVAL);
    assert(dppd_recovery_plan_build(&record, NULL, errors, &plan) == -EINVAL);
    assert(dppd_recovery_plan_build(&record, inspections, NULL, &plan) == -EINVAL);
    assert(dppd_recovery_plan_build(&record, inspections, errors, NULL) == -EINVAL);
    record.count = DPPD_MAX_PORTS + 1;
    assert(dppd_recovery_plan_build(&record, inspections, errors, &plan) == -EINVAL);
    record.count = 2;
    record.attempt_count = DPPD_RECOVERY_ATTEMPT_LIMIT + 1;
    assert(dppd_recovery_plan_build(&record, inspections, errors, &plan) == -EINVAL);
    reset_fixture();
    inspections[0].count = DPPD_RECOVERY_FILTER_LIMIT + 1;
    assert(dppd_recovery_plan_build(&record, inspections, errors, &plan) == -EINVAL);
    inspections[0].count = 1;
    inspections[0].state = DPPD_INSPECT_ABSENT;
    assert(dppd_recovery_plan_build(&record, inspections, errors, &plan) == -EINVAL);
    reset_fixture();
    record.attempts[0].port_id = 100;
    assert(dppd_recovery_plan_build(&record, inspections, errors, &plan) == -EINVAL);
}

int main(void)
{
    test_locations();
    test_verification();
    test_partial_inventory();
    test_invalid_input();
    return 0;
}
