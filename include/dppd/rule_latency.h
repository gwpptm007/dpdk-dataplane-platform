#ifndef DPPD_RULE_LATENCY_H
#define DPPD_RULE_LATENCY_H

#include <stdbool.h>
#include <stdint.h>

/** 固定十六个区间，只保存累计次数，内存用量不会随安装历史增长 */
#define DPPD_RULE_LATENCY_BUCKETS 16U

/** 软件整批发布与单条提交耗时含义不同，不能把同一次整批耗时重复计到每个成员 */
enum dppd_rule_latency_scope {
    DPPD_RULE_LATENCY_SOFTWARE = 0,
    DPPD_RULE_LATENCY_SOFTWARE_BATCH,
    DPPD_RULE_LATENCY_RTE_FLOW,
    DPPD_RULE_LATENCY_SCOPE_COUNT,
};

/**
 * 后端成功提交的历史统计，之后删除或回滚不会扣掉已发生的成功提交
 * samples 只数有效测量，unavailable 单独数时钟不可用的成功提交，rules 数两者涉及的规则
 * total_ns 溢出时饱和并标记，计数溢出时冻结整组，不能绕回零制造错误的均值或分位数
 */
struct dppd_rule_latency_histogram {
    uint64_t samples;
    uint64_t unavailable;
    uint64_t rules;
    uint64_t total_ns;
    uint64_t min_ns;
    uint64_t max_ns;
    /** 区间互斥，所有 buckets 相加等于 samples，不是累积小于某上界的计数 */
    uint64_t buckets[DPPD_RULE_LATENCY_BUCKETS];
    bool total_saturated;
    bool counters_saturated;
};

/**
 * 查询时从完整直方图计算摘要，不修改历史，也不访问时钟或驱动
 * 分位数给出所落区间的上界，并受实际最大值约束，不冒充精确的排序样本值
 * available 字段为假时对应数值输出零，零耗时本身仍可以是一个有效样本
 */
struct dppd_rule_latency_summary {
    struct dppd_rule_latency_histogram histogram;
    uint64_t mean_ns;
    uint64_t p50_upper_ns;
    uint64_t p95_upper_ns;
    uint64_t p99_upper_ns;
    bool mean_available;
    bool quantiles_available;
};

/** 一份完成态包含三个独立统计口径，CLI 与 telemetry 使用相同的结构和名称 */
struct dppd_rule_latency_report {
    struct dppd_rule_latency_summary scopes[DPPD_RULE_LATENCY_SCOPE_COUNT];
};

/** 上界单位统一为纳秒，最后一个区间容纳超过十秒的所有值 */
static inline uint64_t dppd_rule_latency_bucket_upper(unsigned int index)
{
    static const uint64_t bounds[DPPD_RULE_LATENCY_BUCKETS] = {
        1000, 5000, 10000, 50000, 100000, 500000, 1000000, 5000000,
        10000000, 50000000, 100000000, 500000000, 1000000000,
        UINT64_C(5000000000), UINT64_C(10000000000), UINT64_MAX,
    };
    return index < DPPD_RULE_LATENCY_BUCKETS ? bounds[index] : UINT64_MAX;
}

/** 同一个名称同时用于 CLI 的分组标签与 telemetry 的字段前缀 */
static inline const char *dppd_rule_latency_scope_name(enum dppd_rule_latency_scope scope)
{
    static const char *const names[] = {"software", "software_batch", "rte_flow"};
    return (unsigned int)scope < DPPD_RULE_LATENCY_SCOPE_COUNT ? names[scope] : "unknown";
}

/** 只接收后端已有的成功提交测量，不新增计时、分配或逐包操作 */
void dppd_rule_latency_record(struct dppd_rule_latency_histogram *histogram,
    bool timing_available, uint64_t duration_ns, uint32_t rule_count);
/** 复制历史并计算可用摘要，调用方负责与该后端的控制面写者串行 */
void dppd_rule_latency_summarize(const struct dppd_rule_latency_histogram *histogram,
    struct dppd_rule_latency_summary *summary);

#endif
