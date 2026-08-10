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

struct dppd_planner_context {
    const struct dppd_topology *topology;
    uint16_t install_port_id;
    bool hardware_available;
    bool software_equivalent;
};

struct dppd_execution_plan {
    uint64_t rule_id;
    uint64_t rule_generation;
    uint16_t install_port_id;
    enum dppd_plan_backend backend;
    enum dppd_plan_reason reason;
    bool requires_validation;
    bool fallback_used;
};

/* planner 只作可解释决策；硬件最终能力仍以 backend validate 结果为准。 */
int dppd_plan_rule(const struct dppd_planner_context *context,
                   const struct dppd_rule *rule,
                   struct dppd_execution_plan *plan);
const char *dppd_plan_reason_name(enum dppd_plan_reason reason);

#endif
