#ifndef DPPD_PARSER_H
#define DPPD_PARSER_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/pkt.h"

struct dppd_parse_result {
    struct dppd_pkt_meta meta;
    void *l2;
    void *l3;
    void *l4;
    uint16_t arp_op;
    uint32_t ipv4_src_be;
    uint32_t ipv4_dst_be;
    uint16_t l4_src_port_be;
    uint16_t l4_dst_port_be;
};

int dppd_parse_packet(void *pkt, uint32_t len, struct dppd_parse_result *res);

#endif
