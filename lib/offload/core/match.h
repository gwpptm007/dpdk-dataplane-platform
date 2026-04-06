#ifndef DPPD_MATCH_H
#define DPPD_MATCH_H
#include <stdint.h>

enum dppd_match_type {
    DPPD_MATCH_ETH = 0,
    DPPD_MATCH_IPV4_5TUPLE,
    DPPD_MATCH_IPV4_SRC,
    DPPD_MATCH_IPV4_DST,
    DPPD_MATCH_L4_PORT,
    DPPD_MATCH_VLAN,
};

struct dppd_match {
    enum dppd_match_type type;
    uint32_t value32;
    uint32_t mask32;
};

#endif
