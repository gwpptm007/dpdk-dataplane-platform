#include "dppd/stats.h"

/** 从同一份字段清单生成初始化、累加和读取操作，新增统计项时不会漏改其中一处 */
#define STORE_ZERO(field) atomic_init(&stats->field, 0);
#define ADD_FIELD(field) atomic_fetch_add_explicit(&stats->field, delta->field, memory_order_relaxed);
#define READ_FIELD(field) values->field = atomic_load_explicit(&stats->field, memory_order_relaxed);
#define ACCUMULATE_FIELD(field) total->field += values->field;

void dppd_stats_init(struct dppd_worker_stats *stats)
{
    DPPD_STATS_FIELDS(STORE_ZERO)
}

void dppd_stats_add(struct dppd_worker_stats *stats, const struct dppd_stats_values *delta)
{
    DPPD_STATS_FIELDS(ADD_FIELD)
}

/** 每个计数器都可安全并发读取，但整组数值不保证来自完全相同的时刻 */
void dppd_stats_read(const struct dppd_worker_stats *stats, struct dppd_stats_values *values)
{
    DPPD_STATS_FIELDS(READ_FIELD)
}

void dppd_stats_accumulate(struct dppd_stats_values *total, const struct dppd_stats_values *values)
{
    DPPD_STATS_FIELDS(ACCUMULATE_FIELD)
}
