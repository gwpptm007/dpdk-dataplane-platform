#ifndef DPPD_STATS_H
#define DPPD_STATS_H

#include <stdatomic.h>
#include <stdint.h>
#include <rte_common.h>
#include "dppd/stats_values.h"

struct dppd_worker_stats {
#define DPPD_STATS_ATOMIC(field) atomic_uint_fast64_t field;
    DPPD_STATS_FIELDS(DPPD_STATS_ATOMIC)
#undef DPPD_STATS_ATOMIC
} __rte_cache_aligned;

void dppd_stats_init(struct dppd_worker_stats *stats);
void dppd_stats_add(struct dppd_worker_stats *stats, const struct dppd_stats_values *delta);
void dppd_stats_read(const struct dppd_worker_stats *stats, struct dppd_stats_values *values);
void dppd_stats_accumulate(struct dppd_stats_values *total,
                           const struct dppd_stats_values *values);

#endif
