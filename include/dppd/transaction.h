#ifndef DPPD_TRANSACTION_H
#define DPPD_TRANSACTION_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/planner.h"
#include "dppd/rule_observation.h"

/** 一整批规则的处理进度；COMMITTED 表示后端安装完成，FINALIZED 表示已放弃事务回滚 */
enum dppd_transaction_state {
    DPPD_TRANSACTION_NEW = 0,
    DPPD_TRANSACTION_VALIDATING,
    DPPD_TRANSACTION_PREPARING,
    DPPD_TRANSACTION_COMMITTING,
    DPPD_TRANSACTION_COMMITTED,
    /**
     * 临时资源已释放，不能再回滚此事务
     * 规则可能尚未保存，异常隔离时也可收尾，因此该状态不代表整个管理请求成功
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


/**
 * 事务后端接口：保存后端对象和各阶段函数的地址，供事务流程统一调用
 *
 * int (*operation)(int, int) → 函数指针，所指函数接收两个 int，返回 int
 * int *operation(int, int)   → 函数声明，函数返回 int 指针，括号的位置决定含义
 *
 * 声明 prepare → 规定函数的参数和返回类型
 * .prepare = transaction_prepare → 保存具体函数地址，此时不执行函数
 * backend->prepare(...) → 取出成员中的地址并调用，等价于 (*backend->prepare)(...)
 *
 * 这种由后端提供、由事务流程调用的函数称为回调，此处调用会等待函数返回
 * 成员名不会自动关联具体实现，软件和硬件后端分别初始化自己的函数指针
 */
struct dppd_transaction_backend {
    /** 保存要操作的后端对象地址，调用时原样传回，由回调转换成具体对象指针 */
    void *context;
    /** 只检查规则是否支持，不安装规则也不改变后端的资源占用 */
    int (*validate)(void *context, const struct dppd_transaction_item *item);
    /**
     * 准备可撤销资源，通过 token 交回资源凭据，失败时也要保留待清理资源的凭据
     * int (*prepare)(...)：prepare 是函数指针，所指函数接收下面三个参数并返回 int
     */
    int (*prepare)(void *context, const struct dppd_transaction_item *item, uintptr_t *token);
    /** 使用已经准备好的资源正式安装规则，返回失败也可能已经产生部分副作用 */
    int (*commit)(void *context, const struct dppd_transaction_item *item, uintptr_t token);
    /** 撤销资源和安装副作用，commit_was_attempted 指明是否曾尝试正式提交 */
    int (
        *rollback)(void *context, const struct dppd_transaction_item *item, uintptr_t token, bool commit_was_attempted);
    /**
     * 放弃回滚后释放临时资源，保留已安装规则；回调不返回错误
     * 没有额外临时资源的后端可不提供此回调
     */
    void (*finalize)(void *context, const struct dppd_transaction_item *item, uintptr_t token);
};

struct dppd_transaction_backends {
    /** 软件后端的对象地址和各阶段回调 */
    struct dppd_transaction_backend software;
    /** rte_flow 后端的对象地址和各阶段回调 */
    struct dppd_transaction_backend rte_flow;
};

struct dppd_transaction {
    /** 本批统一的事务编号，用于把多条规则的结果关联起来 */
    uint64_t id;
    /** 整个事务的进度，例如 NEW、COMMITTED 或 ROLLED_BACK */
    enum dppd_transaction_state state;
    /** 借用调用方的条目数组，不负责分配或释放这块内存 */
    struct dppd_transaction_item *items;
    /** 本次处理的条目数，单条安装时为 1 */
    uint32_t nb_items;
    /** 原操作失败原因，例如第二条规则创建失败 */
    int failure_code;
    /** 回滚中的第一个失败原因；非零表示不能宣称已经完整恢复到事务开始前 */
    int rollback_code;
    /** 回滚会改变条目状态，因此单独保留最初失败的阶段和下标 */
    enum dppd_rule_failure_stage failure_stage;
    uint32_t failure_item;
    /** 首个补偿失败条目的下标，无错误时为 UINT32_MAX */
    uint32_t rollback_item;
};

int dppd_transaction_init(struct dppd_transaction *transaction, uint64_t transaction_id,
    struct dppd_transaction_item *items, uint32_t nb_items);
int dppd_transaction_run(struct dppd_transaction *transaction, const struct dppd_transaction_backends *backends);
/**
 * 逆序撤销已经完整提交但尚未 finalize 的事务，例如撤回本次更新中新安装的版本
 * 返回零才表示全部回滚动作成功，非零时调用方需要进一步处理恢复隔离
 */
int dppd_transaction_rollback_committed(struct dppd_transaction *transaction,
    const struct dppd_transaction_backends *backends);
/**
 * 释放临时资源并结束事务，保留已安装规则，成功后不可再回滚
 * 通常在内存规则记录更新后调用，决定进入恢复隔离时也可用于收尾
 */
int dppd_transaction_finalize(struct dppd_transaction *transaction, const struct dppd_transaction_backends *backends);

#endif
