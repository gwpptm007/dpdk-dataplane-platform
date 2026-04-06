#include "parser.h"
#include <arpa/inet.h>
#include <string.h>

/*
 * parser.c 现在已经从“按长度猜协议”的占位实现，
 * 升级为真正按 Ethernet / ARP / IPv4 / UDP / TCP 头部做最小解析。
 * 目标不是一次做成完整协议栈，而是先把 Phase 1 fast path 里最重要的
 * 二层/三层/四层识别能力建立起来。
 */

#define DPPD_ETHER_TYPE_ARP  0x0806U
#define DPPD_ETHER_TYPE_IPV4 0x0800U
#define DPPD_IPPROTO_TCP     6U
#define DPPD_IPPROTO_UDP     17U

struct dppd_eth_hdr {
    uint8_t dst_addr[6];
    uint8_t src_addr[6];
    uint16_t ether_type;
} __attribute__((packed));

struct dppd_arp_hdr {
    uint16_t hrd;
    uint16_t pro;
    uint8_t hln;
    uint8_t pln;
    uint16_t op;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
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

struct dppd_tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t sent_seq;
    uint32_t recv_ack;
    uint8_t data_off_flags_hi;
    uint8_t flags_lo;
    uint16_t rx_win;
    uint16_t cksum;
    uint16_t tcp_urp;
} __attribute__((packed));

int dppd_parse_packet(void *pkt, uint32_t len, struct dppd_parse_result *res)
{
    struct dppd_eth_hdr *eth;
    uint16_t ether_type;

    if (pkt == NULL || res == NULL || len < sizeof(*eth))
        return -1;

    memset(res, 0, sizeof(*res));
    res->meta.pkt_len = len;
    res->l2 = pkt;
    res->meta.l2_len = sizeof(*eth);

    eth = (struct dppd_eth_hdr *)pkt;
    ether_type = ntohs(eth->ether_type);
    res->meta.ether_type = ether_type;

    if (ether_type == DPPD_ETHER_TYPE_ARP) {
        struct dppd_arp_hdr *arp;

        if (len < sizeof(*eth) + sizeof(*arp))
            return -1;

        arp = (struct dppd_arp_hdr *)((uint8_t *)pkt + sizeof(*eth));
        res->meta.is_arp = true;
        res->l3 = arp;
        res->arp_op = ntohs(arp->op);
        return 0;
    }

    if (ether_type != DPPD_ETHER_TYPE_IPV4)
        return -1;

    {
        struct dppd_ipv4_hdr *ip;
        uint8_t ihl;
        uint16_t ip_total_len;

        if (len < sizeof(*eth) + sizeof(*ip))
            return -1;

        ip = (struct dppd_ipv4_hdr *)((uint8_t *)pkt + sizeof(*eth));
        ihl = (uint8_t)((ip->version_ihl & 0x0fU) * 4U);
        if (ihl < sizeof(*ip))
            return -1;
        if (len < sizeof(*eth) + ihl)
            return -1;

        ip_total_len = ntohs(ip->total_length);
        if (ip_total_len < ihl)
            return -1;

        res->meta.is_ipv4 = true;
        res->meta.l3_len = ihl;
        res->meta.l4_proto = ip->next_proto_id;
        res->l3 = ip;
        res->ipv4_src_be = ip->src_addr;
        res->ipv4_dst_be = ip->dst_addr;

        if (ip->next_proto_id == DPPD_IPPROTO_UDP) {
            struct dppd_udp_hdr *udp;
            if (len < sizeof(*eth) + ihl + sizeof(*udp))
                return -1;
            udp = (struct dppd_udp_hdr *)((uint8_t *)ip + ihl);
            res->meta.is_udp = true;
            res->meta.l4_len = sizeof(*udp);
            res->l4 = udp;
            res->l4_src_port_be = udp->src_port;
            res->l4_dst_port_be = udp->dst_port;
            return 0;
        }

        if (ip->next_proto_id == DPPD_IPPROTO_TCP) {
            struct dppd_tcp_hdr *tcp;
            uint8_t tcp_len;
            if (len < sizeof(*eth) + ihl + sizeof(*tcp))
                return -1;
            tcp = (struct dppd_tcp_hdr *)((uint8_t *)ip + ihl);
            tcp_len = (uint8_t)(((tcp->data_off_flags_hi >> 4) & 0x0fU) * 4U);
            if (tcp_len < sizeof(*tcp))
                return -1;
            if (len < sizeof(*eth) + ihl + tcp_len)
                return -1;
            res->meta.is_tcp = true;
            res->meta.l4_len = tcp_len;
            res->l4 = tcp;
            res->l4_src_port_be = tcp->src_port;
            res->l4_dst_port_be = tcp->dst_port;
            return 0;
        }
    }

    return 0;
}
