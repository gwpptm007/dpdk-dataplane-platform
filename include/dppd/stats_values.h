#ifndef DPPD_STATS_VALUES_H
#define DPPD_STATS_VALUES_H

#include <stdint.h>

#define DPPD_STATS_ALL UINT16_MAX
#define DPPD_STATS_FIELDS(F)  \
    F(rx_packets)             \
    F(rx_bytes)               \
    F(tx_packets)             \
    F(tx_bytes)               \
    F(rx_malformed)           \
    F(rx_unsupported)         \
    F(policy_drops)           \
    F(tx_drops)               \
    F(rule_drops)             \
    F(no_route_drops)         \
    F(egress_drops)           \
    F(tx_linearize_drops)     \
    F(tx_queue_drops)         \
    F(no_neighbor_drops)      \
    F(ipv4_ttl_drops)         \
    F(ipv4_checksum_drops)    \
    F(ipv4_unsupported_drops) \
    F(ipv4_rewrite_drops)     \
    F(ct_invalid_drops)       \
    F(ct_full_drops)          \
    F(ct_unsupported_drops)   \
    F(ct_zone_drops)          \
    F(ct_checksum_drops)      \
    F(nat_pool_drops)         \
    F(nat_miss_drops)         \
    F(ipv6_hop_drops)         \
    F(ipv6_checksum_drops)    \
    F(ipv6_unsupported_drops) \
    F(ipv6_rewrite_drops)

struct dppd_stats_values {
#define DPPD_STATS_VALUE(field) uint64_t field;
    DPPD_STATS_FIELDS(DPPD_STATS_VALUE)
#undef DPPD_STATS_VALUE
};

#endif
