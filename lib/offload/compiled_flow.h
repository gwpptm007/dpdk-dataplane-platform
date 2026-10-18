#ifndef DPPD_COMPILED_FLOW_H
#define DPPD_COMPILED_FLOW_H

#include "dppd/offload.h"
#include <rte_flow.h>

/**
 * 匹配项和动作中的指针都指向此结构体里的配置，避免引用编译函数已经失效的局部变量
 * 整个结构体必须保留到驱动的 validate 或 create 调用返回
 */
struct dppd_compiled_flow {
    struct rte_flow_attr attr;
    struct rte_flow_item pattern[DPPD_RULE_MAX_ITEMS + 1U];
    struct rte_flow_item_ipv4 ipv4_spec[DPPD_RULE_MAX_ITEMS];
    struct rte_flow_item_ipv4 ipv4_mask[DPPD_RULE_MAX_ITEMS];
    struct rte_flow_item_udp udp_spec[DPPD_RULE_MAX_ITEMS];
    struct rte_flow_item_udp udp_mask[DPPD_RULE_MAX_ITEMS];
    struct rte_flow_item_tcp tcp_spec[DPPD_RULE_MAX_ITEMS];
    struct rte_flow_item_tcp tcp_mask[DPPD_RULE_MAX_ITEMS];
    struct rte_flow_item_ethdev ethdev_spec[DPPD_RULE_MAX_ITEMS];
    struct rte_flow_item_ethdev ethdev_mask[DPPD_RULE_MAX_ITEMS];
    struct rte_flow_action actions[DPPD_RULE_MAX_ACTIONS + 2U];
    struct dppd_tap_owner_action owner;
    struct rte_flow_action_queue queue[DPPD_RULE_MAX_ACTIONS];
    struct rte_flow_action_mark mark[DPPD_RULE_MAX_ACTIONS];
    struct rte_flow_action_count count[DPPD_RULE_MAX_ACTIONS];
    struct rte_flow_action_age age[DPPD_RULE_MAX_ACTIONS];
    struct rte_flow_action_ethdev ethdev_action[DPPD_RULE_MAX_ACTIONS];
};

/** 同步与异步适配共用编译器，输出位置必须固定，不能浅拷贝内部自引用指针 */
int dppd_compile_flow(const struct dppd_rule *rule, const struct dppd_flow_binding *binding,
    struct dppd_compiled_flow *compiled);

#endif
