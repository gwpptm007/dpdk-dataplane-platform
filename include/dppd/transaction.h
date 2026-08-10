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

#endif
