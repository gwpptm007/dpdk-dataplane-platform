#ifndef DPPD_STATS_VALUES_H
#define DPPD_STATS_VALUES_H

#include <stdint.h>

#define DPPD_STATS_ALL UINT16_MAX
#define DPPD_STATS_FIELDS(F) \
    F(rx_packets) F(rx_bytes) F(tx_packets) F(tx_bytes) \
    F(rx_malformed) F(rx_unsupported) F(policy_drops) F(tx_drops) \
    F(rule_drops) F(no_route_drops) F(egress_drops) \
    F(tx_linearize_drops) F(tx_queue_drops)

struct dppd_stats_values {
#define DPPD_STATS_VALUE(field) uint64_t field;
    DPPD_STATS_FIELDS(DPPD_STATS_VALUE)
#undef DPPD_STATS_VALUE
};

#endif
