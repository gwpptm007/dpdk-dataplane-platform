#ifndef DPPD_PLANNER_H
#define DPPD_PLANNER_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/rule.h"
#include "dppd/topology.h"

enum dppd_plan_backend {
    DPPD_PLAN_BACKEND_SOFTWARE = 0,
    DPPD_PLAN_BACKEND_RTE_FLOW,
};

enum dppd_plan_reason {
    DPPD_PLAN_REASON_NONE = 0,
    DPPD_PLAN_REASON_SOFTWARE_REQUESTED,
    DPPD_PLAN_REASON_HARDWARE_SELECTED,
    DPPD_PLAN_REASON_HARDWARE_UNAVAILABLE,
    DPPD_PLAN_REASON_SOFTWARE_FALLBACK,
    DPPD_PLAN_REASON_SOFTWARE_UNAVAILABLE,
    DPPD_PLAN_REASON_NO_EQUIVALENT_FALLBACK,
    DPPD_PLAN_REASON_ENDPOINT_NOT_FOUND,
    DPPD_PLAN_REASON_TRANSFER_DOMAIN_MISSING,
    DPPD_PLAN_REASON_TRANSFER_DOMAIN_MISMATCH,
    DPPD_PLAN_REASON_INVALID_RULE,
};

/** 规划规则时需要的端口信息、运行模式和后端能力 */
struct dppd_planner_context {
    /** 可用端口及其关系，供规划时检查安装目标 */
    const struct dppd_topology *topology;
    /** 本次规则的安装端口 */
    uint16_t install_port_id;
    /** 是否允许选择硬件后端，具体能力还需驱动校验 */
    bool hardware_available;
    /** 软件是否支持该规则的匹配条件和动作 */
    bool software_equivalent;
    /** 三层模式只接受入口策略，拒绝绕过工作线程的 transfer 和其他执行域 */
    bool ipv4_routing;
    /** 是否启用连接跟踪 */
    bool connection_tracking;
    /** 是否启用 IPv6 邻居发现 */
    bool neighbor_discovery;
};

/** 规划输出：记录规则身份、执行后端和选择原因，供后续事务安装 */
struct dppd_execution_plan {
    /** 要安装的规则 ID */
    uint64_t rule_id;
    /** 本次候选规则的版本 */
    uint64_t rule_generation;
    /** 安装到哪个端口 */
    uint16_t install_port_id;
    /** 由软件执行，或交给 rte_flow 后端安装 */
    enum dppd_plan_backend backend;
    /** 选择或拒绝该方案的原因 */
    enum dppd_plan_reason reason;
    /** 选中硬件方案时，标记仍需驱动确认支持 */
    bool requires_validation;
    /** 是否从硬件优先转为软件执行，主动指定软件时为 false */
    bool fallback_used;
};

/** 根据运行条件和规则生成执行计划，成功返回 0，失败返回负错误码并尽可能填写拒绝原因 */
int dppd_plan_rule(const struct dppd_planner_context *context, const struct dppd_rule *rule,
    struct dppd_execution_plan *plan);
const char *dppd_plan_reason_name(enum dppd_plan_reason reason);

#endif
