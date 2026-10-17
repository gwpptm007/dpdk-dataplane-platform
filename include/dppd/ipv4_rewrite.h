#ifndef DPPD_IPV4_REWRITE_H
#define DPPD_IPV4_REWRITE_H

#include "dppd/ipv4_router.h"
#include "dppd/packet.h"

/**
 * 先核对 IPv4 头和校验和，再改 MAC、TTL 和头校验和，不改 IP 地址或传输层内容
 * 共享、间接或外部缓冲区先复制，成功后可能替换 mbuf，调用方始终拥有返回的报文
 * 失败时原报文及引用保持不变，TTL 耗尽返回 ETIMEDOUT，坏校验和返回 EBADMSG
 * 初版只允许无选项或 NOP/EOL 填充选项，其他选项返回 ENOTSUP，避免误转源路由报文
 */
int dppd_ipv4_rewrite(struct rte_mbuf **mbuf, const struct dppd_packet *packet, const struct dppd_ipv4_forward *forward,
    const uint8_t source_mac[6]);

#endif
