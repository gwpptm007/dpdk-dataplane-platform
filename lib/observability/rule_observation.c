#include "dppd/rule_observation.h"

#include <errno.h>
#include <string.h>

/** 阶段优先保留保存和隔离语义，其余错误按原始 errno 分类，不把临时错误当作不支持 */
enum dppd_rule_failure_kind dppd_rule_failure_classify(enum dppd_rule_failure_stage stage, int code)
{
    if (code == 0)
        return DPPD_RULE_FAILURE_NONE;
    if (stage == DPPD_RULE_STAGE_PERSIST || stage == DPPD_RULE_STAGE_LOAD)
        return DPPD_RULE_FAILURE_PERSISTENCE;
    if (stage == DPPD_RULE_STAGE_ISOLATION)
        return DPPD_RULE_FAILURE_ISOLATED;
    switch (code) {
    case -EINVAL: case -EBADMSG: case -ERANGE:
        return DPPD_RULE_FAILURE_INPUT;
    case -ESTALE: case -EEXIST: case -EALREADY:
        return DPPD_RULE_FAILURE_CONFLICT;
    case -ENOENT:
        return DPPD_RULE_FAILURE_NOT_FOUND;
    case -ENOTSUP: case -ENOSYS:
        return DPPD_RULE_FAILURE_UNSUPPORTED;
    case -ENOMEM: case -ENOSPC: case -EOVERFLOW:
        return DPPD_RULE_FAILURE_RESOURCE;
    case -ENODEV: case -EIO: case -EFAULT:
        return DPPD_RULE_FAILURE_DEVICE;
    case -EAGAIN: case -EBUSY: case -ETIMEDOUT: case -EINTR:
        return DPPD_RULE_FAILURE_TEMPORARY;
    case -EACCES: case -EPERM:
        return DPPD_RULE_FAILURE_PERMISSION;
    case -EUCLEAN:
        return DPPD_RULE_FAILURE_INCONSISTENT;
    default:
        return DPPD_RULE_FAILURE_INTERNAL;
    }
}

/** 开始一次请求时只重置临时上下文，累计值与最近一次失败保留 */
void dppd_rule_observation_begin(struct dppd_rule_observation *observation,
                                 enum dppd_rule_operation operation)
{
    memset(&observation->context, 0, sizeof(observation->context));
    memset(&observation->failure, 0, sizeof(observation->failure));
    observation->context.operation = operation;
    observation->context.stage = DPPD_RULE_STAGE_PREFLIGHT;
    observation->fallback_rules = 0;
    observation->active = true;
    observation->failed = false;
    observation->applied = false;
    observation->unchanged = false;
}

/** 第一个原始错误确定主原因，后续清理或返回 EUCLEAN 不覆盖它 */
void dppd_rule_observation_fault(struct dppd_rule_observation *observation, int code)
{
    int32_t compensation_code;
    uint64_t compensation_rule_id;

    if (!observation->active || observation->failed || code == 0)
        return;
    /** 即使调用方先登记补偿，随后登记主错误时也不能丢掉已经保存的补偿原因 */
    compensation_code = observation->failure.compensation_code;
    compensation_rule_id = observation->failure.compensation_rule_id;
    observation->failure = observation->context;
    observation->failure.compensation_code = compensation_code;
    observation->failure.compensation_rule_id = compensation_rule_id;
    observation->failure.cause_code = code;
    observation->failure.kind = dppd_rule_failure_classify(observation->context.stage, code);
    observation->failed = true;
}

/** 补偿失败另存首个错误和对应 ID，不能改写最初的创建或删除错误 */
void dppd_rule_observation_compensation(struct dppd_rule_observation *observation,
                                        int code, uint64_t rule_id)
{
    if (!observation->active || code == 0 || observation->failure.compensation_code != 0)
        return;
    observation->failure.compensation_code = code;
    observation->failure.compensation_rule_id = rule_id;
}

/**
 * 只在失败请求的最终错误与生效状态都已确定后追加，内部回滚不另记一条
 * 满容量只覆盖最旧记录，失败 ID 连续递增，成功操作不会留下空洞
 */
static void history_append(struct dppd_rule_failure_history *history,
                           const struct dppd_rule_failure_event *failure)
{
    uint32_t index;

    if (history->exhausted)
        return;
    if (history->revision == DPPD_RULE_HISTORY_REVISION_ANY - 1U) {
        history->exhausted = true;
        return;
    }
    if (history->count == DPPD_RULE_HISTORY_CAPACITY) {
        index = history->head;
        history->head = (history->head + 1U) % DPPD_RULE_HISTORY_CAPACITY;
        history->overwritten++;
    } else {
        index = (history->head + history->count++) % DPPD_RULE_HISTORY_CAPACITY;
    }
    history->entries[index].event_id = ++history->revision;
    history->entries[index].failure = *failure;
}

/** 请求结束才发布指标，批量失败按一笔请求计数，生效后的保存失败另有累计字段 */
void dppd_rule_observation_finish(struct dppd_rule_observation *observation, int result)
{
    struct dppd_rule_metrics *metrics = &observation->metrics;

    if (!observation->active)
        return;
    metrics->operations++;
    if (observation->applied)
        metrics->applied++;
    if (result == 0 || observation->applied)
        metrics->fallback_rules += observation->fallback_rules;
    if (result == 0) {
        metrics->succeeded++;
        if (observation->unchanged)
            metrics->unchanged++;
    } else {
        dppd_rule_observation_fault(observation, result);
        metrics->failed++;
        metrics->failures[observation->failure.kind]++;
        if (observation->applied)
            metrics->failed_after_apply++;
        if (observation->failure.compensation_code != 0)
            metrics->compensation_failures++;
        observation->failure.sequence = metrics->operations;
        observation->failure.response_code = result;
        observation->failure.applied = observation->applied;
        metrics->last = observation->failure;
        history_append(&observation->history, &metrics->last);
    }
    observation->active = false;
}

/**
 * 从环形窗口复制完整的小页，读取没有消费语义，重复查询得到相同记录
 * after 为零从保留窗口开头读取，已被覆盖的未读记录用 gap 明示，空页保留输入游标
 */
int dppd_rule_history_read(const struct dppd_rule_failure_history *history,
    uint64_t after_event_id, uint64_t expected_revision, struct dppd_rule_history_page *page)
{
    if (page == NULL)
        return -EINVAL;
    memset(page, 0, sizeof(*page));
    if (history == NULL)
        return -EINVAL;
    if (history->count > DPPD_RULE_HISTORY_CAPACITY || history->head >= DPPD_RULE_HISTORY_CAPACITY)
        return -EUCLEAN;
    if (expected_revision != DPPD_RULE_HISTORY_REVISION_ANY && expected_revision != history->revision)
        return -ESTALE;
    page->revision = history->revision;
    page->overwritten = history->overwritten;
    page->capacity = DPPD_RULE_HISTORY_CAPACITY;
    page->total = history->count;
    page->exhausted = history->exhausted;
    page->after_event_id = page->next_after = after_event_id;
    if (history->count != 0) {
        page->oldest_event_id = history->entries[history->head].event_id;
        page->newest_event_id = history->entries[
            (history->head + history->count - 1U) % DPPD_RULE_HISTORY_CAPACITY].event_id;
        page->gap = page->oldest_event_id > 1 && after_event_id < page->oldest_event_id - 1U;
    }
    for (uint32_t offset = 0; offset < history->count; ++offset) {
        const struct dppd_rule_history_entry *entry = &history->entries[
            (history->head + offset) % DPPD_RULE_HISTORY_CAPACITY];

        if (entry->event_id <= after_event_id)
            continue;
        if (page->returned == DPPD_RULE_HISTORY_PAGE_SIZE) {
            page->more = true;
            break;
        }
        page->events[page->returned++] = *entry;
        page->next_after = entry->event_id;
    }
    return 0;
}
