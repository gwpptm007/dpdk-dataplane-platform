#ifndef DPPD_CONTROL_H
#define DPPD_CONTROL_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/planner.h"
#include "dppd/rte_flow_backend.h"
#include "dppd/rule_repository.h"
#include "dppd/software_backend.h"

enum dppd_control_recovery_state {
    /* 正常运行：desired repository 与 backend 的控制面不变量成立。 */
    DPPD_CONTROL_RECOVERY_READY = 0,
    /* 启动重放的回滚失败，backend 可能仍留有本进程可定位的 flow handle。 */
    DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED,
    /* 残留对象已清除；必须重启后从 snapshot 重新建立完整 desired state。 */
    DPPD_CONTROL_RECOVERY_RESTART_REQUIRED,
};

struct dppd_control_service {
    /*
     * topology 在运行期由 runtime 持有且不可变；control 只借用该指针来规划规则，
     * 因此 runtime 的销毁必须晚于 control_fini。
     */
    const struct dppd_topology *topology;
    /* desired state 的唯一真源；只有 backend actual state 已提交后才允许写入。 */
    struct dppd_rule_repository rules;
    /* 硬件 rte_flow 与等价软件 classifier 分别保存 actual state，同一版本只能存在其一。 */
    struct dppd_rte_flow_backend rte_flow;
    struct dppd_software_backend software;
    uint64_t next_transaction_id;
    /* persistence path 由 service 动态持有；NULL 表示未启用自动 snapshot。 */
    char *persistence_path;
    uint64_t persisted_generation;
    int persistence_last_error;
    bool persistence_dirty;
    /* 仅在启动恢复回滚失败时置位；普通运行路径不能自行清除此状态。 */
    enum dppd_control_recovery_state recovery_state;
    int recovery_last_error;
};

struct dppd_control_apply_result {
    /* CREATED/UPDATED/UNCHANGED：后者没有创建 backend 事务，transaction_id 为 0。 */
    enum dppd_rule_apply_status status;
    uint64_t generation;
    uint64_t transaction_id;
    struct dppd_execution_plan plan;
};

/*
 * 批量创建的单条输入。首版刻意只接受 expected_generation=0 的新 rule：这样可以
 * 在进入 backend transaction 前完整验证容量、ID 唯一性和计划，保证同批没有部分
 * desired-state 发布。跨 rule 更新/删除会在后续独立设计 replacement 依赖图后加入。
 */
struct dppd_control_batch_create_request {
    uint16_t install_port_id;
    uint16_t reserved;
    uint64_t expected_generation;
    struct dppd_rule rule;
};

/**
 * 批量删除的单条条件。首版要求精确 generation，不接受 0 或 ANY：删除是不可逆语义，
 * 因而不能把“对象已不存在”或“已被他人更新”静默当作成功。
 */
struct dppd_control_batch_remove_request {
    uint64_t rule_id;
    uint64_t expected_generation;
};

/** 每条删除结果按请求顺序返回；generation 是删除后 repository 的全局修订号。 */
struct dppd_control_batch_remove_result {
    uint64_t rule_id;
    uint64_t generation;
};

/**
 * 批量更新的单条输入。rule.id 是待替换对象的稳定 ID，expected_generation 必须等于
 * repository 当前旧版本；install_port_id 与单规则 apply 一样覆盖 rule 内携带的值。
 * 新 rule 不填写 generation，control 在整批预检后按输入顺序分配连续新版本。
 */
struct dppd_control_batch_update_request {
    uint16_t install_port_id;
    uint16_t reserved;
    uint64_t expected_generation;
    struct dppd_rule rule;
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

struct dppd_control_recovery_status {
    enum dppd_control_recovery_state state;
    uint32_t residual_objects;
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
void dppd_control_recovery_status(
    const struct dppd_control_service *service,
    struct dppd_control_recovery_status *status);
/*
 * 只允许恢复隔离模式调用。成功后进入 RESTART_REQUIRED，不恢复 worker 或发布空
 * repository；必须由主程序退出并重新走完整 snapshot 恢复。
 */
int dppd_control_reconciliation_retry(struct dppd_control_service *service);
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
/*
 * 原子创建一批此前不存在的规则。全部 backend validate/prepare/commit 成功且
 * repository 一次连续发布后才返回成功；任一失败会逆序回滚已创建 actual 对象。
 * results 必须指向 request_count 个元素，输出与输入一一对应的 generation/plan。
 * 成功后每个结果共享一个非零 transaction_id；失败时调用方只能查看返回 errno，
 * 不得把 results 当成部分提交回执。若回滚本身失败，接口返回 -EUCLEAN 并封锁后续写入。
 */
int dppd_control_create_batch(
    struct dppd_control_service *service,
    const struct dppd_control_batch_create_request *requests,
    uint32_t request_count,
    struct dppd_control_apply_result *results);
/**
 * 原子删除一批当前存在且 generation 精确匹配的规则。先验证全批并从 actual backend
 * 删除；只有全部删除成功才连续移除 desired records。actual 删除中途失败会尝试重建已删
 * 对象；重建或 repository 补偿无法保证一致时返回 -EUCLEAN 并进入 recovery 隔离。
 */
int dppd_control_remove_batch(
    struct dppd_control_service *service,
    const struct dppd_control_batch_remove_request *requests,
    uint32_t request_count,
    struct dppd_control_batch_remove_result *results);
/**
 * 原子替换一批已有规则。新 generation 全部创建成功后才删除全部旧 generation，最后才
 * 连续发布 desired repository；结果数组与输入一一对应并共享同一 transaction ID。
 * 若后端没有足够空间让新旧版本短暂共存，必须在触碰 backend 前返回 -ENOSPC。
 */
int dppd_control_update_batch(
    struct dppd_control_service *service,
    const struct dppd_control_batch_update_request *requests,
    uint32_t request_count,
    struct dppd_control_apply_result *results);
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
