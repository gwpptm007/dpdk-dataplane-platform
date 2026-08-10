#ifndef DPPD_STATS_H
#define DPPD_STATS_H

#include <stdatomic.h>
#include <stdint.h>
#include <rte_common.h>

struct dppd_stats_values {
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t tx_packets;
    uint64_t tx_bytes;
    uint64_t rx_malformed;
    uint64_t rx_unsupported;
    uint64_t policy_drops;
    uint64_t tx_drops;
};

struct dppd_worker_stats {
    atomic_uint_fast64_t rx_packets;
    atomic_uint_fast64_t rx_bytes;
    atomic_uint_fast64_t tx_packets;
    atomic_uint_fast64_t tx_bytes;
    atomic_uint_fast64_t rx_malformed;
    atomic_uint_fast64_t rx_unsupported;
    atomic_uint_fast64_t policy_drops;
    atomic_uint_fast64_t tx_drops;
} __rte_cache_aligned;

void dppd_stats_init(struct dppd_worker_stats *stats);
void dppd_stats_add(struct dppd_worker_stats *stats, const struct dppd_stats_values *delta);
void dppd_stats_read(const struct dppd_worker_stats *stats, struct dppd_stats_values *values);
void dppd_stats_accumulate(struct dppd_stats_values *total,
                           const struct dppd_stats_values *values);

#endif

