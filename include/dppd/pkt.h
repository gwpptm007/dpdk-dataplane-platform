#ifndef DPPD_PKT_H
#define DPPD_PKT_H

#include <stdbool.h>
#include <stdint.h>

struct dppd_pkt_meta {
    bool is_arp;
    bool is_ipv4;
    bool is_udp;
    bool is_tcp;
    uint8_t l4_proto;
    uint16_t ether_type;
    uint32_t pkt_len;
    uint32_t rss_hash;
    uint16_t l2_len;
    uint16_t l3_len;
    uint16_t l4_len;
};

#endif
