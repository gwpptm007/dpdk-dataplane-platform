#ifndef DPPD_FLOW_ARP_IO_H
#define DPPD_FLOW_ARP_IO_H

#include "dppd/flow_state.h"

#define DPPD_FLOW_ARP_BYTES (16U + DPPD_ARP_LOCAL_LIMIT * 36U)

/** 文件与幂等请求共用规范编码，只写有效地址和 VLAN，不保存 C 结构体填充字节 */
int dppd_flow_arp_encode(const struct dppd_arp_policy *policy, uint8_t *data, size_t capacity, size_t *length);
int dppd_flow_arp_decode(const uint8_t *data, size_t length, struct dppd_arp_policy *policy);
int dppd_flow_arp_validate(const struct dppd_flow_arp_state *arp, const struct dppd_flow_routing_state *routing);

#endif
