#ifndef DPPD_IPV6_H
#define DPPD_IPV6_H

#include "dppd/packet.h"
#include "dppd/ipv4_router.h"

/** 本阶段只转发普通单播，链路本地地址仅允许作为带出口端口的静态下一跳 */
bool dppd_ipv6_unicast(const uint8_t address[16], bool allow_link_local);
/** 内部解析入口，调用前已经完成以太网及 VLAN 解析，不重新清零输出 */
int dppd_ipv6_parse(const uint8_t *data, uint32_t available, uint32_t length, struct dppd_packet *packet);
/** 先完整检查再取得独占报文，只修改 MAC 和 Hop Limit，保留 IPv6 地址、流标签和负载 */
int dppd_ipv6_rewrite(struct rte_mbuf **mbuf, const struct dppd_packet *packet, const struct dppd_ipv4_forward *forward,
    const uint8_t source_mac[6]);

#endif
