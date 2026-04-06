#ifndef DPPD_STATS_H
#define DPPD_STATS_H

#include <stdint.h>

struct dppd_lcore_stats {
    uint64_t rx_pkts;
    uint64_t tx_pkts;
    uint64_t rx_drop;
    uint64_t tx_drop;
    uint64_t arp_pkts;
    uint64_t ipv4_pkts;
    uint64_t udp_pkts;
    uint64_t tcp_pkts;
};

void dppd_stats_reset(void);
void dppd_stats_account_parse(int is_arp, int is_ipv4, int is_udp, int is_tcp);
void dppd_stats_account_rx_drop(void);
void dppd_stats_account_fwd_tx(void);
void dppd_stats_account_fwd_drop(void);
void dppd_stats_tick(void);
void dppd_stats_dump(void);

#endif
