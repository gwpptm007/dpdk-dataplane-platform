#ifndef DPPD_FLOW_STATE_H
#define DPPD_FLOW_STATE_H

#include "dppd/flow_service.h"

enum dppd_flow_state_mode { DPPD_FLOW_STATE_SOFTWARE = 1, DPPD_FLOW_STATE_NATIVE = 2 };

struct dppd_flow_state_config {
    struct dppd_admission_config admission;
    enum dppd_flow_state_mode mode;
    uint16_t port_id;
    uint32_t rule_capacity, queue_count, depth, queue_capacity[8];
    uint32_t template_leases, shared_leases, native_counters, native_aging;
    uint64_t timeout_ns;
};

struct dppd_flow_state_table {
    /** 文件内稳定的模板键，运行时重新解析为本进程模板凭据 */
    uint64_t key;
    struct dppd_flow_template_spec spec;
};

struct dppd_flow_state_rule {
    uint64_t table_key;
    uint32_t queue;
    struct dppd_rule rule;
};

struct dppd_flow_state {
    struct dppd_flow_state_config config;
    struct dppd_flow_state_table tables[64];
    struct dppd_shared_count_spec counts[64];
    uint32_t table_count, shared_count, rule_count;
    uint64_t revision, shared_next_generation;
    struct dppd_flow_state_rule *rules;
};

/** 独立版本的完整配置文件，规则使用既有 v5 编码，不保存任何实际句柄和运行时计数 */
int dppd_flow_state_validate(const struct dppd_flow_state *state);
int dppd_flow_state_save(const char *path, const struct dppd_flow_state *state);
/** 初次配置只在文件不存在时创建，存在时明确拒绝，不能覆盖其他启动者已保存的策略 */
int dppd_flow_state_initialize(const char *path, const struct dppd_flow_state *state);
/** output 必须为空，失败时输出不变，损坏与不兼容版本分别返回 EBADMSG 和 EPROTONOSUPPORT */
int dppd_flow_state_load(const char *path, struct dppd_flow_state **output);
void dppd_flow_state_free(struct dppd_flow_state **state);

struct dppd_flow_store_runtime {
    void *context;
    /** 原生模式每次启动都必须独立证明端口没有旧对象，不能把进程重启等同于硬件已清空 */
    int (*verify_empty)(void *context, uint16_t port_id);
    int (*configure)(void *context, const struct dppd_flow_state_config *config);
    struct dppd_flow_shared_count_driver counts;
    struct dppd_async_flow_driver flows;
    struct dppd_flow_template_driver (*templates)(void *context, struct dppd_flow_shared_counts *counts);
};

struct dppd_flow_store_status {
    bool ready, dirty, residual, closing;
    int last_error;
    uint64_t durable_revision;
    struct dppd_flow_service_status service;
};

struct dppd_flow_store;

/**
 * 打开已存在的配置，独占文件写锁并完整重放，只有 ready 后才能取得 classifier 并启动 worker
 * 普通失败保持空输出，已知残留返回 EUCLEAN 并输出实例，调用者必须保留实例并重试关闭
 */
int dppd_flow_store_open(const char *path, const struct dppd_flow_store_runtime *runtime,
    struct dppd_flow_store **output);
/** 先停止并注销 worker，未领取任务返回 EBUSY，已知残留保留凭据和写锁供重试关闭 */
int dppd_flow_store_close(struct dppd_flow_store **store);
int dppd_flow_store_status(struct dppd_flow_store *store, struct dppd_flow_store_status *status);
struct dppd_software_backend *dppd_flow_store_software(struct dppd_flow_store *store);
int dppd_flow_store_get(struct dppd_flow_store *store, uint64_t id, struct dppd_rule *rule);
/** 重启后共享对象身份保留，驱动累计值从零开始，查询不修改配置或计数 */
int dppd_flow_store_query_shared(struct dppd_flow_store *store, uint64_t id, uint64_t generation, uint64_t *hits,
    uint64_t *bytes);
/** 请求内模板使用文件稳定键，实际指针和进程内模板凭据都由本实例解析 */
int dppd_flow_store_submit(struct dppd_flow_store *store, uint32_t queue, const struct dppd_flow_state_rule *rules,
    uint32_t count, uint64_t now_ns, struct dppd_flow_job_id *job);
int dppd_flow_store_update(struct dppd_flow_store *store, uint32_t queue, const struct dppd_flow_state_rule *rules,
    const uint64_t *expected, uint32_t count, uint64_t now_ns, struct dppd_flow_job_id *job);
int dppd_flow_store_remove(struct dppd_flow_store *store, uint32_t queue, const struct dppd_flow_remove_request *rules,
    uint32_t count, uint64_t now_ns, struct dppd_flow_job_id *job);
/** 实际发布后先持久化再允许领取成功回执，写盘失败封闭写入并保留 dirty，flush 成功后才继续 */
int dppd_flow_store_process(struct dppd_flow_store *store, uint64_t now_ns, uint32_t budget);
int dppd_flow_store_collect(struct dppd_flow_store *store, const struct dppd_flow_job_id *job,
    struct dppd_flow_job_result *result);
int dppd_flow_store_cancel(struct dppd_flow_store *store, const struct dppd_flow_job_id *job);
int dppd_flow_store_retry(struct dppd_flow_store *store);
int dppd_flow_store_flush(struct dppd_flow_store *store);

#endif
