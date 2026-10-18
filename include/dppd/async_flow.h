#ifndef DPPD_ASYNC_FLOW_H
#define DPPD_ASYNC_FLOW_H

#include <stddef.h>
#include "dppd/flow_template.h"

#define DPPD_ASYNC_FLOW_BATCH 64U
#define DPPD_ASYNC_FLOW_DEPTH 256U

struct dppd_async_driver_result {
    uintptr_t tag;
    int status;
};

/** 回调只负责排队，成功取得的创建句柄即使完成失败也必须等待完成后再销毁 */
struct dppd_async_flow_driver {
    void *context;
    int (*create)(void *context, uint16_t port, uint32_t queue, void *table, const struct dppd_rule *rule,
        uintptr_t tag, void **object);
    int (*destroy)(void *context, uint16_t port, uint32_t queue, void *object, uintptr_t tag);
    int (*push)(void *context, uint16_t port, uint32_t queue);
    int (*pull)(void *context, uint16_t port, uint32_t queue, struct dppd_async_driver_result *results, uint16_t count);
    /** 只能在 destroy 成功完成后释放应用侧包装内存，不在这里再次调用驱动删除 */
    void (*release)(void *context, void *object);
};

struct dppd_async_flow_config {
    struct dppd_admission *admission;
    struct dppd_flow_templates *templates;
    struct dppd_async_flow_driver driver;
    enum dppd_resource_kind flow_resource;
    uint16_t port_id, queue_id;
    uint32_t depth, flow_capacity;
};

struct dppd_async_flow_id {
    uint64_t owner, serial;
    uint32_t slot;
};

struct dppd_async_flow_create_request {
    struct dppd_flow_template_id table;
    struct dppd_rule rule;
};

enum dppd_async_flow_state {
    DPPD_ASYNC_FLOW_QUEUED = 0,
    DPPD_ASYNC_FLOW_CREATING,
    DPPD_ASYNC_FLOW_READY,
    DPPD_ASYNC_FLOW_CREATE_FAILED,
    DPPD_ASYNC_FLOW_DESTROYING,
};

struct dppd_async_flow_result {
    struct dppd_async_flow_id flow;
    uint64_t rule_id, generation;
    uintptr_t tag;
    int status;
    bool destroy, object_exists;
};

struct dppd_async_flow_status {
    uint32_t flows, queued, pending, completed, depth;
    uint64_t submitted, completions, timeouts;
    int last_error;
    bool fault, fatal;
};

struct dppd_async_flow_queue;

/** 一个实例独占一个驱动队列，所有回调由同一把锁串行，允许多个生产者并发提交 */
int dppd_async_flow_queue_create(const struct dppd_async_flow_config *config, struct dppd_async_flow_queue **output);
/** 调用者先停用所有线程，未完成、未领取结果或实际 flow 仍存在时拒绝关闭 */
int dppd_async_flow_queue_close(struct dppd_async_flow_queue **queue);
/** 整批先取得槽位、配额和模板引用再受理，提交成功不代表安装或账本发布成功 */
int dppd_async_flow_submit(struct dppd_async_flow_queue *queue, const struct dppd_async_flow_create_request *requests,
    uint32_t count, uint64_t now_ns, struct dppd_async_flow_id *flows);
/** 创建尚未完成时拒绝销毁，失败完成的创建句柄也必须走本接口清理 */
int dppd_async_flow_destroy(struct dppd_async_flow_queue *queue, const struct dppd_async_flow_id *flow,
    uint64_t now_ns);
int dppd_async_flow_cancel(struct dppd_async_flow_queue *queue, const struct dppd_async_flow_id *flow);
/** 每次最多发送和拉取 budget 项，不等待硬件；超时只隔离并保留全部实际资源 */
int dppd_async_flow_process(struct dppd_async_flow_queue *queue, uint64_t now_ns, uint64_t timeout_ns, uint32_t budget);
int dppd_async_flow_collect(struct dppd_async_flow_queue *queue, struct dppd_async_flow_result *results,
    uint32_t capacity);
int dppd_async_flow_get(struct dppd_async_flow_queue *queue, const struct dppd_async_flow_id *flow,
    enum dppd_async_flow_state *state);
int dppd_async_flow_status(struct dppd_async_flow_queue *queue, struct dppd_async_flow_status *status);
/** 只允许已收齐完成、清除全部 flow 且成功空拉取后的暂态故障恢复，陌生完成不能自行解除 */
int dppd_async_flow_resume(struct dppd_async_flow_queue *queue);
struct dppd_async_flow_driver dppd_native_async_flow_driver(void);

#endif
