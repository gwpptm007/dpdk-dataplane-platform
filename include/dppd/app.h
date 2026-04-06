#ifndef DPPD_APP_H
#define DPPD_APP_H

#include <stdbool.h>
#include <stdint.h>

struct dppd_app_config {
    uint16_t port_id;
    uint16_t nb_rxq;
    uint16_t nb_txq;
    uint32_t mbuf_count;
    uint16_t mbuf_cache;
    uint16_t burst_size;
    uint32_t poll_loops;
    bool promiscuous;
    int stats_interval_s;
};

#endif
