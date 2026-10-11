#ifndef DPPD_RECOVERY_PLAN_H
#define DPPD_RECOVERY_PLAN_H

#include "dppd/recovery_inspect.h"

/** 原因只描述本次观察，candidate 也不代表已经获得删除权限 */
enum dppd_recovery_plan_reason {
    DPPD_PLAN_MATCH,
    DPPD_PLAN_REMOVED,
    DPPD_PLAN_REMOVED_TOKEN_PRESENT,
    DPPD_PLAN_NO_TOKEN,
    DPPD_PLAN_NO_EXPECTED,
    DPPD_PLAN_INCOMPLETE,
    DPPD_PLAN_TOKEN_ABSENT,
    DPPD_PLAN_TOKEN_AMBIGUOUS,
    DPPD_PLAN_WRONG_PORT,
    DPPD_PLAN_CONTENT_MISMATCH,
    DPPD_PLAN_CONTENT_UNSUPPORTED,
    DPPD_PLAN_SCOPE_MISMATCH,
    DPPD_PLAN_SHARED_ACTION,
    DPPD_PLAN_SCOPE_UNSUPPORTED
};

struct dppd_recovery_plan_entry {
    enum dppd_recovery_plan_reason reason;
    uint32_t token_matches;
    /** 唯一匹配时指向输入的端口和过滤器下标，否则均为 UINT32_MAX */
    uint32_t matched_port, matched_filter;
    int content_error, scope_error;
};

/** 与恢复记录的尝试顺序一致，不复制大块内核列表，也不保存可执行删除命令 */
struct dppd_recovery_plan {
    bool inventory_complete;
    uint32_t candidates, rejected, excluded;
    uint32_t ports_without_attempts, unassociated_filters;
    struct dppd_recovery_plan_entry entries[DPPD_RECOVERY_ATTEMPT_LIMIT];
};

/**
 * 只分析已经读取的数据，不访问内核或改写恢复文件
 * inspections 和 errors 与 record 的端口顺序一致，任一端口读取不完整都不产生候选
 * 调用方负责持锁核对 revision，候选仍缺少接口实例连续性和检查到删除之间的并发保护
 */
int dppd_recovery_plan_build(const struct dppd_recovery_record *record,
    const struct dppd_recovery_inspection *inspections, const int *errors, struct dppd_recovery_plan *plan);
const char *dppd_recovery_plan_reason_name(enum dppd_recovery_plan_reason reason);

#endif
