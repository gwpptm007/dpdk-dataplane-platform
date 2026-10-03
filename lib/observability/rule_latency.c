#include "dppd/rule_latency.h"

#include <string.h>

/**
 * 每个成功提交最多增加一个区间，时钟未知的提交只增加 unavailable 和涉及规则数
 * 先核对所有计数的空间，再整体更新，饱和时保留上一份一致的历史并冻结此组
 */
void dppd_rule_latency_record(struct dppd_rule_latency_histogram *histogram,
    bool timing_available, uint64_t duration_ns, uint32_t rule_count)
{
    uint64_t *counter;
    unsigned int bucket = 0;

    if (histogram == NULL || rule_count == 0 || histogram->counters_saturated)
        return;
    counter = timing_available ? &histogram->samples : &histogram->unavailable;
    if (*counter == UINT64_MAX || histogram->rules > UINT64_MAX - rule_count) {
        histogram->counters_saturated = true;
        return;
    }
    (*counter)++;
    histogram->rules += rule_count;
    if (!timing_available)
        return;
    while (duration_ns > dppd_rule_latency_bucket_upper(bucket))
        bucket++;
    histogram->buckets[bucket]++;
    if (histogram->samples == 1 || duration_ns < histogram->min_ns)
        histogram->min_ns = duration_ns;
    if (duration_ns > histogram->max_ns)
        histogram->max_ns = duration_ns;
    if (duration_ns > UINT64_MAX - histogram->total_ns) {
        histogram->total_ns = UINT64_MAX;
        histogram->total_saturated = true;
    } else if (!histogram->total_saturated) {
        histogram->total_ns += duration_ns;
    }
}

/**
 * 按向上取整的样本排名定位区间，先除后乘避免 samples 接近 UINT64_MAX 时溢出
 * 最后一个区间没有固定有限上界，用该组已观察到的实际最大值作为上界
 */
static uint64_t percentile_upper(const struct dppd_rule_latency_histogram *histogram,
                                  unsigned int percentile)
{
    uint64_t target = histogram->samples / 100U * percentile +
        (histogram->samples % 100U * percentile + 99U) / 100U;
    uint64_t seen = 0;

    for (unsigned int bucket = 0; bucket < DPPD_RULE_LATENCY_BUCKETS; ++bucket) {
        seen += histogram->buckets[bucket];
        if (seen >= target) {
            uint64_t upper = dppd_rule_latency_bucket_upper(bucket);

            return upper < histogram->max_ns ? upper : histogram->max_ns;
        }
    }
    return 0;
}

/** 空统计、未知时钟和饱和均显式标记不可用，查询不清零、不重算安装和不改变区间 */
void dppd_rule_latency_summarize(const struct dppd_rule_latency_histogram *histogram,
    struct dppd_rule_latency_summary *summary)
{
    if (summary == NULL)
        return;
    memset(summary, 0, sizeof(*summary));
    if (histogram == NULL)
        return;
    summary->histogram = *histogram;
    summary->quantiles_available = histogram->samples != 0 && !histogram->counters_saturated;
    summary->mean_available = summary->quantiles_available && !histogram->total_saturated;
    if (summary->mean_available)
        summary->mean_ns = histogram->total_ns / histogram->samples;
    if (summary->quantiles_available) {
        summary->p50_upper_ns = percentile_upper(histogram, 50);
        summary->p95_upper_ns = percentile_upper(histogram, 95);
        summary->p99_upper_ns = percentile_upper(histogram, 99);
    }
}
