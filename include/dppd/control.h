#ifndef DPPD_CONTROL_H
#define DPPD_CONTROL_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/planner.h"
#include "dppd/rte_flow_backend.h"
#include "dppd/rule_repository.h"

struct dppd_control_service {
    const struct dppd_topology *topology;
    struct dppd_rule_repository rules;
    struct dppd_rte_flow_backend rte_flow;
    uint64_t next_transaction_id;
    /* persistence path 由 service 动态持有；NULL 表示未启用自动 snapshot。 */
    char *persistence_path;
    uint64_t persisted_generation;
    int persistence_last_error;
    bool persistence_dirty;
};

struct dppd_control_apply_result {
    enum dppd_rule_apply_status status;
    uint64_t generation;
    uint64_t transaction_id;
    struct dppd_execution_plan plan;
};

struct dppd_control_count_result {
    /* 回显最终定位到的 desired rule，避免客户端把响应关联到错误对象。 */
    uint64_t rule_id;
    /* 实际查询的已发布 generation。 */
    uint64_t generation;
    /* PMD 返回的累计命中包数和字节数；查询本身不清零。 */
    uint64_t hits;
    uint64_t bytes;
};

struct dppd_control_persistence_status {
    bool enabled;
    bool dirty;
    uint64_t persisted_generation;
    uint64_t current_generation;
    int last_error;
};

int dppd_control_init(struct dppd_control_service *service,
                      const struct dppd_topology *topology,
                      uint32_t rule_capacity,
                      const struct dppd_flow_api *flow_api);
int dppd_control_fini(struct dppd_control_service *service);
/*
 * attach 只建立后续 mutation 的保存关系，不读取也不覆盖文件。
 * 非零 repository generation 会被保守标记为 dirty，直到 flush/preflight 完整保存；
 * 调用方必须先完成 load/reconcile，或确认这是一个全新的 state path。
 */
int dppd_control_persistence_attach(struct dppd_control_service *service,
                                    const char *path);
/*
 * 启动时加载并 fail-closed 重放 snapshot。文件不存在时创建一个空 v2 snapshot；
 * 文件存在时必须全部规则完成 plan/硬件事务后才发布 repository 并启用后续自动保存。
 */
int dppd_control_persistence_restore(struct dppd_control_service *service,
                                     const char *path);
/* 立即重试保存当前完整 repository；可用于清除 dirty state。 */
int dppd_control_persistence_flush(struct dppd_control_service *service);
void dppd_control_persistence_status(
    const struct dppd_control_service *service,
    struct dppd_control_persistence_status *status);
/*
 * apply 的核心不变量：硬件事务成功后才发布 desired generation。
 * 相同规则为幂等重放；不同规则按新 generation 安装，旧 generation
 * 删除成功后才更新 repository。
 */
int dppd_control_apply(struct dppd_control_service *service,
                       uint16_t install_port_id,
                       const struct dppd_rule *rule,
                       uint64_t expected_generation,
                       struct dppd_control_apply_result *result);
int dppd_control_remove(struct dppd_control_service *service,
                        uint64_t rule_id,
                        uint64_t expected_generation,
                        bool *removed,
                        uint64_t *generation);
/*
 * expected_generation 与更新/删除采用同一并发语义。查询只读取当前已发布代，
 * 不允许通过旧 generation 访问更新窗口中已退休的硬件对象。
 */
int dppd_control_query_count(struct dppd_control_service *service,
                             uint64_t rule_id,
                             uint64_t expected_generation,
                             struct dppd_control_count_result *result);

#endif
