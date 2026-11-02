#ifndef DPPD_SHARED_COUNT_H
#define DPPD_SHARED_COUNT_H

#include <stdint.h>
#include "dppd/rule.h"

#define DPPD_SHARED_COUNT_MAX 64U

struct rte_flow_action_handle;

/** 多条规则可引用同一个共享计数器；这里只保存创建配置，重启后重新创建并从零计数 */
struct dppd_shared_count_spec {
    /** 共享计数器 ID，供规则的 SHARED_COUNT 动作引用 */
    uint64_t id;
    /** 对象版本，用于识别规则引用的是否仍是同一代对象 */
    uint64_t generation;
    /** 创建此计数器的 DPDK 端口编号 */
    uint16_t install_port_id;
    /** 计数器所属的规则域，如 ingress、egress 或 transfer */
    enum dppd_rule_domain domain;
};

/** 引用数包含已预留、已安装和回滚失败的规则，只要非零就不能删除共享对象 */
struct dppd_shared_count_info {
    struct dppd_shared_count_spec spec;
    uint32_t references;
};

/** 句柄只在当前进程和端口生命周期内有效，不能保存进快照或跨复位继续使用 */
struct dppd_shared_count_handle {
    struct dppd_shared_count_spec spec;
    struct rte_flow_action_handle *opaque;
};

/** 数组下标对应规则动作下标，只有共享动作的位置携带借用句柄，其余位置必须为空 */
struct dppd_flow_binding {
    struct rte_flow_action_handle *actions[DPPD_RULE_MAX_ACTIONS];
};

#endif
