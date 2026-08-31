#ifndef DPPD_TRANSACTION_H
#define DPPD_TRANSACTION_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/planner.h"

enum dppd_transaction_state {
    DPPD_TRANSACTION_NEW = 0,
    DPPD_TRANSACTION_VALIDATING,
    DPPD_TRANSACTION_PREPARING,
    DPPD_TRANSACTION_COMMITTING,
    DPPD_TRANSACTION_COMMITTED,
    /*
     * desired repository 已成功发布，backend 可释放仅供 rollback 使用的 prepare token。
     * 此后不允许再调用 rollback_committed；若之后持久化失败，由控制层以 dirty 状态
     * 表示“内存 actual/desired 已生效、磁盘快照待补写”，而不是回滚已确认的规则。
     */
    DPPD_TRANSACTION_FINALIZED,
    DPPD_TRANSACTION_ROLLING_BACK,
    DPPD_TRANSACTION_ROLLED_BACK,
    DPPD_TRANSACTION_FAILED,
};

enum dppd_transaction_item_state {
    DPPD_TRANSACTION_ITEM_PENDING = 0,
    DPPD_TRANSACTION_ITEM_VALIDATED,
    DPPD_TRANSACTION_ITEM_PREPARED,
    DPPD_TRANSACTION_ITEM_COMMITTED,
    DPPD_TRANSACTION_ITEM_ROLLED_BACK,
    DPPD_TRANSACTION_ITEM_FAILED,
};

struct dppd_transaction_item {
    struct dppd_rule rule;
    struct dppd_execution_plan plan;
    enum dppd_transaction_item_state state;
    uintptr_t backend_token;
    int error_code;
};

struct dppd_transaction_backend {
    void *context;
    /* validate 不得改变 backend 状态；prepare 只能预留可回滚资源。 */
    int (*validate)(void *context, const struct dppd_transaction_item *item);
    int (*prepare)(void *context, const struct dppd_transaction_item *item,
                   uintptr_t *token);
    int (*commit)(void *context, const struct dppd_transaction_item *item,
                  uintptr_t token);
    /* commit 返回失败也可能已部分生效，因此仍必须支持 rollback。 */
    int (*rollback)(void *context, const struct dppd_transaction_item *item,
                    uintptr_t token, bool commit_was_attempted);
    /*
     * 成功提交后 token 仍可能被 repository 发布失败路径用于 rollback；只有控制面确认
     * actual/desired 均已发布后才调用 finalize。无返回值，释放动作不得再产生失败点。
     */
    void (*finalize)(void *context, const struct dppd_transaction_item *item,
                     uintptr_t token);
};

struct dppd_transaction_backends {
    struct dppd_transaction_backend software;
    struct dppd_transaction_backend rte_flow;
};

struct dppd_transaction {
    uint64_t id;
    enum dppd_transaction_state state;
    struct dppd_transaction_item *items;
    uint32_t nb_items;
    int failure_code;
    int rollback_code;
};

int dppd_transaction_init(struct dppd_transaction *transaction,
                          uint64_t transaction_id,
                          struct dppd_transaction_item *items,
                          uint32_t nb_items);
int dppd_transaction_run(struct dppd_transaction *transaction,
                         const struct dppd_transaction_backends *backends);
/*
 * 对已经完整 COMMITTED 的事务执行逆序回滚。启动恢复在硬件全部创建后、desired
 * state 发布前若遇到内部错误，可用该接口恢复到“没有本轮对象”的 fail-closed 状态。
 */
int dppd_transaction_rollback_committed(
    struct dppd_transaction *transaction,
    const struct dppd_transaction_backends *backends);
/* 提交后不再需要 rollback 时释放 backend 私有 prepare token。 */
/**
 * 确认 actual 和 desired 已一致后，释放仅供失败 rollback 使用的 backend token。
 * finalize 后事务不可再回滚；调用方必须在 repository 发布成功以后才调用。若
 * repository 发布或补偿失败，必须先 rollback_committed 或进入 recovery 隔离，绝不能
 * 先 finalize 再尝试恢复。
 */
int dppd_transaction_finalize(struct dppd_transaction *transaction,
                              const struct dppd_transaction_backends *backends);

#endif
