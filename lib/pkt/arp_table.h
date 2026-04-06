#ifndef DPPD_ARP_TABLE_H
#define DPPD_ARP_TABLE_H
#include <stdint.h>
struct dppd_arp_entry {
    uint32_t ipv4_be;
    uint8_t mac[6];
    uint64_t last_seen_tsc;
};
int dppd_arp_table_upsert(uint32_t ipv4_be, const uint8_t mac[6]);
#endif
