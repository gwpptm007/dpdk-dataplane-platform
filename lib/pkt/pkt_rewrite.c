#include "pkt_rewrite.h"
#include "parser.h"
#include "nat_session.h"
#include "csum.h"
#include <arpa/inet.h>
#include <stdint.h>

/*
 * pkt_rewrite.c 负责对已经判决为“允许发送”的 UDP/IPv4 报文做最小改写。
 * 当前版本先把 NAT 场景中最常用的源端口改写和 checksum 重算打通。
 */

struct dppd_eth_hdr {
    uint8_t dst_addr[6];
    uint8_t src_addr[6];
    uint16_t ether_type;
} __attribute__((packed));

struct dppd_ipv4_hdr {
    uint8_t version_ihl;
    uint8_t type_of_service;
    uint16_t total_length;
    uint16_t packet_id;
    uint16_t fragment_offset;
    uint8_t time_to_live;
    uint8_t next_proto_id;
    uint16_t hdr_checksum;
    uint32_t src_addr;
    uint32_t dst_addr;
} __attribute__((packed));

struct dppd_udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t dgram_len;
    uint16_t dgram_cksum;
} __attribute__((packed));

static uint16_t dppd_udp_checksum(const struct dppd_ipv4_hdr *ip,
                                  const struct dppd_udp_hdr *udp,
                                  const uint8_t *payload,
                                  uint16_t payload_len)
{
    uint32_t sum = 0;
    uint16_t udp_len = ntohs(udp->dgram_len);
    uint16_t i;
    const uint8_t *udp_bytes = (const uint8_t *)udp;

    sum += (ntohl(ip->src_addr) >> 16) & 0xffffU;
    sum += ntohl(ip->src_addr) & 0xffffU;
    sum += (ntohl(ip->dst_addr) >> 16) & 0xffffU;
    sum += ntohl(ip->dst_addr) & 0xffffU;
    sum += ip->next_proto_id;
    sum += udp_len;

    for (i = 0; i + 1 < udp_len; i += 2)
        sum += ((uint32_t)udp_bytes[i] << 8) | udp_bytes[i + 1];

    if (udp_len & 1U)
        sum += ((uint32_t)udp_bytes[udp_len - 1] << 8);

    (void)payload;
    (void)payload_len;

    while (sum >> 16)
        sum = (sum & 0xffffU) + (sum >> 16);

    return (uint16_t)(~sum & 0xffffU);
}

int dppd_rewrite_udp_ipv4(void *pkt,
                          unsigned int len,
                          const struct dppd_parse_result *res,
                          const struct dppd_nat_key *nat)
{
    struct dppd_ipv4_hdr *ip;
    struct dppd_udp_hdr *udp;
    uint16_t udp_len;

    if (pkt == 0 || res == 0 || nat == 0)
        return -1;
    if (!res->meta.is_ipv4 || !res->meta.is_udp)
        return 0;
    if (len < (unsigned int)(res->meta.l2_len + res->meta.l3_len + res->meta.l4_len))
        return -1;

    ip = (struct dppd_ipv4_hdr *)res->l3;
    udp = (struct dppd_udp_hdr *)res->l4;
    udp->src_port = htons(nat->src_port);
    ip->hdr_checksum = 0;
    ip->hdr_checksum = dppd_ipv4_csum16(ip, res->meta.l3_len);

    udp_len = ntohs(udp->dgram_len);
    if (udp_len < sizeof(*udp) ||
        (unsigned int)(res->meta.l2_len + res->meta.l3_len + udp_len) > len)
        return -1;

    udp->dgram_cksum = 0;
    udp->dgram_cksum = dppd_udp_checksum(ip,
                                         udp,
                                         (const uint8_t *)udp + sizeof(*udp),
                                         (uint16_t)(udp_len - sizeof(*udp)));
    return 0;
}
