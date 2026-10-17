#ifndef DPPD_IPV4_REWRITE_H
#define DPPD_IPV4_REWRITE_H

#include "dppd/ipv4_router.h"
#include "dppd/packet.h"
#include "dppd/conntrack.h"

/** 跟踪前验证 IPv4 与 TCP/UDP 校验和、长度、TTL 和选项，坏包不能建立或刷新会话 */
int dppd_ipv4_transport_validate(const struct rte_mbuf *mbuf, const struct dppd_packet *packet);
/** 在同一个独占副本中完成 NAT、MAC 和 TTL 改写，TCP/UDP 校验和包含伪首部地址变化 */
int dppd_ipv4_rewrite_flow(struct rte_mbuf **mbuf, const struct dppd_packet *packet,
    const struct dppd_ipv4_forward *forward, const uint8_t source_mac[6], const struct dppd_flow_tuple *translation);

/**
 * 先核对 IPv4 头和校验和，再改 MAC、TTL 和头校验和，不改 IP 地址或传输层内容
 * 共享、间接或外部缓冲区先复制，成功后可能替换 mbuf，调用方始终拥有返回的报文
 * 失败时原报文及引用保持不变，TTL 耗尽返回 ETIMEDOUT，坏校验和返回 EBADMSG
 * 初版只允许无选项或 NOP/EOL 填充选项，其他选项返回 ENOTSUP，避免误转源路由报文
 */
int dppd_ipv4_rewrite(struct rte_mbuf **mbuf, const struct dppd_packet *packet, const struct dppd_ipv4_forward *forward,
    const uint8_t source_mac[6]);

#endif
