#ifndef DPPD_NAT_SESSION_H
#define DPPD_NAT_SESSION_H
#include <stdint.h>
struct dppd_nat_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t proto;
};
int dppd_nat_translate(struct dppd_nat_key *key);
void dppd_nat_aging_run(uint64_t now_tsc);
#endif
