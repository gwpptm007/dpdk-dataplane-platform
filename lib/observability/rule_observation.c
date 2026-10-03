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
    }
    observation->active = false;
}
