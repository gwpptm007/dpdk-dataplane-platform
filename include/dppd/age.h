#ifndef DPPD_AGE_H
#define DPPD_AGE_H

#include <stdint.h>
#include "dppd/offload.h"

#define DPPD_AGE_POLL_BUDGET 64U
#define DPPD_AGE_POLL_INTERVAL_NS UINT64_C(100000000)
#define DPPD_AGE_RETRY_INTERVAL_NS UINT64_C(1000000000)

/** 查询失败保留规则，统计同时保留失败次数和当前尚未完成一轮成功核对的错误 */
struct dppd_age_status {
    uint64_t polls, queries, expired, query_errors, delete_errors;
    uint64_t cursor, next_poll_ns, last_rule_id;
    int32_t last_error;
    uint32_t tracked;
};

struct dppd_control_age_result {
    uint64_t rule_id, generation;
    struct dppd_flow_age_result state;
};

struct dppd_control_service;

/** 仅主线程调用，按预算查询驱动 AGE，只有驱动明确到期才删除精确版本并保存账本 */
int dppd_control_age_poll(struct dppd_control_service *service, uint64_t now_ns, uint32_t budget);
/** 状态只读取内存，不向设备发请求，恢复隔离期间也能查看错误和剩余规则 */
int dppd_control_age_status(const struct dppd_control_service *service, struct dppd_age_status *status);
/** 查询一个已发布版本的真实 AGE，不把 COUNT 值或缺失的驱动能力当作空闲证据 */
int dppd_control_query_age(const struct dppd_control_service *service, uint64_t id, uint64_t expected,
    struct dppd_control_age_result *result);

#endif
