#include "dppd/transaction.h"

#include <errno.h>
#include <stddef.h>

/** 根据规划结果选择软件或硬件的操作接口，让事务流程不用分别编写两套安装逻辑 */
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

/**
 * 校验、准备、提交和回滚是一个后端必须提供的四项能力
 * finalize 只用于释放额外的临时资源，没有这类资源的后端可以不提供它
 */
static int backend_valid(const struct dppd_transaction_backend *backend)
{
    return backend != NULL && backend->validate != NULL &&
           backend->prepare != NULL && backend->commit != NULL &&
           backend->rollback != NULL;
}

/**
 * 尽力撤销本次事务已经占用的资源，不能因为某一条撤销失败就放弃其余条目
 * commit_attempts 表示有多少条规则已经调用过 commit，包括调用了但返回错误的那一条
 * 这个信息帮助后端区分“只预留了资源”和“可能已经产生安装副作用”
 */
static void rollback_items(struct dppd_transaction *transaction,
                           const struct dppd_transaction_backends *backends,
                           uint32_t commit_attempts)
{
    uint32_t i = transaction->nb_items;

    /** 按最后准备或提交的对象优先撤销，避免先释放了后续对象仍可能依赖的资源 */
    transaction->state = DPPD_TRANSACTION_ROLLING_BACK;
    while (i > 0) {
        struct dppd_transaction_item *item;
        const struct dppd_transaction_backend *backend;
        bool commit_was_attempted;
        int rc;

        --i;
        item = &transaction->items[i];
        /**
         * 只经过校验的条目没有占用资源，不需要回滚
         * 失败条目若留下非零 token，说明仍有需要清理的资源，不能仅因为 FAILED 就跳过
         */
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
        /** 保留第一个清理错误，同时继续尝试清理其余条目，便于上层判断是否必须隔离 */
        else if (transaction->rollback_code == 0) {
            transaction->rollback_code = rc;
            transaction->rollback_item = i;
        }
    }
    transaction->state = transaction->rollback_code == 0 ?
                         DPPD_TRANSACTION_ROLLED_BACK :
                         DPPD_TRANSACTION_FAILED;
}

/**
 * 为一批已完成规划的规则建立事务状态，不复制规则数组，也不安装任何对象
 * 调用方持有 items 的内存，必须让它存活到事务结束
 * 这里会清空旧 token，因此不能用 init 代替尚未完成的事务清理
 */
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
    transaction->failure_stage = DPPD_RULE_STAGE_NONE;
    transaction->failure_item = UINT32_MAX;
    transaction->rollback_item = UINT32_MAX;
    for (i = 0; i < nb_items; ++i) {
        items[i].state = DPPD_TRANSACTION_ITEM_PENDING;
        items[i].backend_token = 0;
        items[i].error_code = 0;
    }
    return 0;
}

/**
 * 按“全部校验、全部准备、逐条提交”的顺序安装规则
 * 校验阶段失败直接结束，准备或提交失败则自动回滚已占用的资源
 * 返回值说明原操作为什么失败，rollback_code 另行说明清理是否也失败
 * 返回成功仅表示后端对象已安装，规则账本发布和快照保存仍由控制层负责
 */
int dppd_transaction_run(struct dppd_transaction *transaction,
                         const struct dppd_transaction_backends *backends)
{
    uint32_t i;
    int rc;

    if (transaction == NULL || backends == NULL ||
        transaction->state != DPPD_TRANSACTION_NEW)
        return -EINVAL;

    transaction->state = DPPD_TRANSACTION_VALIDATING;
    /**
     * 第一阶段只询问“能不能做”，不允许后端预留槽位或安装规则
     * 同时核对规划的 ID 和版本确实对应当前规则，避免拿错计划安装另一个版本
     */
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
            transaction->failure_stage = DPPD_RULE_STAGE_VALIDATE;
            transaction->failure_item = i;
            transaction->state = DPPD_TRANSACTION_FAILED;
            return rc;
        }
        item->state = DPPD_TRANSACTION_ITEM_VALIDATED;
    }

    transaction->state = DPPD_TRANSACTION_PREPARING;
    /**
     * 第二阶段为每条规则准备可撤销的资源，例如空槽位或软件发布需要的临时对象
     * 后一条准备失败时，前面已经准备好的资源也必须归还
     * 传入零次 commit 尝试，告诉回滚逻辑这一阶段还没有正式安装对象
     */
    for (i = 0; i < transaction->nb_items; ++i) {
        struct dppd_transaction_item *item = &transaction->items[i];
        const struct dppd_transaction_backend *backend =
            backend_for(backends, item->plan.backend);

        rc = backend->prepare(backend->context, item, &item->backend_token);
        if (rc != 0) {
            item->state = DPPD_TRANSACTION_ITEM_FAILED;
            item->error_code = rc;
            transaction->failure_code = rc;
            transaction->failure_stage = DPPD_RULE_STAGE_PREPARE;
            transaction->failure_item = i;
            rollback_items(transaction, backends, 0);
            return rc;
        }
        item->state = DPPD_TRANSACTION_ITEM_PREPARED;
    }

    transaction->state = DPPD_TRANSACTION_COMMITTING;
    /**
     * 第三阶段才真正安装对象，每次提交都使用对应 prepare 返回的 token
     * 提交报错不一定代表完全没有副作用，因此失败条目本身也要参加回滚
     */
    for (i = 0; i < transaction->nb_items; ++i) {
        struct dppd_transaction_item *item = &transaction->items[i];
        const struct dppd_transaction_backend *backend =
            backend_for(backends, item->plan.backend);

        rc = backend->commit(backend->context, item, item->backend_token);
        if (rc != 0) {
            item->state = DPPD_TRANSACTION_ITEM_FAILED;
            item->error_code = rc;
            transaction->failure_code = rc;
            transaction->failure_stage = DPPD_RULE_STAGE_COMMIT;
            transaction->failure_item = i;
            rollback_items(transaction, backends, i + 1U);
            return rc;
        }
        item->state = DPPD_TRANSACTION_ITEM_COMMITTED;
    }

    transaction->state = DPPD_TRANSACTION_COMMITTED;
    return 0;
}

/**
 * 撤销一个已经安装成功、但尚未最终确认的事务
 * 例如批量更新的新版本全部装好后，旧版本删除失败，就需要撤销整批新版本
 * 已经 finalize 的事务不再拥有回滚资源，因此不允许通过这个入口继续撤销
 */
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

/**
 * 结束本次事务的临时资源生命周期，不删除已经安装的规则
 * 正常流程在规则账本发布后调用；异常流程决定隔离且不再回滚时也可用于收尾
 * 逐项清零 token，表示调用方已经放弃再次提交或回滚本次事务的可能
 * finalize 回调不能报错，因此后端必须把它设计成单纯的临时资源释放动作
 */
int dppd_transaction_finalize(struct dppd_transaction *transaction,
                              const struct dppd_transaction_backends *backends)
{
    uint32_t i;

    if (transaction == NULL || backends == NULL ||
        transaction->state != DPPD_TRANSACTION_COMMITTED)
        return -EINVAL;
    for (i = 0; i < transaction->nb_items; ++i) {
        struct dppd_transaction_item *item = &transaction->items[i];
        const struct dppd_transaction_backend *backend =
            backend_for(backends, item->plan.backend);

        if (item->state != DPPD_TRANSACTION_ITEM_COMMITTED ||
            !backend_valid(backend))
            return -EUCLEAN;
        /** 后端没有额外清理回调时也要清空 token，并让整个事务进入不可回滚的终态 */
        if (backend->finalize != NULL)
            backend->finalize(backend->context, item, item->backend_token);
        item->backend_token = 0;
    }
    transaction->state = DPPD_TRANSACTION_FINALIZED;
    return 0;
}
