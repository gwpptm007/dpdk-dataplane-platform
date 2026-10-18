#ifndef DPPD_FLOW_SERVICE_H
#define DPPD_FLOW_SERVICE_H

#include "dppd/async_flow.h"
#include "dppd/software_backend.h"

struct dppd_flow_service_config {
    struct dppd_admission *admission;
    struct dppd_flow_templates *templates;
    struct dppd_async_flow_queue *queues[8];
    /** 非空时只接受 software-only，服务独占该 classifier 的写入，不与旧控制面混用 */
    struct dppd_software_backend *software;
    uint32_t rule_capacity, queue_count, depth;
    uint64_t timeout_ns;
    uint16_t port_id;
};

struct dppd_flow_job_id {
    uint64_t owner, serial;
    uint32_t slot;
};

struct dppd_flow_remove_request {
    uint64_t id, generation;
};

struct dppd_flow_job_result {
    uint32_t count;
    int status;
    bool applied, remove;
    uint64_t ids[64], generations[64];
};

struct dppd_flow_service_status {
    uint32_t rules, jobs, completed;
    uint64_t revision;
    int last_error;
    bool isolated;
};

struct dppd_flow_service;

/** 借用空队列及空 classifier，之后独占它们的写入和完成领取，所有依赖晚于服务关闭 */
int dppd_flow_service_create(const struct dppd_flow_service_config *config, struct dppd_flow_service **output);
int dppd_flow_service_close(struct dppd_flow_service **service);
/** 多生产者复制完整批次，受理后不可修改，结果领取前始终占有相应队列的加权深度 */
int dppd_flow_service_submit(struct dppd_flow_service *service, uint32_t queue,
    const struct dppd_async_flow_create_request *requests, uint32_t count, uint64_t now_ns,
    struct dppd_flow_job_id *job);
int dppd_flow_service_remove(struct dppd_flow_service *service, uint32_t queue,
    const struct dppd_flow_remove_request *requests, uint32_t count, uint64_t now_ns, struct dppd_flow_job_id *job);
/** 同一控制消费者按受理顺序执行完整批次，驱动完成仍可乱序，不等待硬件 */
int dppd_flow_service_process(struct dppd_flow_service *service, uint64_t now_ns, uint32_t budget);
/** 未完成返回 EAGAIN，补偿残留返回 EUCLEAN 且保留任务，只有完整终态才释放回执槽位 */
int dppd_flow_service_collect(struct dppd_flow_service *service, const struct dppd_flow_job_id *job,
    struct dppd_flow_job_result *result);
int dppd_flow_service_cancel(struct dppd_flow_service *service, const struct dppd_flow_job_id *job);
/** 重试已知补偿，完成后再次调用以核对队列及解除隔离，不会抹掉未知实际状态 */
int dppd_flow_service_retry(struct dppd_flow_service *service);
int dppd_flow_service_get(struct dppd_flow_service *service, uint64_t id, struct dppd_rule *rule);
int dppd_flow_service_status(struct dppd_flow_service *service, struct dppd_flow_service_status *status);

#endif
