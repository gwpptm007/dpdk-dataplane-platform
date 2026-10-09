#ifndef DPPD_RTE_FLOW_BACKEND_H
#define DPPD_RTE_FLOW_BACKEND_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/install_info.h"
#include "dppd/rule_latency.h"
#include "dppd/capability.h"
#include "dppd/offload.h"
#include "dppd/transaction.h"

/**
 * 控制层调用硬件规则能力的接口集合，默认接入真实 rte_flow，单测可替换为故障注入接口
 * validate 只检查支持情况，create 成功后返回的 handle 必须一直保留到 remove 成功
 */
struct dppd_flow_api {
    int (*validate)(uint16_t port_id, const struct dppd_rule *rule,
                    struct dppd_flow_error *error);
    int (*create)(uint16_t port_id, const struct dppd_rule *rule,
                  struct dppd_flow_handle *handle,
                  struct dppd_flow_error *error);
    int (*remove)(struct dppd_flow_handle *handle,
                  struct dppd_flow_error *error);
    /**
     * 计数查询是可选能力，只测试规则创建和删除时可以不提供此回调
     * 真正查询时若接口缺失，返回 ENOTSUP，不能伪造零命中来表示不支持
     */
    int (*query_count)(const struct dppd_flow_handle *handle,
                       uint64_t *hits,
                       uint64_t *bytes,
                       struct dppd_flow_error *error);
};

struct dppd_rte_flow_object;

struct dppd_rte_flow_backend {
    /** 固定地址的对象数组，事务 token 会直接引用其中的槽位 */
    struct dppd_rte_flow_object *objects;
    /** 本地仓库槽位上限，不代表网卡还能安装多少条规则 */
    uint32_t capacity;
    /** 已占用槽位数，包含准备阶段的预留项和已安装对象 */
    uint32_t count;
    /** 操作入口的副本，普通运行使用真实驱动调用 */
    struct dppd_flow_api api;
    /** 本进程的端口校验统计和短期诊断缓存，不拥有任何网卡规则 */
    struct dppd_flow_probe_cache *probes;
    /** 只由控制面记录成功 create 的历史，后续删除和回滚不抹掉已经发生的安装 */
    struct dppd_rule_latency_histogram latency;
    /**
     * 可选的安装前保护，只在控制面调用，成功后才允许进入驱动 create
     * daemon 用它先持久化可能残留的端口，context 必须存活到 backend 清理完成
     */
    int (*before_create)(void *context, uint16_t port_id, const struct dppd_rule *rule, uint64_t *attempt);
    void *before_create_context;
    /** 创建结果记录失败时向事务返回错误，真实 handle 仍保留供回滚删除 */
    int (*after_create)(void *context, uint64_t attempt, int create_error);
    /** 删除通知不替代驱动结果，记录故障由保护层锁存并阻止后续安装和自动清理确认 */
    void (*after_remove)(void *context, uint64_t attempt, int remove_error);
};

/**
 * 初始化同时保存预留项和已安装对象的本地仓库
 * 对象由 ID 和版本共同标识，同一个 ID 的新旧版本可短暂共存，完全相同的版本不能重复
 * api 为空时使用真实 flow 实现，非空时调用方必须提供校验、创建和删除三个基础接口
 */
int dppd_rte_flow_backend_init(struct dppd_rte_flow_backend *backend,
                               uint32_t capacity,
                               const struct dppd_flow_api *api);
/** 删除已安装或失败后仍有 handle 的对象，删除失败时保留剩余清理线索 */
int dppd_rte_flow_backend_fini(struct dppd_rte_flow_backend *backend);
/** 正式安装始终重新校验，不用探测缓存跳过驱动，也不把探测成功当成安装成功 */
int dppd_rte_flow_backend_validate_rule(struct dppd_rte_flow_backend *backend,
    uint16_t port_id, const struct dppd_rule *rule, struct dppd_flow_error *error);
struct dppd_transaction_backend dppd_rte_flow_transaction_backend(
    struct dppd_rte_flow_backend *backend);
uint32_t dppd_rte_flow_backend_count(const struct dppd_rte_flow_backend *backend);
const struct dppd_flow_handle *dppd_rte_flow_backend_find(
    const struct dppd_rte_flow_backend *backend, uint64_t rule_id);
const struct dppd_flow_handle *dppd_rte_flow_backend_find_version(
    const struct dppd_rte_flow_backend *backend,
    uint64_t rule_id,
    uint64_t generation);
/**
 * 读取精确版本的安装记录，不调用网卡接口，不把 prepare 预留项当作已安装对象
 * 本地 handle 的 ID、版本、端口或 COUNT 配置与安装记录失配时返回 EUCLEAN
 */
int dppd_rte_flow_backend_install_info(
    const struct dppd_rte_flow_backend *backend, uint64_t rule_id,
    uint64_t generation, struct dppd_rule_install_info *info);
int dppd_rte_flow_backend_remove(struct dppd_rte_flow_backend *backend,
                                 uint64_t rule_id,
                                 struct dppd_flow_error *error);
int dppd_rte_flow_backend_remove_version(struct dppd_rte_flow_backend *backend,
                                         uint64_t rule_id,
                                         uint64_t generation,
                                         struct dppd_flow_error *error);
/**
 * 恢复隔离期间再次尝试删除本仓库的所有对象，成功时 residual_objects 为零
 * 可能同时清理旧规则和失败事务留下的新规则，因此不能在正常提供服务时调用
 * 函数不改写规则账本，清理完成后由控制层要求退出重启，再从快照恢复业务规则
 */
int dppd_rte_flow_backend_reconcile(struct dppd_rte_flow_backend *backend,
                                    uint32_t *residual_objects);
/**
 * 查询指定已安装版本的 COUNT 计数，调用方负责选择账本当前认可的版本
 * ENOENT 表示没有对应对象，ENODATA 表示规则没有计数动作，ENOTSUP 表示接口不支持查询
 * 这些状态与“计数为零”含义不同，客户端应保留区别
 */
int dppd_rte_flow_backend_query_count(
    const struct dppd_rte_flow_backend *backend,
    uint64_t rule_id,
    uint64_t generation,
    uint64_t *hits,
    uint64_t *bytes,
    struct dppd_flow_error *error);

#endif
