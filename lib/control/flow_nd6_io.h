#ifndef DPPD_FLOW_ND6_IO_H
#define DPPD_FLOW_ND6_IO_H

#include "dppd/flow_state.h"

#define DPPD_FLOW_ND6_BYTES (16U + DPPD_ND6_LOCAL_LIMIT * 48U)

/** 文件与幂等请求共用规范编码，只写有效地址和 VLAN，不保存 C 结构体填充字节 */
int dppd_flow_nd6_encode(const struct dppd_nd6_policy *policy, uint8_t *data, size_t capacity, size_t *length);
int dppd_flow_nd6_decode(const uint8_t *data, size_t length, struct dppd_nd6_policy *policy);
int dppd_flow_nd6_validate(const struct dppd_flow_nd6_state *nd6, const struct dppd_flow_routing_state *routing);

#endif
