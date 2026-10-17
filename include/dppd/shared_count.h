#ifndef DPPD_SHARED_COUNT_H
#define DPPD_SHARED_COUNT_H

#include <stdint.h>
#include "dppd/rule.h"

#define DPPD_SHARED_COUNT_MAX 64U

struct rte_flow_action_handle;

/** 共享对象的持久身份不包含驱动指针，重启按此定义重新创建且计数从零开始 */
struct dppd_shared_count_spec {
    uint64_t id;
    uint64_t generation;
    uint16_t install_port_id;
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
