#ifndef DPPD_FLOW_ROUTING_IO_H
#define DPPD_FLOW_ROUTING_IO_H

#include "dppd/flow_state.h"

#define DPPD_FLOW_ROUTING_BYTES 24616U
#define DPPD_FLOW_BINDING_BYTES 216U

/** 磁盘和请求比较共用显式编码，地址保留网络字节顺序，其他整数使用小端 */
int dppd_flow_routing_encode(const struct dppd_ipv4_configuration *configuration, uint8_t *data,
    size_t capacity, size_t *length);
int dppd_flow_routing_decode(const uint8_t *data, size_t length, struct dppd_ipv4_configuration *configuration);
int dppd_flow_routing_validate(const struct dppd_flow_routing_state *routing);

#endif
