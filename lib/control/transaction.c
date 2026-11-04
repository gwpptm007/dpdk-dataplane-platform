#include "dppd/transaction.h"

#include <errno.h>
#include <stddef.h>

/** 根据规划结果选择软件或硬件的操作接口，让事务流程不用分别编写两套安装逻辑 */
static const struct dppd_transaction_backend *backend_for(const struct dppd_transaction_backends *backends,
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
 * 校验、准备、提交和回滚是软件规则安装和网卡规则下发都必须提供的四项能力
 * finalize 只用于释放额外的临时资源，没有这类资源的规则安装实现可以不提供它
 */
static int backend_valid(const struct dppd_transaction_backend *backend)
{
    return backend != NULL && backend->validate != NULL && backend->prepare != NULL && backend->commit != NULL &&
        backend->rollback != NULL;
}

/**
 * 从后往前清理本次事务的条目，具体释放或删除操作由所选的软件规则或网卡规则操作函数执行
 * commit_attempts 是已尝试提交的条目数，包含提交失败的那条
 *
 * 软件示例：R1 已发布，R2 提交失败但未发布，R3 只完成准备
 *   R3 → 释放准备记录
 *   R2 → 释放准备记录
 *   R1 → 删除已发布版本，再释放准备记录
 *
 * 本函数无返回值，清理全部成功时设为 ROLLED_BACK，否则设为 FAILED 并记录首个清理错误
 */
static void rollback_items(struct dppd_transaction *transaction, const struct dppd_transaction_backends *backends,
    uint32_t commit_attempts)
{
    uint32_t i = transaction->nb_items;

    /** 三条规则时，下标依次为 2 → 1 → 0；尚未占用资源的条目在下面跳过 */
    transaction->state = DPPD_TRANSACTION_ROLLING_BACK;
    while (i > 0) {
        struct dppd_transaction_item *item;
        const struct dppd_transaction_backend *backend;
        bool commit_was_attempted;
        int rc;

        --i;
        item = &transaction->items[i];
        /**
         * PREPARED、COMMITTED 需要清理；FAILED 则看 token 是否记录了待清理资源
         * PENDING、VALIDATED 没有准备资源，直接跳过
         */
        if (item->state != DPPD_TRANSACTION_ITEM_PREPARED && item->state != DPPD_TRANSACTION_ITEM_COMMITTED &&
            item->state != DPPD_TRANSACTION_ITEM_FAILED)
            continue;
        if (item->state == DPPD_TRANSACTION_ITEM_FAILED && item->backend_token == 0)
            continue;
        backend = backend_for(backends, item->plan.backend);
        /** R2 提交失败时 commit_attempts = 2：下标 0、1 为 true，下标 2 为 false */
        commit_was_attempted = i < commit_attempts;
        /** 软件计划选择 .rollback = software_backend.c 的 transaction_rollback，token 转交原 pending 地址 */
        rc = backend->rollback(backend->context, item, item->backend_token, commit_was_attempted);
        if (rc == 0)
            item->state = DPPD_TRANSACTION_ITEM_ROLLED_BACK;
        /** 保留第一个清理错误，同时继续尝试清理其余条目，便于上层判断是否必须隔离 */
        else if (transaction->rollback_code == 0) {
            transaction->rollback_code = rc;
            transaction->rollback_item = i;
        }
    }
    transaction->state = transaction->rollback_code == 0 ? DPPD_TRANSACTION_ROLLED_BACK : DPPD_TRANSACTION_FAILED;
}

/**
 * 初始化规则安装事务：记录编号、关联条目，重置状态和错误记录
 *
 * transaction（NEW）
 *   └─ items → 调用方的条目（PENDING，保留 rule 和 plan）
 *
 * items 的内存由调用方保留到事务结束
 * 成功返回 0，参数无效返回 -EINVAL
 */
int dppd_transaction_init(struct dppd_transaction *transaction, uint64_t transaction_id,
    struct dppd_transaction_item *items, uint32_t nb_items)
{
    uint32_t i;

    if (transaction == NULL || transaction_id == 0 || items == NULL || nb_items == 0)
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
    /** 清零 token 不会释放规则安装时占用的资源，复用条目前需先完成旧事务的回滚或收尾 */
    for (i = 0; i < nb_items; ++i) {
        items[i].state = DPPD_TRANSACTION_ITEM_PENDING;
        items[i].backend_token = 0;
        items[i].error_code = 0;
    }
    return 0;
}

/**
 * 按条目的执行计划安装规则，失败时撤销已占用的资源
 *
 * CPU 软件规则安装示例：dppd_transaction_run
 *   ├─ backend->validate(...) → software_backend.c：transaction_validate
 *   │    检查 CPU 软件处理是否支持这条规则
 *   └─ backend->prepare(...)  → software_backend.c：transaction_prepare
 *        分配临时记录，保存规则，把记录地址交回事务
 *
 * 全部 validate → 全部 prepare → 逐条 commit → COMMITTED
 *   校验失败 → 直接返回错误
 *   准备或提交失败 → 逆序回滚，再返回原始错误
 *
 * 返回 0 表示实际规则安装完成，控制层随后更新规则记录、收尾并保存配置
 * 失败时另查 rollback_code：0 表示清理成功，非 0 表示仍有清理错误
 */
int dppd_transaction_run(struct dppd_transaction *transaction, const struct dppd_transaction_backends *backends)
{
    uint32_t i;
    int rc;

    if (transaction == NULL || backends == NULL || transaction->state != DPPD_TRANSACTION_NEW)
        return -EINVAL;

    transaction->state = DPPD_TRANSACTION_VALIDATING;
    /**
     * 校验所选规则处理方式支持的能力，以及规则和计划的 ID、版本是否一致
     * 此阶段不占用规则安装所需资源，失败时无需回滚
     */
    for (i = 0; i < transaction->nb_items; ++i) {
        struct dppd_transaction_item *item = &transaction->items[i];
        /** 本例 plan.backend = SOFTWARE → backends.software → software_backend.c 中的各阶段函数 */
        const struct dppd_transaction_backend *backend = backend_for(backends, item->plan.backend);

        if (item->rule.id == 0 || item->plan.rule_id != item->rule.id ||
            item->plan.rule_generation != item->rule.generation)
            rc = -EINVAL;
        else if (!backend_valid(backend))
            rc = -ENOTSUP;
        else
            /** 调用所选规则处理方式的校验函数，软件计划进入 software_backend.c 的 transaction_validate */
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
     * 为所有条目准备资源，任一条失败都要归还此前已准备的资源
     * 尚未调用 commit，因此回滚时传入的提交次数为 0
     */
    for (i = 0; i < transaction->nb_items; ++i) {
        struct dppd_transaction_item *item = &transaction->items[i];
        const struct dppd_transaction_backend *backend = backend_for(backends, item->plan.backend);

        /**
         * 软件规则安装函数的指针从初始化到调用：
         *   control.c：backends.software = dppd_software_transaction_backend(&service->software)
         *     → 接口保存 context = &service->software、prepare = transaction_prepare
         *     → backend_for 返回 &backends->software
         *     → 下面的调用进入 software_backend.c 的 transaction_prepare
         *
         * backend 指向操作接口，backend->context 指向software_backend.c 管理软件规则的结构
         * item 原样传入，&item->backend_token 让回调把资源凭据写回事务条目
         * 选择向网卡下发规则时，同一调用语句使用 rte_flow 接口中保存的函数地址
         */
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
     * 使用 prepare 返回的 token 安装规则，提交失败的条目也可能需要回滚
     * 软件规则每安装一条就发布一次快照，worker 可能先看到本批中的部分规则
     */
    for (i = 0; i < transaction->nb_items; ++i) {
        struct dppd_transaction_item *item = &transaction->items[i];
        const struct dppd_transaction_backend *backend = backend_for(backends, item->plan.backend);
        /** 调用所选函数正式安装规则，成功后再将条目标记为 COMMITTED */
        rc = backend->commit(backend->context, item, item->backend_token);
        if (rc != 0) {
            item->state = DPPD_TRANSACTION_ITEM_FAILED;
            item->error_code = rc;
            transaction->failure_code = rc;
            transaction->failure_stage = DPPD_RULE_STAGE_COMMIT;
            transaction->failure_item = i;
            /**
             * 提交失败也可能留下资源，当前条目必须纳入回滚
             * 单条示例 i = 0：commit_attempts = 1，回滚后仍返回原来的提交错误
             * 清理若也失败，另存到 transaction->rollback_code，交给控制层决定是否隔离
             */
            rollback_items(transaction, backends, i + 1U);
            return rc;
        }
        item->state = DPPD_TRANSACTION_ITEM_COMMITTED;
    }

    transaction->state = DPPD_TRANSACTION_COMMITTED;
    return 0;
}

/**
 * 撤销已提交的事务，例如新版本装好后，删除旧版本失败时撤回新版本
 * 只接受 COMMITTED 状态；返回 0 表示全部撤销成功，负值表示清理失败
 */
int dppd_transaction_rollback_committed(struct dppd_transaction *transaction,
    const struct dppd_transaction_backends *backends)
{
    if (transaction == NULL || backends == NULL || transaction->state != DPPD_TRANSACTION_COMMITTED)
        return -EINVAL;
    rollback_items(transaction, backends, transaction->nb_items);
    return transaction->rollback_code;
}

/**
 * 释放已提交事务的临时资源，保留已安装的规则
 *
 * 调用所选接口的 finalize → 清零各条目的 token → 状态改为 FINALIZED
 *
 * 控制层确认不再回滚后调用；返回 0 表示收尾成功，负值表示状态或规则安装接口异常
 */
int dppd_transaction_finalize(struct dppd_transaction *transaction, const struct dppd_transaction_backends *backends)
{
    uint32_t i;

    if (transaction == NULL || backends == NULL || transaction->state != DPPD_TRANSACTION_COMMITTED)
        return -EINVAL;
    for (i = 0; i < transaction->nb_items; ++i) {
        struct dppd_transaction_item *item = &transaction->items[i];
        const struct dppd_transaction_backend *backend = backend_for(backends, item->plan.backend);

        if (item->state != DPPD_TRANSACTION_ITEM_COMMITTED || !backend_valid(backend))
            return -EUCLEAN;
        /** finalize 回调只释放临时资源，不返回错误；未提供回调时也要清零 token */
        if (backend->finalize != NULL)
            backend->finalize(backend->context, item, item->backend_token);
        item->backend_token = 0;
    }
    transaction->state = DPPD_TRANSACTION_FINALIZED;
    return 0;
}
