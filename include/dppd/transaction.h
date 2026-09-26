#ifndef DPPD_TRANSACTION_H
#define DPPD_TRANSACTION_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/planner.h"

/** 一整批规则的处理进度；COMMITTED 表示后端安装完成，FINALIZED 表示已放弃事务回滚 */
enum dppd_transaction_state {
    DPPD_TRANSACTION_NEW = 0,
    DPPD_TRANSACTION_VALIDATING,
    DPPD_TRANSACTION_PREPARING,
    DPPD_TRANSACTION_COMMITTING,
    DPPD_TRANSACTION_COMMITTED,
    /**
     * 仅用于回滚的临时资源已释放，此后不能再调用 rollback_committed
     * 正常流程此时已发布规则账本，随后保存失败交给 dirty 状态处理
     * 异常流程也可能在决定隔离后收尾，因此不能仅凭 FINALIZED 判断整体业务成功
     */
    DPPD_TRANSACTION_FINALIZED,
    DPPD_TRANSACTION_ROLLING_BACK,
    DPPD_TRANSACTION_ROLLED_BACK,
    DPPD_TRANSACTION_FAILED,
};

/** 单条规则的阶段状态，回滚时据此判断该条是否占用过资源 */
enum dppd_transaction_item_state {
    DPPD_TRANSACTION_ITEM_PENDING = 0,
    DPPD_TRANSACTION_ITEM_VALIDATED,
    DPPD_TRANSACTION_ITEM_PREPARED,
    DPPD_TRANSACTION_ITEM_COMMITTED,
    DPPD_TRANSACTION_ITEM_ROLLED_BACK,
    DPPD_TRANSACTION_ITEM_FAILED,
};

struct dppd_transaction_item {
    /** 要安装的完整规则，版本号由控制层提前分配 */
    struct dppd_rule rule;
    /** 规划出的目标后端、安装端口及选择原因 */
    struct dppd_execution_plan plan;
    /** 当前条目是否只校验过、已准备资源或已提交，独立于整批状态 */
    enum dppd_transaction_item_state state;
    /** 后端交回的资源凭据，事务只保存和转交它，不解释其内部含义 */
    uintptr_t backend_token;
    /** 当前条目校验、准备或提交时的错误，便于定位哪条规则首先失败 */
    int error_code;
};

struct dppd_transaction_backend {
    /** 后端私有上下文，例如硬件对象仓库或软件规则表 */
    void *context;
    /** 只检查规则是否支持，不安装规则也不改变后端的资源占用 */
    int (*validate)(void *context, const struct dppd_transaction_item *item);
    /** 预留可撤销资源并返回 token；失败后仍需清理的资源也要留下可识别的 token */
    int (*prepare)(void *context, const struct dppd_transaction_item *item,
                   uintptr_t *token);
    /** 使用已经准备好的资源正式安装规则，返回失败也可能已经产生部分副作用 */
    int (*commit)(void *context, const struct dppd_transaction_item *item,
                  uintptr_t token);
    /** 撤销资源和安装副作用，commit_was_attempted 指明是否曾尝试正式提交 */
    int (*rollback)(void *context, const struct dppd_transaction_item *item,
                    uintptr_t token, bool commit_was_attempted);
    /**
     * 控制层确认不再需要回滚时，释放 prepare 创建的额外临时资源
     * 不删除实际规则，也不再引入可失败操作；没有额外资源的后端可以不提供此回调
     */
    void (*finalize)(void *context, const struct dppd_transaction_item *item,
                     uintptr_t token);
};

struct dppd_transaction_backends {
    struct dppd_transaction_backend software;
    struct dppd_transaction_backend rte_flow;
};

struct dppd_transaction {
    /** 本批统一的事务编号，用于把多条规则的结果关联起来 */
    uint64_t id;
    enum dppd_transaction_state state;
    /** 借用调用方的条目数组，不负责分配或释放这块内存 */
    struct dppd_transaction_item *items;
    uint32_t nb_items;
    /** 原操作失败原因，例如第二条规则创建失败 */
    int failure_code;
    /** 回滚中的第一个失败原因；非零表示不能宣称已经完整恢复到事务开始前 */
    int rollback_code;
};

int dppd_transaction_init(struct dppd_transaction *transaction,
                          uint64_t transaction_id,
                          struct dppd_transaction_item *items,
                          uint32_t nb_items);
int dppd_transaction_run(struct dppd_transaction *transaction,
                         const struct dppd_transaction_backends *backends);
/**
 * 逆序撤销已经完整提交但尚未 finalize 的事务，例如撤回本次更新中新安装的版本
 * 返回零才表示全部回滚动作成功，非零时调用方需要进一步处理恢复隔离
 */
int dppd_transaction_rollback_committed(
    struct dppd_transaction *transaction,
    const struct dppd_transaction_backends *backends);
/**
 * 释放后端临时资源并结束事务，通常在规则账本发布成功后调用
 * 若已经发生无法补偿的错误，调用方决定进入隔离后也可用它释放不再使用的临时资源
 * finalize 不等于删除已安装规则，而且调用后不能再尝试 rollback_committed
 */
int dppd_transaction_finalize(struct dppd_transaction *transaction,
                              const struct dppd_transaction_backends *backends);

#endif
