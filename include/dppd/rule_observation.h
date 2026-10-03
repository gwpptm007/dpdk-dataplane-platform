#ifndef DPPD_RULE_OBSERVATION_H
#define DPPD_RULE_OBSERVATION_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/planner.h"

/** 分类说明为什么失败，阶段另行说明在哪里失败，不能仅凭 EUCLEAN 猜测是否已生效 */
enum dppd_rule_failure_kind {
    DPPD_RULE_FAILURE_NONE = 0,
    DPPD_RULE_FAILURE_INPUT,
    DPPD_RULE_FAILURE_CONFLICT,
    DPPD_RULE_FAILURE_NOT_FOUND,
    DPPD_RULE_FAILURE_UNSUPPORTED,
    DPPD_RULE_FAILURE_RESOURCE,
    DPPD_RULE_FAILURE_DEVICE,
    DPPD_RULE_FAILURE_TEMPORARY,
    DPPD_RULE_FAILURE_PERMISSION,
    DPPD_RULE_FAILURE_PERSISTENCE,
    DPPD_RULE_FAILURE_INCONSISTENT,
    DPPD_RULE_FAILURE_ISOLATED,
    DPPD_RULE_FAILURE_INTERNAL,
    DPPD_RULE_FAILURE_KIND_COUNT,
};

/** 保留最初失败的阶段，后面的回滚不会把创建失败覆盖为“回滚阶段失败” */
enum dppd_rule_failure_stage {
    DPPD_RULE_STAGE_NONE = 0,
    DPPD_RULE_STAGE_PREFLIGHT,
    DPPD_RULE_STAGE_PLAN,
    DPPD_RULE_STAGE_VALIDATE,
    DPPD_RULE_STAGE_PREPARE,
    DPPD_RULE_STAGE_COMMIT,
    DPPD_RULE_STAGE_REMOVE,
    DPPD_RULE_STAGE_PUBLISH,
    DPPD_RULE_STAGE_PERSIST,
    DPPD_RULE_STAGE_COMPENSATE,
    DPPD_RULE_STAGE_LOAD,
    DPPD_RULE_STAGE_RECONCILE,
    DPPD_RULE_STAGE_ISOLATION,
};

/** 一次公开控制操作计数一次，批量中的成员和内部恢复动作不另算请求 */
enum dppd_rule_operation {
    DPPD_RULE_OPERATION_NONE = 0,
    DPPD_RULE_OPERATION_APPLY,
    DPPD_RULE_OPERATION_CREATE_BATCH,
    DPPD_RULE_OPERATION_UPDATE_BATCH,
    DPPD_RULE_OPERATION_REMOVE,
    DPPD_RULE_OPERATION_REMOVE_BATCH,
    DPPD_RULE_OPERATION_RESTORE,
    DPPD_RULE_OPERATION_FLUSH,
    DPPD_RULE_OPERATION_RECONCILE,
};

/**
 * 最近一次失败请求的值记录，不持有规则、快照或驱动指针
 * cause 是原始错误，response 是 API 最终错误，compensation 单独保存补偿错误
 * 例如创建返回 EIO、撤销返回 EFAULT 时，response 可为 EUCLEAN，但前两个错误不能丢失
 */
struct dppd_rule_failure_event {
    uint64_t sequence;
    uint64_t rule_id;
    uint64_t generation;
    uint64_t transaction_id;
    uint64_t compensation_rule_id;
    /** 批量整体阶段可能无法定位到一个 ID，此时 rule_id 为零但仍保留请求条数 */
    uint32_t rule_count;
    enum dppd_rule_operation operation;
    enum dppd_rule_failure_stage stage;
    enum dppd_rule_failure_kind kind;
    enum dppd_plan_backend backend;
    int32_t cause_code;
    int32_t response_code;
    int32_t compensation_code;
    uint16_t install_port_id;
    bool port_known;
    bool backend_known;
    /** 规则账本完整发布后才保存失败时为真，false 也不能证明后端没有部分副作用 */
    bool applied;
};

/** 字段清单供管理 CLI 与 telemetry 共用，累计单位是公开控制请求而不是报文 */
#define DPPD_RULE_METRIC_FIELDS(X) \
    X(operations) X(succeeded) X(failed) X(unchanged) X(applied) \
    X(fallback_rules) X(compensation_failures) X(failed_after_apply)

/** 本进程累计值和最近一次失败，成功请求与只读查询都不会清除最后的失败记录 */
struct dppd_rule_metrics {
#define DPPD_RULE_METRIC_DECLARE(field) uint64_t field;
    DPPD_RULE_METRIC_FIELDS(DPPD_RULE_METRIC_DECLARE)
#undef DPPD_RULE_METRIC_DECLARE
    uint64_t failures[DPPD_RULE_FAILURE_KIND_COUNT];
    struct dppd_rule_failure_event last;
};

/** 仅管理线程更新的临时上下文，公共指标查询只复制 metrics，不暴露进行中的请求 */
struct dppd_rule_observation {
    struct dppd_rule_metrics metrics;
    struct dppd_rule_failure_event context;
    struct dppd_rule_failure_event failure;
    uint64_t fallback_rules;
    bool active;
    bool failed;
    bool applied;
    bool unchanged;
};

/** 名称在 CLI 和 JSON 中保持一致，非法枚举明确显示 unknown */
static inline const char *dppd_rule_failure_kind_name(enum dppd_rule_failure_kind value)
{
    static const char *const names[] = {"none", "invalid-input", "conflict", "not-found",
        "unsupported", "resource", "device", "temporary", "permission", "persistence",
        "inconsistent", "isolated", "internal"};
    return (unsigned int)value < DPPD_RULE_FAILURE_KIND_COUNT ? names[value] : "unknown";
}

/** 阶段名称描述失败边界，不表示后端已完成对应动作 */
static inline const char *dppd_rule_failure_stage_name(enum dppd_rule_failure_stage value)
{
    static const char *const names[] = {"none", "preflight", "plan", "validate", "prepare",
        "commit", "remove", "publish", "persist", "compensate", "load", "reconcile", "isolation"};
    return (unsigned int)value < sizeof(names) / sizeof(names[0]) ? names[value] : "unknown";
}

/** apply 包含单规则新建、更新和幂等重放，批量操作各有独立名称 */
static inline const char *dppd_rule_operation_name(enum dppd_rule_operation value)
{
    static const char *const names[] = {"none", "apply", "create-batch", "update-batch",
        "remove", "remove-batch", "restore", "flush", "reconcile"};
    return (unsigned int)value < sizeof(names) / sizeof(names[0]) ? names[value] : "unknown";
}

enum dppd_rule_failure_kind dppd_rule_failure_classify(enum dppd_rule_failure_stage stage, int code);
void dppd_rule_observation_begin(struct dppd_rule_observation *observation,
                                 enum dppd_rule_operation operation);
void dppd_rule_observation_fault(struct dppd_rule_observation *observation, int code);
void dppd_rule_observation_compensation(struct dppd_rule_observation *observation,
                                        int code, uint64_t rule_id);
void dppd_rule_observation_finish(struct dppd_rule_observation *observation, int result);

#endif
