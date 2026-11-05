#ifndef DPPD_FLOW_CONNTRACK_IO_H
#define DPPD_FLOW_CONNTRACK_IO_H

#include "dppd/flow_state.h"

#define DPPD_FLOW_CONNTRACK_BYTES 4096U

/** 配置文件和幂等请求使用同一种编码，不依赖 C 结构体的填充和机器字节顺序 */
int dppd_flow_conntrack_encode(const struct dppd_conntrack_policy *policy, uint8_t *data,
    size_t capacity, size_t *length);
int dppd_flow_conntrack_decode(const uint8_t *data, size_t length, struct dppd_conntrack_policy *policy);
int dppd_flow_conntrack_validate(const struct dppd_flow_conntrack_state *conntrack,
    const struct dppd_flow_routing_state *routing);

#endif
