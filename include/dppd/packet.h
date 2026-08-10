#ifndef DPPD_PACKET_H
#define DPPD_PACKET_H

#include <stdbool.h>
#include <stdint.h>

struct rte_mbuf;

enum dppd_parse_status {
    DPPD_PARSE_OK = 0,
    DPPD_PARSE_UNSUPPORTED = 1,
    DPPD_PARSE_MALFORMED = -1,
};

enum dppd_l3_type {
    DPPD_L3_NONE = 0,
    DPPD_L3_ARP,
    DPPD_L3_IPV4,
};

enum dppd_l4_type {
    DPPD_L4_NONE = 0,
    DPPD_L4_UDP,
    DPPD_L4_TCP,
    DPPD_L4_FRAGMENT,
    DPPD_L4_OTHER,
};

struct dppd_packet {
    uint32_t packet_len;
    uint16_t ether_type;
    uint16_t l2_len;
    uint16_t l3_len;
    uint16_t l4_len;
    uint16_t vlan_tci[2];
    uint8_t vlan_depth;
    enum dppd_l3_type l3_type;
    enum dppd_l4_type l4_type;
    uint8_t ip_protocol;
    uint32_t ipv4_src_be;
    uint32_t ipv4_dst_be;
    uint16_t l4_src_port_be;
    uint16_t l4_dst_port_be;
    bool ipv4_more_fragments;
};

int dppd_packet_parse_buffer(const uint8_t *data,
                             uint32_t available_len,
                             uint32_t packet_len,
                             struct dppd_packet *packet);
int dppd_packet_parse_mbuf(const struct rte_mbuf *mbuf, struct dppd_packet *packet);

#endif

