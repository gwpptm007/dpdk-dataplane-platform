#ifndef DPPD_FLOW_MANAGEMENT_H
#define DPPD_FLOW_MANAGEMENT_H

#include "dppd/management.h"

/** 最大八条队列各保留二百五十六个单规则回执，不因客户端断开而遗失关闭所需的任务 */
struct dppd_flow_management {
    struct dppd_flow_store *store;
    uint64_t session;
    struct dppd_flow_job_id jobs[2048];
};

int dppd_flow_management_init(struct dppd_flow_management *flows, struct dppd_flow_store *store);
int dppd_flow_management_handle(struct dppd_flow_management *flows, struct dppd_runtime *runtime,
    const struct dppd_management_request *request, struct dppd_management_response *response);
/** 关闭入口并停止 worker 后取消未执行任务，领取完整终态，绝不把脏配置当作已保存 */
int dppd_flow_management_drain(struct dppd_flow_management *flows);

#endif
