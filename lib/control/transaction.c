#include "dppd/transaction.h"

#include <errno.h>
#include <stddef.h>

static const struct dppd_transaction_backend *backend_for(
    const struct dppd_transaction_backends *backends,
    enum dppd_plan_backend backend)
{
    switch (backend) {
    case DPPD_PLAN_BACKEND_SOFTWARE:
        return &backends->software;
    case DPPD_PLAN_BACKEND_RTE_FLOW:
        return &backends->rte_flow;
    }
    return NULL;
}

static int backend_valid(const struct dppd_transaction_backend *backend)
{
    return backend != NULL && backend->validate != NULL &&
           backend->prepare != NULL && backend->commit != NULL &&
           backend->rollback != NULL;
}

static void rollback_items(struct dppd_transaction *transaction,
                           const struct dppd_transaction_backends *backends,
                           uint32_t commit_attempts)
{
    uint32_t i = transaction->nb_items;

    /* 逆序释放，避免后提交对象依赖先提交对象时破坏回滚顺序。 */
    transaction->state = DPPD_TRANSACTION_ROLLING_BACK;
    while (i > 0) {
        struct dppd_transaction_item *item;
        const struct dppd_transaction_backend *backend;
        bool commit_was_attempted;
        int rc;

        --i;
        item = &transaction->items[i];
        if (item->state != DPPD_TRANSACTION_ITEM_PREPARED &&
            item->state != DPPD_TRANSACTION_ITEM_COMMITTED &&
            item->state != DPPD_TRANSACTION_ITEM_FAILED)
            continue;
        if (item->state == DPPD_TRANSACTION_ITEM_FAILED &&
            item->backend_token == 0)
            continue;
        backend = backend_for(backends, item->plan.backend);
        commit_was_attempted = i < commit_attempts;
        rc = backend->rollback(backend->context, item, item->backend_token,
                               commit_was_attempted);
        if (rc == 0)
            item->state = DPPD_TRANSACTION_ITEM_ROLLED_BACK;
        else if (transaction->rollback_code == 0)
            transaction->rollback_code = rc;
    }
    transaction->state = transaction->rollback_code == 0 ?
                         DPPD_TRANSACTION_ROLLED_BACK :
                         DPPD_TRANSACTION_FAILED;
}

int dppd_transaction_init(struct dppd_transaction *transaction,
                          uint64_t transaction_id,
                          struct dppd_transaction_item *items,
                          uint32_t nb_items)
{
    uint32_t i;

    if (transaction == NULL || transaction_id == 0 ||
        items == NULL || nb_items == 0)
        return -EINVAL;
    transaction->id = transaction_id;
    transaction->state = DPPD_TRANSACTION_NEW;
    transaction->items = items;
    transaction->nb_items = nb_items;
    transaction->failure_code = 0;
    transaction->rollback_code = 0;
    for (i = 0; i < nb_items; ++i) {
        items[i].state = DPPD_TRANSACTION_ITEM_PENDING;
        items[i].backend_token = 0;
        items[i].error_code = 0;
    }
    return 0;
}

int dppd_transaction_run(struct dppd_transaction *transaction,
                         const struct dppd_transaction_backends *backends)
{
    uint32_t i;
    int rc;

    if (transaction == NULL || backends == NULL ||
        transaction->state != DPPD_TRANSACTION_NEW)
        return -EINVAL;

    transaction->state = DPPD_TRANSACTION_VALIDATING;
    /* 第一阶段只验证，全批通过前不允许 backend 分配或安装资源。 */
    for (i = 0; i < transaction->nb_items; ++i) {
        struct dppd_transaction_item *item = &transaction->items[i];
        const struct dppd_transaction_backend *backend =
            backend_for(backends, item->plan.backend);

        if (item->rule.id == 0 || item->plan.rule_id != item->rule.id ||
            item->plan.rule_generation != item->rule.generation)
            rc = -EINVAL;
        else if (!backend_valid(backend))
            rc = -ENOTSUP;
        else
            rc = backend->validate(backend->context, item);
        if (rc != 0) {
            item->state = DPPD_TRANSACTION_ITEM_FAILED;
            item->error_code = rc;
            transaction->failure_code = rc;
            transaction->state = DPPD_TRANSACTION_FAILED;
            return rc;
        }
        item->state = DPPD_TRANSACTION_ITEM_VALIDATED;
    }

    transaction->state = DPPD_TRANSACTION_PREPARING;
    /* prepare 可以预留资源，但必须能由 rollback 完整撤销。 */
    for (i = 0; i < transaction->nb_items; ++i) {
        struct dppd_transaction_item *item = &transaction->items[i];
        const struct dppd_transaction_backend *backend =
            backend_for(backends, item->plan.backend);

        rc = backend->prepare(backend->context, item, &item->backend_token);
        if (rc != 0) {
            item->state = DPPD_TRANSACTION_ITEM_FAILED;
            item->error_code = rc;
            transaction->failure_code = rc;
            rollback_items(transaction, backends, 0);
            return rc;
        }
        item->state = DPPD_TRANSACTION_ITEM_PREPARED;
    }

    transaction->state = DPPD_TRANSACTION_COMMITTING;
    for (i = 0; i < transaction->nb_items; ++i) {
        struct dppd_transaction_item *item = &transaction->items[i];
        const struct dppd_transaction_backend *backend =
            backend_for(backends, item->plan.backend);

        rc = backend->commit(backend->context, item, item->backend_token);
        if (rc != 0) {
            item->state = DPPD_TRANSACTION_ITEM_FAILED;
            item->error_code = rc;
            transaction->failure_code = rc;
            rollback_items(transaction, backends, i + 1U);
            return rc;
        }
        item->state = DPPD_TRANSACTION_ITEM_COMMITTED;
    }

    transaction->state = DPPD_TRANSACTION_COMMITTED;
    return 0;
}

int dppd_transaction_rollback_committed(
    struct dppd_transaction *transaction,
    const struct dppd_transaction_backends *backends)
{
    if (transaction == NULL || backends == NULL ||
        transaction->state != DPPD_TRANSACTION_COMMITTED)
        return -EINVAL;
    rollback_items(transaction, backends, transaction->nb_items);
    return transaction->rollback_code;
}
