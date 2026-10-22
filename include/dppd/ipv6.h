#ifndef DPPD_IPV6_H
#define DPPD_IPV6_H

#include "dppd/packet.h"
#include "dppd/ipv4_router.h"
#include "dppd/conntrack.h"

/** ICMPv6 邻居发现消息类型，编号由 RFC 4861 定义，对应 ICMPv6 头的 Type 字段 */
enum dppd_icmpv6_type {
    /** RS：主机请求路由器发送通告，以获取本链路的路由器和网络配置信息 */
    DPPD_ICMPV6_ROUTER_SOLICITATION = 133,
    /** RA：路由器通告自身及前缀、有效期等信息，供主机发现路由器和配置网络 */
    DPPD_ICMPV6_ROUTER_ADVERTISEMENT = 134,
    /** NS：查询邻居的链路层地址、探测可达性，也用于本机重复地址检测 */
    DPPD_ICMPV6_NEIGHBOR_SOLICITATION = 135,
    /** NA：通告邻居的链路层地址等信息，可响应 NS 或主动通知地址变化 */
    DPPD_ICMPV6_NEIGHBOR_ADVERTISEMENT = 136,
    /** Redirect：路由器告知主机，到某个目的地址应使用本链路上更合适的下一跳 */
    DPPD_ICMPV6_REDIRECT = 137
};

/** 判断 Type 是否属于 RFC 4861 的五种邻居发现消息，仅分类，不校验消息内容 */
static inline bool dppd_icmpv6_is_nd(uint8_t type)
{
    return type >= DPPD_ICMPV6_ROUTER_SOLICITATION && type <= DPPD_ICMPV6_REDIRECT;
}

/** 本阶段只转发普通单播，链路本地地址仅允许作为带出口端口的静态下一跳 */
bool dppd_ipv6_unicast(const uint8_t address[16], bool allow_link_local);
/** 建立会话前检查真实报文和完整传输层校验和，失败报文不能刷新状态 */
int dppd_ipv6_transport_validate(const struct rte_mbuf *mbuf, const struct dppd_packet *packet);
/** 内部解析入口，调用前已经完成以太网及 VLAN 解析，不重新清零输出 */
int dppd_ipv6_parse(const uint8_t *data, uint32_t available, uint32_t length, struct dppd_packet *packet);
/** 先完整检查再取得独占报文，只修改 MAC 和 Hop Limit，保留 IPv6 地址、流标签和负载 */
int dppd_ipv6_rewrite(struct rte_mbuf **mbuf, const struct dppd_packet *packet, const struct dppd_ipv4_forward *forward,
    const uint8_t source_mac[6]);
/** 转换时同步更新完整 IPv6 地址、端口和伪首部校验和，普通路由传入空转换 */
int dppd_ipv6_rewrite_flow(struct rte_mbuf **mbuf, const struct dppd_packet *packet,
    const struct dppd_ipv4_forward *forward, const uint8_t source_mac[6], const struct dppd_flow_tuple *translation);

#endif
