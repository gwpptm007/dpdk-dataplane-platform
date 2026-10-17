#ifndef DPPD_TRANSFER_H
#define DPPD_TRANSFER_H

#include <stddef.h>
#include "dppd/control.h"

#define DPPD_TRANSFER_MAX_EDGES 2U
#define DPPD_TRANSFER_MESSAGE_SIZE 160U

enum dppd_transfer_template {
    DPPD_TRANSFER_PAIR = 0,
    DPPD_TRANSFER_UPLINK_VF,
    DPPD_TRANSFER_UPLINK_SF,
    DPPD_TRANSFER_PF_VF,
    DPPD_TRANSFER_PF_SF,
    DPPD_TRANSFER_VF_VF,
    DPPD_TRANSFER_SF_SF,
    DPPD_TRANSFER_TEMPLATE_COUNT,
};

/** 名称由核心和轻量 CLI 共用，不要求客户端链接 DPDK */
static inline const char *dppd_transfer_template_name(unsigned int type)
{
    const char *const names[] = {"pair", "uplink-vf", "uplink-sf", "pf-vf", "pf-sf", "vf-vf", "sf-sf"};
    return type < DPPD_TRANSFER_TEMPLATE_COUNT ? names[type] : "unknown";
}

/** 只表达新建的一对端点，apply 必须携带 plan 返回的精确账本版本 */
struct dppd_transfer_spec {
    uint64_t first_rule_id, expected_repository_generation;
    uint32_t priority, age_timeout_seconds;
    uint16_t install_port_id, source_port_id, destination_port_id;
    uint8_t scenario, bidirectional, with_count;
    uint8_t reserved[7];
};

/** 图中的边表示明确入口到明确出口，不包含隐式 JUMP、泛匹配入口或软件回退 */
struct dppd_transfer_graph {
    uint64_t repository_generation;
    uint16_t switch_domain_id, count;
    struct dppd_rule rules[DPPD_TRANSFER_MAX_EDGES];
    struct dppd_execution_plan plans[DPPD_TRANSFER_MAX_EDGES];
};

/** 只构造并核对完整图，不访问 PMD、不安装、不修改输出中的旧结果 */
int dppd_transfer_graph_build(const struct dppd_planner_context *context, const struct dppd_transfer_spec *spec,
    struct dppd_transfer_graph *graph, char *message, size_t message_size);
/** plan 不访问驱动；apply 复用正式整批创建和失败补偿，硬件报文生效不承诺原子切换 */
int dppd_control_transfer(struct dppd_control_service *control, const struct dppd_transfer_spec *spec, bool apply,
    struct dppd_transfer_graph *graph, struct dppd_control_apply_result results[DPPD_TRANSFER_MAX_EDGES], char *message,
    size_t message_size);

#endif
