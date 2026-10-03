#include "dppd/telemetry.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rte_telemetry.h>
#include "dppd/control.h"
#include "dppd/runtime.h"

/** 小页保证 JSON 回复受控，规则数量超过一页时用 ID 与仓库版本继续查询 */
#define RULE_PAGE_SIZE 64U

/** 规则副本只包含值，查询线程不会持有软件快照、规则仓库或网卡 handle */
struct telemetry_rule {
    uint64_t id;
    uint64_t generation;
    uint16_t port_id;
    enum dppd_fallback_policy fallback;
    struct dppd_rule_install_info installation;
    enum dppd_plan_backend backend;
    int error;
};

/** 一个发布时刻的整体规则信息，与同次发布的规则数组配对 */
struct telemetry_rules {
    struct dppd_rule_metrics metrics;
    struct dppd_rule_latency_report latency;
    struct dppd_control_persistence_status persistence;
    uint64_t generation;
    uint32_t count;
    uint32_t software_rules;
    uint32_t rte_flow_rules;
    uint32_t unavailable_rules;
    uint32_t hardware_objects;
    uint32_t software_objects;
    enum dppd_control_recovery_state recovery;
    int recovery_error;
};

/**
 * 只有管理线程构建 work，独立查询线程只能在短锁内复制 rows 和 summary
 * 双缓冲交换避免查询等待驱动校验、创建或磁盘保存，锁不进入逐包路径
 * runtime 的统计读取也受同一生命周期锁保护，解绑后不再有查询借用该指针
 */
static struct {
    pthread_mutex_t lock;
    const struct dppd_runtime *runtime;
    struct telemetry_rule *rows;
    struct telemetry_rule *work;
    struct telemetry_rules summary;
    uint32_t capacity;
    uint64_t publication;
    int snapshot_error;
} active = {.lock = PTHREAD_MUTEX_INITIALIZER};
static unsigned int registered_commands;

/** 每个字段的构造错误都回传，不能展示一份被静默截断的成功 JSON */
#define ADD_U64(name, value) do { \
    int field_rc = rte_tel_data_add_dict_u64(data, name, value); \
    if (field_rc != 0) return field_rc; \
} while (0)
#define ADD_INT(name, value) do { \
    int field_rc = rte_tel_data_add_dict_int(data, name, value); \
    if (field_rc != 0) return field_rc; \
} while (0)
#define ADD_STRING(name, value) do { \
    int field_rc = rte_tel_data_add_dict_string(data, name, value); \
    if (field_rc != 0) return field_rc; \
} while (0)
#define START_DICT() do { \
    int field_rc = rte_tel_data_start_dict(data); \
    if (field_rc != 0) return field_rc; \
} while (0)

/** 不接受空值、符号、空白或溢出，逗号只用于分隔明确规定的参数 */
static int parse_number(const char **cursor, uint64_t *value)
{
    const char *position = *cursor;
    uint64_t number = 0;

    if (position == NULL || *position < '0' || *position > '9')
        return -EINVAL;
    do {
        unsigned int digit = (unsigned int)(*position - '0');

        if (number > (UINT64_MAX - digit) / 10U)
            return -EINVAL;
        number = number * 10U + digit;
        position++;
    } while (*position >= '0' && *position <= '9');
    if (*position != '\0' && *position != ',')
        return -EINVAL;
    *value = number;
    *cursor = position;
    return 0;
}

/** 只有拥有 control 的管理线程调用此函数，复制期间没有仓库写入者 */
static int build_snapshot(const struct dppd_control_service *control,
    struct telemetry_rule *rows, uint32_t capacity, struct telemetry_rules *summary)
{
    uint64_t cursor = 0;

    memset(summary, 0, sizeof(*summary));
    dppd_control_rule_metrics(control, &summary->metrics);
    dppd_control_rule_latency(control, &summary->latency);
    dppd_control_persistence_status(control, &summary->persistence);
    summary->generation = control->rules.generation;
    summary->count = control->rules.count;
    summary->recovery = control->recovery_state;
    summary->recovery_error = control->recovery_last_error;
    summary->hardware_objects = dppd_rte_flow_backend_count(&control->rte_flow);
    summary->software_objects = dppd_software_backend_count(&control->software);
    if (summary->count > capacity)
        return -EUCLEAN;
    for (uint32_t index = 0; index < summary->count; ++index) {
        struct dppd_rule rule;
        struct dppd_control_rule_status status;
        struct telemetry_rule *row = &rows[index];
        uint64_t generation;
        uint32_t count;
        bool more;
        int rc;

        rc = dppd_rule_repository_list(&control->rules, cursor, summary->generation,
                                       &rule, 1, &count, &more, &generation);
        if (rc != 0 || count != 1)
            return -EUCLEAN;
        memset(row, 0, sizeof(*row));
        row->id = rule.id;
        row->generation = rule.generation;
        row->port_id = rule.install_port_id;
        row->fallback = rule.fallback;
        row->error = dppd_control_rule_status(control, rule.id, rule.generation, &status);
        if (row->error == 0) {
            row->installation = status.installation;
            row->backend = status.backend;
            if (row->backend == DPPD_PLAN_BACKEND_SOFTWARE)
                summary->software_rules++;
            else
                summary->rte_flow_rules++;
        } else {
            summary->unavailable_rules++;
        }
        cursor = rule.id;
    }
    return 0;
}

/** 原有报文统计仍按原子字段采样，生命周期锁只防止退出时释放实例 */
static int stats_callback(const char *command, const char *parameters, struct rte_tel_data *data)
{
    struct dppd_stats_values stats;

    (void)command;
    if (parameters != NULL && parameters[0] != '\0')
        return -EINVAL;
    pthread_mutex_lock(&active.lock);
    if (active.runtime == NULL) {
        pthread_mutex_unlock(&active.lock);
        return -EAGAIN;
    }
    dppd_runtime_stats_read(active.runtime, &stats);
    pthread_mutex_unlock(&active.lock);
    START_DICT();
#define ADD_STATS(field) ADD_U64(#field, stats.field);
    DPPD_STATS_FIELDS(ADD_STATS)
#undef ADD_STATS
    return 0;
}

/** 分页先复制最多六十四个 ID，离开锁后构造 JSON，不把整个规则数组交给 DPDK */
static int rules_callback(const char *command, const char *parameters, struct rte_tel_data *data)
{
    struct telemetry_rules summary;
    struct rte_tel_data *array;
    uint64_t ids[RULE_PAGE_SIZE], after = 0, expected = DPPD_RULE_GENERATION_ANY, publication;
    uint32_t count = 0;
    bool more = false;
    int rc;

    (void)command;
    if (parameters != NULL && parameters[0] != '\0') {
        if (parse_number(&parameters, &after) != 0)
            return -EINVAL;
        if (*parameters == ',') {
            parameters++;
            if (parse_number(&parameters, &expected) != 0 || expected == DPPD_RULE_GENERATION_ANY)
                return -EINVAL;
        }
        if (*parameters != '\0')
            return -EINVAL;
    }
    pthread_mutex_lock(&active.lock);
    if (active.runtime == NULL || active.snapshot_error != 0) {
        rc = active.runtime == NULL ? -EAGAIN : active.snapshot_error;
        pthread_mutex_unlock(&active.lock);
        return rc;
    }
    summary = active.summary;
    publication = active.publication;
    if (expected != DPPD_RULE_GENERATION_ANY && expected != summary.generation) {
        pthread_mutex_unlock(&active.lock);
        return -ESTALE;
    }
    for (uint32_t index = 0; index < summary.count; ++index) {
        if (active.rows[index].id <= after)
            continue;
        if (count == RULE_PAGE_SIZE) {
            more = true;
            break;
        }
        ids[count++] = active.rows[index].id;
    }
    pthread_mutex_unlock(&active.lock);
    START_DICT();
    ADD_U64("publication", publication);
    ADD_U64("repository_generation", summary.generation);
    ADD_U64("total", summary.count);
    ADD_U64("software_rules", summary.software_rules);
    ADD_U64("rte_flow_rules", summary.rte_flow_rules);
    ADD_U64("unavailable_rules", summary.unavailable_rules);
    ADD_U64("hardware_objects", summary.hardware_objects);
    ADD_U64("software_objects", summary.software_objects);
    ADD_INT("recovery_state", summary.recovery);
    ADD_INT("recovery_error", summary.recovery_error);
    ADD_INT("persistence_enabled", summary.persistence.enabled);
    ADD_INT("dirty", summary.persistence.dirty);
    ADD_U64("persisted_generation", summary.persistence.persisted_generation);
    ADD_U64("operations", summary.metrics.operations);
    ADD_U64("failed", summary.metrics.failed);
    ADD_U64("last_failure_sequence", summary.metrics.last.sequence);
    ADD_U64("returned", count);
    ADD_INT("has_more", more);
    ADD_U64("next_after", count == 0 ? after : ids[count - 1U]);
    array = rte_tel_data_alloc();
    if (array == NULL)
        return -ENOMEM;
    rc = rte_tel_data_start_array(array, RTE_TEL_U64_VAL);
    for (uint32_t index = 0; rc == 0 && index < count; ++index)
        rc = rte_tel_data_add_array_u64(array, ids[index]);
    if (rc == 0)
        rc = rte_tel_data_add_dict_container(data, "rule_ids", array, 0);
    /** 成功交接后由 telemetry 释放容器，失败时所有权还在本函数 */
    if (rc != 0)
        rte_tel_data_free(array);
    return rc;
}

/** 单规则查询从已发布副本二分定位，不访问 PMD、COUNT 或软件回收队列 */
static int rule_callback(const char *command, const char *parameters, struct rte_tel_data *data)
{
    struct telemetry_rule row;
    uint64_t id, publication, generation;
    uint32_t low = 0, high;
    int rc;

    (void)command;
    if (parse_number(&parameters, &id) != 0 || *parameters != '\0' || id == 0)
        return -EINVAL;
    pthread_mutex_lock(&active.lock);
    if (active.runtime == NULL || active.snapshot_error != 0) {
        rc = active.runtime == NULL ? -EAGAIN : active.snapshot_error;
        pthread_mutex_unlock(&active.lock);
        return rc;
    }
    high = active.summary.count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2U;

        if (active.rows[middle].id < id)
            low = middle + 1U;
        else
            high = middle;
    }
    if (low == active.summary.count || active.rows[low].id != id) {
        pthread_mutex_unlock(&active.lock);
        return -ENOENT;
    }
    row = active.rows[low];
    publication = active.publication;
    generation = active.summary.generation;
    pthread_mutex_unlock(&active.lock);
    START_DICT();
    ADD_U64("publication", publication);
    ADD_U64("repository_generation", generation);
    ADD_U64("rule_id", row.id);
    ADD_U64("generation", row.generation);
    ADD_INT("port", row.port_id);
    ADD_INT("status_error", row.error);
    ADD_STRING("backend", row.error != 0 ? "unknown" :
        row.backend == DPPD_PLAN_BACKEND_SOFTWARE ? "software" : "rte_flow");
    ADD_STRING("fallback", row.fallback == DPPD_FALLBACK_SOFTWARE_ONLY ? "software-only" :
        row.fallback == DPPD_FALLBACK_REQUIRE_HARDWARE ? "require-hardware" : "prefer-hardware");
    ADD_INT("count_configured", row.error == 0 && row.installation.has_count);
    ADD_INT("timing_available", row.error == 0 && row.installation.timing_available);
    ADD_U64("install_duration_ns", row.installation.install_duration_ns);
    ADD_U64("commit_rule_count", row.installation.commit_rule_count);
    return 0;
}

/** 失败指标复制后即可解锁，成功请求和重复查询不清除最近失败 */
static int failures_callback(const char *command, const char *parameters, struct rte_tel_data *data)
{
    struct dppd_rule_metrics metrics;
    const struct dppd_rule_failure_event *last = &metrics.last;
    uint64_t publication;

    (void)command;
    if (parameters != NULL && parameters[0] != '\0')
        return -EINVAL;
    pthread_mutex_lock(&active.lock);
    if (active.runtime == NULL) {
        pthread_mutex_unlock(&active.lock);
        return -EAGAIN;
    }
    metrics = active.summary.metrics;
    publication = active.publication;
    pthread_mutex_unlock(&active.lock);
    START_DICT();
    ADD_U64("publication", publication);
#define ADD_METRIC(field) ADD_U64(#field, metrics.field);
    DPPD_RULE_METRIC_FIELDS(ADD_METRIC)
#undef ADD_METRIC
    for (unsigned int kind = DPPD_RULE_FAILURE_INPUT; kind < DPPD_RULE_FAILURE_KIND_COUNT; ++kind) {
        char name[64];

        snprintf(name, sizeof(name), "failed_%s", dppd_rule_failure_kind_name(kind));
        ADD_U64(name, metrics.failures[kind]);
    }
    ADD_U64("last_sequence", last->sequence);
    ADD_STRING("operation", dppd_rule_operation_name(last->operation));
    ADD_STRING("stage", dppd_rule_failure_stage_name(last->stage));
    ADD_STRING("kind", dppd_rule_failure_kind_name(last->kind));
    ADD_U64("rule_id", last->rule_id);
    ADD_U64("generation", last->generation);
    ADD_U64("transaction_id", last->transaction_id);
    ADD_U64("rule_count", last->rule_count);
    ADD_INT("port_known", last->port_known);
    ADD_INT("port", last->install_port_id);
    ADD_INT("backend_known", last->backend_known);
    ADD_STRING("backend", !last->backend_known ? "unknown" :
        last->backend == DPPD_PLAN_BACKEND_SOFTWARE ? "software" : "rte_flow");
    ADD_INT("cause_error", last->cause_code);
    ADD_INT("response_error", last->response_code);
    ADD_INT("compensation_error", last->compensation_code);
    ADD_U64("compensation_rule_id", last->compensation_rule_id);
    ADD_INT("last_applied", last->applied);
    return 0;
}

/**
 * 每组字段使用独立前缀，区间计数保持互斥，不需要动态分配子容器
 * 全部三组和十六个上界仍在 DPDK 字典容量内，字段构造失败直接回传错误
 */
static int add_latency_scope(struct rte_tel_data *data, enum dppd_rule_latency_scope scope,
    const struct dppd_rule_latency_summary *summary)
{
    const struct dppd_rule_latency_histogram *histogram = &summary->histogram;
    const char *prefix = dppd_rule_latency_scope_name(scope);
    char name[64];

#define ADD_HISTOGRAM(field) do { \
    snprintf(name, sizeof(name), "%s_" #field, prefix); \
    ADD_U64(name, histogram->field); \
} while (0)
    ADD_HISTOGRAM(samples); ADD_HISTOGRAM(unavailable); ADD_HISTOGRAM(rules);
    ADD_HISTOGRAM(total_ns); ADD_HISTOGRAM(min_ns); ADD_HISTOGRAM(max_ns);
#undef ADD_HISTOGRAM
#define ADD_SUMMARY(field) do { \
    snprintf(name, sizeof(name), "%s_" #field, prefix); \
    ADD_U64(name, summary->field); \
} while (0)
    ADD_SUMMARY(mean_ns); ADD_SUMMARY(p50_upper_ns); ADD_SUMMARY(p95_upper_ns); ADD_SUMMARY(p99_upper_ns);
#undef ADD_SUMMARY
    snprintf(name, sizeof(name), "%s_mean_available", prefix);
    ADD_INT(name, summary->mean_available);
    snprintf(name, sizeof(name), "%s_quantiles_available", prefix);
    ADD_INT(name, summary->quantiles_available);
    snprintf(name, sizeof(name), "%s_total_saturated", prefix);
    ADD_INT(name, histogram->total_saturated);
    snprintf(name, sizeof(name), "%s_counters_saturated", prefix);
    ADD_INT(name, histogram->counters_saturated);
    for (unsigned int bucket = 0; bucket < DPPD_RULE_LATENCY_BUCKETS; ++bucket) {
        snprintf(name, sizeof(name), "%s_bucket_%u", prefix, bucket);
        ADD_U64(name, histogram->buckets[bucket]);
    }
    return 0;
}

/** 从完整发布副本读取历史，过滤只选分组，不触发后端访问或新的计时 */
static int latency_callback(const char *command, const char *parameters, struct rte_tel_data *data)
{
    struct dppd_rule_latency_report report;
    unsigned int selected = DPPD_RULE_LATENCY_SCOPE_COUNT;
    uint64_t publication;

    (void)command;
    if (parameters != NULL && parameters[0] != '\0') {
        for (unsigned int scope = 0; scope < DPPD_RULE_LATENCY_SCOPE_COUNT; ++scope) {
            if (strcmp(parameters, dppd_rule_latency_scope_name(scope)) == 0)
                selected = scope;
        }
        if (selected == DPPD_RULE_LATENCY_SCOPE_COUNT)
            return -EINVAL;
    }
    pthread_mutex_lock(&active.lock);
    if (active.runtime == NULL) {
        pthread_mutex_unlock(&active.lock);
        return -EAGAIN;
    }
    report = active.summary.latency;
    publication = active.publication;
    pthread_mutex_unlock(&active.lock);
    START_DICT();
    ADD_U64("publication", publication);
    ADD_STRING("scope", selected == DPPD_RULE_LATENCY_SCOPE_COUNT ? "all" : dppd_rule_latency_scope_name(selected));
    ADD_U64("bucket_count", DPPD_RULE_LATENCY_BUCKETS);
    for (unsigned int bucket = 0; bucket < DPPD_RULE_LATENCY_BUCKETS; ++bucket) {
        char name[64];

        snprintf(name, sizeof(name), "bucket_%u_upper_ns", bucket);
        ADD_U64(name, dppd_rule_latency_bucket_upper(bucket));
    }
    for (unsigned int scope = 0; scope < DPPD_RULE_LATENCY_SCOPE_COUNT; ++scope) {
        int rc;

        if (selected != DPPD_RULE_LATENCY_SCOPE_COUNT && selected != scope)
            continue;
        rc = add_latency_scope(data, scope, &report.scopes[scope]);
        if (rc != 0)
            return rc;
    }
    return 0;
}

/** 注册失败时保留已完成的进度，重试只补剩余命令，所有回调在绑定前都返回不可用 */
int dppd_telemetry_register(const struct dppd_runtime *runtime,
                            const struct dppd_control_service *control)
{
    static const struct {
        const char *name;
        telemetry_cb callback;
        const char *help;
    } commands[] = {
        {"/dppd/stats", stats_callback, "Aggregate software packet counters. No parameters."},
        {"/dppd/rules", rules_callback, "Rule summary and 64 IDs. Optional: after_id,repository_generation."},
        {"/dppd/rule", rule_callback, "Installed rule snapshot. Required: rule_id."},
        {"/dppd/rule_failures", failures_callback, "Control outcomes and last failure. No parameters."},
        {"/dppd/rule_latency", latency_callback, "Successful commit history. Optional: software, software_batch or rte_flow."},
    };
    struct telemetry_rule *rows, *work;
    struct telemetry_rules summary;
    int rc;

    if (runtime == NULL || control == NULL || control->rules.records == NULL)
        return -EINVAL;
    pthread_mutex_lock(&active.lock);
    if (active.runtime != NULL) {
        pthread_mutex_unlock(&active.lock);
        return -EALREADY;
    }
    pthread_mutex_unlock(&active.lock);
    rows = calloc(control->rules.capacity, sizeof(*rows));
    work = calloc(control->rules.capacity, sizeof(*work));
    if (rows == NULL || work == NULL) {
        free(rows);
        free(work);
        return -ENOMEM;
    }
    rc = build_snapshot(control, rows, control->rules.capacity, &summary);
    while (rc == 0 && registered_commands < sizeof(commands) / sizeof(commands[0])) {
        rc = rte_telemetry_register_cmd(commands[registered_commands].name,
            commands[registered_commands].callback, commands[registered_commands].help);
        if (rc == 0)
            registered_commands++;
    }
    if (rc != 0) {
        free(rows);
        free(work);
        return rc;
    }
    pthread_mutex_lock(&active.lock);
    active.runtime = runtime;
    active.rows = rows;
    active.work = work;
    active.summary = summary;
    active.capacity = control->rules.capacity;
    active.publication = 1;
    active.snapshot_error = 0;
    pthread_mutex_unlock(&active.lock);
    return 0;
}

/** 静态表和指标没有变化时不重建，变化时在锁外复制，最后短锁交换整份副本 */
int dppd_telemetry_publish_rules(const struct dppd_control_service *control)
{
    struct telemetry_rules summary, previous;
    struct dppd_rule_latency_report latency;
    struct telemetry_rule *work, *old;
    uint32_t capacity;
    int rc;

    if (control == NULL || control->rules.records == NULL)
        return -EINVAL;
    pthread_mutex_lock(&active.lock);
    if (active.runtime == NULL) {
        pthread_mutex_unlock(&active.lock);
        return -EAGAIN;
    }
    work = active.work;
    capacity = active.capacity;
    previous = active.summary;
    pthread_mutex_unlock(&active.lock);
    /** 后端自身的成功提交也可能没有改变账本，显式对比历史，避免副本漏掉这类变化 */
    dppd_control_rule_latency(control, &latency);
    if (previous.metrics.operations == control->observation.metrics.operations &&
        previous.generation == control->rules.generation &&
        previous.recovery == control->recovery_state &&
        previous.recovery_error == control->recovery_last_error &&
        previous.persistence.enabled == (control->persistence_path != NULL) &&
        previous.persistence.dirty == control->persistence_dirty &&
        previous.persistence.last_error == control->persistence_last_error &&
        previous.persistence.persisted_generation == control->persisted_generation &&
        memcmp(&previous.latency, &latency, sizeof(latency)) == 0)
        return 0;
    rc = build_snapshot(control, work, capacity, &summary);
    pthread_mutex_lock(&active.lock);
    old = active.rows;
    active.rows = work;
    active.work = old;
    active.summary = summary;
    active.snapshot_error = rc;
    active.publication++;
    pthread_mutex_unlock(&active.lock);
    return rc;
}

/** 与查询的短锁串行，所有旧借用结束后清空入口，后续回调只能返回 EAGAIN */
void dppd_telemetry_unregister_runtime(void)
{
    pthread_mutex_lock(&active.lock);
    active.runtime = NULL;
    free(active.rows);
    free(active.work);
    active.rows = active.work = NULL;
    memset(&active.summary, 0, sizeof(active.summary));
    active.capacity = 0;
    active.publication = 0;
    active.snapshot_error = 0;
    pthread_mutex_unlock(&active.lock);
}
