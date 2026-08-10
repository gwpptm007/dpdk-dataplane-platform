#include "dppd/stats.h"

#include <string.h>

#define STORE_ZERO(field) atomic_init(&stats->field, 0)
#define ADD_FIELD(field) atomic_fetch_add_explicit(&stats->field, delta->field, memory_order_relaxed)
#define READ_FIELD(field) values->field = atomic_load_explicit(&stats->field, memory_order_relaxed)

void dppd_stats_init(struct dppd_worker_stats *stats)
{
    STORE_ZERO(rx_packets);
    STORE_ZERO(rx_bytes);
    STORE_ZERO(tx_packets);
    STORE_ZERO(tx_bytes);
    STORE_ZERO(rx_malformed);
    STORE_ZERO(rx_unsupported);
    STORE_ZERO(policy_drops);
    STORE_ZERO(tx_drops);
}

void dppd_stats_add(struct dppd_worker_stats *stats, const struct dppd_stats_values *delta)
{
    ADD_FIELD(rx_packets);
    ADD_FIELD(rx_bytes);
    ADD_FIELD(tx_packets);
    ADD_FIELD(tx_bytes);
    ADD_FIELD(rx_malformed);
    ADD_FIELD(rx_unsupported);
    ADD_FIELD(policy_drops);
    ADD_FIELD(tx_drops);
}

void dppd_stats_read(const struct dppd_worker_stats *stats, struct dppd_stats_values *values)
{
    READ_FIELD(rx_packets);
    READ_FIELD(rx_bytes);
    READ_FIELD(tx_packets);
    READ_FIELD(tx_bytes);
    READ_FIELD(rx_malformed);
    READ_FIELD(rx_unsupported);
    READ_FIELD(policy_drops);
    READ_FIELD(tx_drops);
}

void dppd_stats_accumulate(struct dppd_stats_values *total,
                           const struct dppd_stats_values *values)
{
    total->rx_packets += values->rx_packets;
    total->rx_bytes += values->rx_bytes;
    total->tx_packets += values->tx_packets;
    total->tx_bytes += values->tx_bytes;
    total->rx_malformed += values->rx_malformed;
    total->rx_unsupported += values->rx_unsupported;
    total->policy_drops += values->policy_drops;
    total->tx_drops += values->tx_drops;
}
