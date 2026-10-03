#ifndef DPPD_CONTROL_H
#define DPPD_CONTROL_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/planner.h"
#include "dppd/rte_flow_backend.h"
#include "dppd/rule_repository.h"
#include "dppd/software_backend.h"

/** 每批最多处理四条已有规则，限制临时资源需求，也让请求和结果保持固定的小规模 */
#define DPPD_CONTROL_BATCH_UPDATE_MAX 4U

enum dppd_control_recovery_state {
    /* 正常运行：desired repository 与 backend 的控制面不变量成立。 */
    DPPD_CONTROL_RECOVERY_READY = 0,
    /* 启动重放或在线事务补偿失败，backend 可能仍留有本进程可定位的 flow handle。 */
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
    /* 启动恢复或在线补偿失败时置位；普通 mutation 不能自行清除此状态。 */
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

/**
 * 批量创建中的一条请求，只表达新建，不接受覆盖已有规则
 * rule.id 必须尚不存在，expected_generation 必须为零，更新和删除使用另外的批量接口
 * 这样失败时只需撤销本批新建对象，不必同时承担恢复被覆盖旧规则的责任
 */
struct dppd_control_batch_create_request {
    /** 本条新规则安装的 DPDK 端口，覆盖 rule 内携带的安装端口 */
    uint16_t install_port_id;
    /** 协议扩展预留字段，调用方按协议约定填写零 */
    uint16_t reserved;
    /** 只能填写零，明确表示此 ID 之前不应存在 */
    uint64_t expected_generation;
    /** 完整的新规则内容，generation 由控制层按批次顺序分配 */
    struct dppd_rule rule;
};

/**
 * 批量删除中的一条条件，必须同时指定稳定 ID 和最近读取到的精确版本
 * 不接受零或 ANY，避免把对象已不存在或已被其他请求更新的情况当作删除成功
 * 删除失败的补偿会尝试重建原规则，但不承诺恢复被删除对象的历史计数
 */
struct dppd_control_batch_remove_request {
    /** 要删除的已有规则 ID，同一批次不能重复 */
    uint64_t rule_id;
    /** 要删除的精确旧版本，必须与当前账本中的版本相同 */
    uint64_t expected_generation;
};

/**
 * 删除结果按请求顺序返回，rule_id 用来对应原请求
 * generation 是执行本次账本删除后的全局修订号，不是已经被删除规则的旧版本
 * 例如全局版本为六时依次删除两条，结果中的 generation 分别为七和八
 */
struct dppd_control_batch_remove_result {
    uint64_t rule_id;
    uint64_t generation;
};

/**
 * 批量更新中的一条请求，可理解为“把这个 ID 对应的旧版本替换成下面的完整规则”
 * expected_generation 必须与仓库当前版本相同，用来证明用户没有基于过期信息修改规则
 * 新 generation 由控制层按整批输入顺序分配，客户端携带的 generation 不会直接发布
 */
struct dppd_control_batch_update_request {
    /** 安装目标是 DPDK 的端口编号，会覆盖 rule 内部的安装端口字段 */
    uint16_t install_port_id;
    /** 保留字段当前必须填零，避免新旧协议对额外含义产生不同解释 */
    uint16_t reserved;
    /** 用户上次看到的精确旧版本，只接受非零数值，不接受 ANY */
    uint64_t expected_generation;
    /** 完整的新规则内容，rule.id 必须保持为要替换的已有规则 ID */
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

/**
 * 一条当前已发布规则的安装状态，实际后端来自对象仓库，fallback 仍表示用户的选择策略
 * persistence 描述整个规则账本的保存状态，不代表某一条规则独立保存了一个文件
 */
struct dppd_control_rule_status {
    struct dppd_rule_install_info installation;
    enum dppd_plan_backend backend;
    enum dppd_fallback_policy fallback;
    struct dppd_control_persistence_status persistence;
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
/**
 * 创建一批原先不存在的规则，先完成全部实际对象安装，再连续发布规则账本并保存快照
 * 安装失败会逆序尝试撤销本批资源，撤销失败返回 EUCLEAN 并进入恢复隔离
 * results 至少容纳 request_count 个元素，成功时结果顺序与请求一致，且共享非零事务编号
 * 返回错误时不能把数组中已经填写的前几项当作部分成功回执
 * 还要区分保存失败：此时本批可能已生效，需要查看 dirty 状态而不能认定已经回滚
 */
int dppd_control_create_batch(
    struct dppd_control_service *service,
    const struct dppd_control_batch_create_request *requests,
    uint32_t request_count,
    struct dppd_control_apply_result *results);
/**
 * 删除一批旧版本精确匹配的已有规则，先验证全批，再删除实际对象，最后移除账本记录
 * 实际删除中途失败时按原版本重建之前已删的对象，账本仍保留完整旧记录
 * 无法完成补偿或账本状态失配时进入恢复隔离，结果数组仅在整个请求成功时有效
 * 全部删除生效后仍可能遇到快照保存失败，此时通过 dirty 状态处理，不能假定旧对象还在
 */
int dppd_control_remove_batch(
    struct dppd_control_service *service,
    const struct dppd_control_batch_remove_request *requests,
    uint32_t request_count,
    struct dppd_control_batch_remove_result *results);
/**
 * 原子替换两到四条已有规则，每条必须提供精确旧版本且不能重复 ID
 *
 * 顺序是：检查全批请求、安装全部新版本、删除全部旧版本、整批更新规则账本
 * 旧对象和新计划全为软件时一次发布整批快照，复用槽位，不需要新旧版本共存空间
 * 其他路径需要临时空间；可能整表替换的 PREFER 在驱动校验后若变成混合路径还需复查
 *
 * results 至少容纳 request_count 个元素，成功时与输入顺序相同且共享一个事务编号
 * 即使规则内容没有变化，也会分配连续新版本，重放旧请求会返回 ESTALE
 * 普通失败且补偿成功时保留旧版本，补偿失败会返回 EUCLEAN 并进入恢复隔离
 * 若只有保存快照失败，整批可能已经生效，需要查看持久化状态，不能直接当作已回滚
 *
 * 纯软件路径的每个报文使用完整旧表或新表，不保证多个报文或线程同时切换
 * 硬件及混合路径只保证规则账本整批发布；新软件版本 COUNT 从零开始
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

/**
 * 查询当前账本版本的安装状态，expected_generation 为精确非零版本或 ANY
 * 同一版本必须恰好安装在一个后端中，身份、端口或 COUNT 配置失配时返回 EUCLEAN
 * 查询只读内存，不调用驱动、不查询计数、不保存磁盘，恢复隔离期间仍拒绝普通规则查询
 */
int dppd_control_rule_status(const struct dppd_control_service *service,
                              uint64_t rule_id, uint64_t expected_generation,
                              struct dppd_control_rule_status *result);

#endif
