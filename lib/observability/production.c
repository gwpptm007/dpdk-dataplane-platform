#define _GNU_SOURCE
#include "dppd/production.h"
#include "dppd/flow_management.h"
#include "dppd/runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <rte_telemetry.h>
#include <rte_version.h>

/** 计数范围单独标记，共享对象的总值不能误认为某一条规则独享的命中数 */
struct production_rule {
    uint64_t id, generation, table, hits, bytes, shared_id, shared_generation, sampled_ms;
    uint32_t queue;
    uint16_t port;
    int count_error;
    bool counted, shared, requested;
};

struct dppd_production {
    struct dppd_runtime *runtime;
    struct dppd_flow_management *flows;
    struct production_rule *rows, *work;
    struct dppd_management_health health;
    struct dppd_flow_store_status status;
    uint32_t capacity, count, sample_cursor, pending_samples;
    uint64_t revision, sampled_ms, refreshed_ms, logged_ms;
    int snapshot_error;
    bool published, monitored;
};

/** 生命周期锁只保护副本与原子统计的借用，不持锁执行驱动操作或写盘 */
static pthread_mutex_t publication_lock = PTHREAD_MUTEX_INITIALIZER;
static struct dppd_production *active;
static unsigned int registered;
static atomic_uint_fast64_t log_dropped;

uint64_t dppd_production_log_dropped(void)
{
    return atomic_load_explicit(&log_dropped, memory_order_relaxed);
}

/** 管道读端可能刚好退出，仅在本线程临时屏蔽并消费本次写入产生的 SIGPIPE */
static ssize_t pipe_write(int fd, const char *line, size_t length)
{
    sigset_t blocked, previous, pending;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    int rc = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    if (rc != 0)
        return -1;
    ssize_t written = -1;
    if (sigpending(&pending) == 0) {
        bool already_pending = sigismember(&pending, SIGPIPE) == 1;
        written = write(fd, line, length);
        if (written < 0 && errno == EPIPE && !already_pending) {
            struct timespec immediate = {0};
            (void)sigtimedwait(&blocked, NULL, &immediate);
        }
    }
    (void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
    return written;
}

/** 管道与终端用独立非阻塞描述符，socket 使用逐次非阻塞发送，不修改父进程共享的 stderr 标志 */
static void emit_log(const char *line, int length, size_t capacity)
{
    int previous_errno = errno;
    ssize_t written = -1;
    struct stat status;
    if (length > 0 && (size_t)length < capacity && fstat(STDERR_FILENO, &status) == 0) {
        if (S_ISSOCK(status.st_mode)) {
            written = send(STDERR_FILENO, line, (size_t)length, MSG_DONTWAIT | MSG_NOSIGNAL);
        } else if (S_ISREG(status.st_mode)) {
            /** 普通文件保留原文件偏移，不从新描述符重开后覆盖或打乱已有日志 */
            written = write(STDERR_FILENO, line, (size_t)length);
        } else {
            int fd = open("/proc/self/fd/2", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0) {
                written = pipe_write(fd, line, (size_t)length);
                close(fd);
            }
        }
    }
    if (written != length || length <= 0)
        atomic_fetch_add_explicit(&log_dropped, 1, memory_order_relaxed);
    errno = previous_errno;
}

void dppd_production_log(enum dppd_production_event event, int error, uint64_t revision)
{
    static const char *const names[] = {"start", "ready", "isolated", "stop", "worker_timeout", "snapshot_failed"};
    const char *name = (unsigned int)event < sizeof(names) / sizeof(names[0]) ? names[event] : "unknown";
    char line[512];
    int length = snprintf(line, sizeof(line),
        "{\"schema\":1,\"component\":\"dppd\",\"event\":\"%s\",\"monotonic_ms\":%" PRIu64 ",\"revision\":%" PRIu64
        ",\"error\":%d}\n",
        name, dppd_monotonic_ms(), revision, error);
    emit_log(line, length, sizeof(line));
}

static int add_u64(struct rte_tel_data *data, const char *key, uint64_t value)
{
#if RTE_VERSION >= RTE_VERSION_NUM(23, 3, 0, 0)
    return rte_tel_data_add_dict_uint(data, key, value);
#else
    return rte_tel_data_add_dict_u64(data, key, value);
#endif
}

#define ADD_U64(key, value)                  \
    do {                                     \
        int rc_ = add_u64(data, key, value); \
        if (rc_ != 0)                        \
            return rc_;                      \
    } while (0)
#define ADD_INT(key, value)                                    \
    do {                                                       \
        int rc_ = rte_tel_data_add_dict_int(data, key, value); \
        if (rc_ != 0)                                          \
            return rc_;                                        \
    } while (0)
#define START_DICT()                             \
    do {                                         \
        int rc_ = rte_tel_data_start_dict(data); \
        if (rc_ != 0)                            \
            return rc_;                          \
    } while (0)

/** 严格消费十进制整数和逗号，空值、负数、尾随字符与溢出均不能变成有效过滤器 */
static int number(const char **text, uint64_t *output)
{
    const char *p = *text;
    uint64_t value = 0;
    if (p == NULL || *p < '0' || *p > '9')
        return -EINVAL;
    while (*p >= '0' && *p <= '9') {
        unsigned int digit = (unsigned int)(*p++ - '0');
        if (value > (UINT64_MAX - digit) / 10U)
            return -EINVAL;
        value = value * 10U + digit;
    }
    if (*p != '\0' && *p != ',')
        return -EINVAL;
    *text = p;
    *output = value;
    return 0;
}

static int status_callback(const char *command, const char *parameters, struct rte_tel_data *data)
{
    (void)command;
    if (parameters != NULL && *parameters != '\0')
        return -EINVAL;
    pthread_mutex_lock(&publication_lock);
    if (active == NULL) {
        pthread_mutex_unlock(&publication_lock);
        return -EAGAIN;
    }
    struct dppd_management_health health = active->health;
    struct dppd_flow_store_status status = active->status;
    uint64_t sampled = active->sampled_ms;
    int snapshot_error = active->snapshot_error;
    pthread_mutex_unlock(&publication_lock);
    START_DICT();
    ADD_U64("schema", 1);
    ADD_U64("sampled_ms", sampled);
    ADD_U64("revision", status.service.revision);
    ADD_U64("durable_revision", status.durable_revision);
    ADD_INT("ready", health.ready);
    ADD_INT("blockers", health.blockers);
    ADD_INT("dirty", status.dirty);
    ADD_INT("residual", status.residual);
    ADD_INT("error", status.last_error);
    ADD_INT("snapshot_error", snapshot_error);
    ADD_U64("workers_expected", health.workers_expected);
    ADD_U64("workers_running", health.workers_running);
    ADD_U64("workers_failed", health.workers_failed);
    ADD_U64("ports_started", health.ports_started);
    ADD_U64("links_up", health.links_up);
    ADD_U64("links_down", health.links_down);
    ADD_U64("links_unknown", health.links_unknown);
    ADD_U64("links_unsupported", health.links_unsupported);
    ADD_U64("rules", status.service.rules);
    ADD_U64("jobs", status.service.jobs);
    ADD_U64("log_dropped", dppd_production_log_dropped());
    ADD_U64("completed", status.service.completed);
    ADD_U64("age_queries", status.service.age_queries);
    ADD_U64("aged_deleted", status.service.aged_deleted);
    ADD_U64("age_query_errors", status.service.age_query_errors);
    ADD_U64("age_delete_errors", status.service.age_delete_errors);
    return 0;
}

static int stats_callback(const char *command, const char *parameters, struct rte_tel_data *data)
{
    (void)command;
    uint64_t port = DPPD_STATS_ALL, queue = DPPD_STATS_ALL;
    if (parameters != NULL && *parameters != '\0') {
        if (number(&parameters, &port) != 0 || *parameters++ != ',' || number(&parameters, &queue) != 0 ||
            *parameters != '\0' || port > UINT16_MAX || queue > UINT16_MAX)
            return -EINVAL;
    }
    struct dppd_stats_values stats;
    pthread_mutex_lock(&publication_lock);
    int rc =
        active == NULL ? -EAGAIN : dppd_runtime_stats_query(active->runtime, (uint16_t)port, (uint16_t)queue, &stats);
    pthread_mutex_unlock(&publication_lock);
    if (rc != 0)
        return rc;
    START_DICT();
    ADD_U64("schema", 1);
    ADD_U64("sampled_ms", dppd_monotonic_ms());
    ADD_U64("port", port);
    ADD_U64("queue", queue);
#define ADD_STAT(field) ADD_U64(#field, stats.field);
    DPPD_STATS_FIELDS(ADD_STAT)
#undef ADD_STAT
    return 0;
}

static int rule_callback(const char *command, const char *parameters, struct rte_tel_data *data)
{
    (void)command;
    uint64_t id;
    if (number(&parameters, &id) != 0 || *parameters != '\0' || id == 0)
        return -EINVAL;
    pthread_mutex_lock(&publication_lock);
    if (active == NULL || active->snapshot_error != 0) {
        int rc = active == NULL ? -EAGAIN : active->snapshot_error;
        pthread_mutex_unlock(&publication_lock);
        return rc;
    }
    uint32_t begin = 0, end = active->count;
    while (begin < end) {
        uint32_t middle = begin + (end - begin) / 2U;
        if (active->rows[middle].id < id)
            begin = middle + 1;
        else
            end = middle;
    }
    if (begin == active->count || active->rows[begin].id != id) {
        pthread_mutex_unlock(&publication_lock);
        return -ENOENT;
    }
    /** 查询只登记采样意愿，主线程下一轮处理有界数量，查询线程不进入仓库锁 */
    if (active->rows[begin].counted && !active->rows[begin].requested) {
        active->rows[begin].requested = true;
        ++active->pending_samples;
    }
    struct production_rule row = active->rows[begin];
    uint64_t revision = active->revision, sampled = active->refreshed_ms;
    pthread_mutex_unlock(&publication_lock);
    if (row.counted && row.sampled_ms == 0)
        return -EAGAIN;
    START_DICT();
    ADD_U64("schema", 1);
    ADD_U64("sampled_ms", sampled);
    ADD_U64("count_sampled_ms", row.sampled_ms);
    ADD_U64("revision", revision);
    ADD_U64("id", row.id);
    ADD_U64("generation", row.generation);
    ADD_U64("table", row.table);
    ADD_U64("queue", row.queue);
    ADD_U64("port", row.port);
    ADD_INT("count_available", row.counted && row.count_error == 0);
    ADD_INT("count_error", row.count_error);
    ADD_INT("shared", row.shared);
    ADD_U64("shared_id", row.shared_id);
    ADD_U64("shared_generation", row.shared_generation);
    ADD_U64("hits", row.hits);
    ADD_U64("bytes", row.bytes);
    return 0;
}

static int worker_callback(const char *command, const char *parameters, struct rte_tel_data *data)
{
    (void)command;
    uint64_t queue;
    if (number(&parameters, &queue) != 0 || *parameters != '\0')
        return -EINVAL;
    pthread_mutex_lock(&publication_lock);
    if (active == NULL || queue >= active->runtime->nb_workers) {
        int rc = active == NULL ? -EAGAIN : -ENOENT;
        pthread_mutex_unlock(&publication_lock);
        return rc;
    }
    const struct dppd_worker *worker = &active->runtime->workers[queue];
    uint64_t now = dppd_monotonic_ms();
    uint64_t heartbeat = atomic_load_explicit(&worker->heartbeat_ms, memory_order_acquire);
    unsigned int state = atomic_load_explicit(&worker->state, memory_order_acquire);
    unsigned int lcore = worker->lcore_id;
    int socket = worker->socket_id;
    uint32_t timeout = active->runtime->config.worker_timeout_ms;
    /** 运行标志仅由管理线程发布，查询线程不并发读取非原子的生命周期字段 */
    bool stalled =
        active->monitored && timeout != 0 && (heartbeat == 0 || (now >= heartbeat && now - heartbeat > timeout));
    pthread_mutex_unlock(&publication_lock);
    START_DICT();
    ADD_U64("schema", 1);
    ADD_U64("queue", queue);
    ADD_U64("lcore", lcore);
    ADD_INT("socket", socket);
    ADD_U64("state", state);
    ADD_U64("heartbeat_ms", heartbeat);
    ADD_U64("heartbeat_age_ms", now >= heartbeat ? now - heartbeat : 0);
    ADD_U64("timeout_ms", timeout);
    ADD_INT("stalled", stalled);
    return 0;
}

/** 同一主线程逐页复制固定版本，期间不会处理下一条管理写入，计数保持原子采样语义 */
static int build_rules(struct dppd_production *p, uint64_t revision, uint32_t *count)
{
    uint64_t after = 0;
    bool more = true;
    *count = 0;
    while (more) {
        struct dppd_flow_state_rule rules[64];
        uint32_t returned;
        uint64_t actual;
        int rc = dppd_flow_store_list(p->flows->store, after, revision, rules, 64, &returned, &actual, &more);
        if (rc != 0)
            return rc;
        if (actual != revision || *count + returned > p->capacity || (more && returned == 0))
            return -EUCLEAN;
        for (uint32_t i = 0; i < returned; ++i) {
            const struct dppd_rule *rule = &rules[i].rule;
            struct production_rule *row = &p->work[(*count)++];
            *row = (struct production_rule){.id = rule->id,
                .generation = rule->generation,
                .table = rules[i].table_key,
                .queue = rules[i].queue,
                .port = rule->install_port_id};
            for (uint16_t j = 0; j < rule->nb_actions; ++j) {
                const struct dppd_action *action = &rule->actions[j];
                if (action->type == DPPD_ACTION_COUNT)
                    row->counted = true;
                if (action->type == DPPD_ACTION_SHARED_COUNT) {
                    row->counted = row->shared = true;
                    row->shared_id = action->conf.shared_count.id;
                    row->shared_generation = action->conf.shared_count.generation;
                }
            }
            after = row->id;
        }
    }
    return 0;
}

/** 每轮最多刷新十六条实际被查询的规则，避免为闲置指标反复扫描整个软件表 */
static void sample_counts(struct dppd_production *p, uint64_t now_ms)
{
    struct production_rule selected[16];
    uint32_t indices[16], count = 0;
    pthread_mutex_lock(&publication_lock);
    for (uint32_t visited = 0; visited < p->count && count < 16 && p->pending_samples != 0; ++visited) {
        uint32_t i = p->sample_cursor++ % p->count;
        if (!p->rows[i].requested)
            continue;
        p->rows[i].requested = false;
        --p->pending_samples;
        selected[count] = p->rows[i];
        indices[count++] = i;
    }
    pthread_mutex_unlock(&publication_lock);
    for (uint32_t i = 0; i < count; ++i) {
        struct production_rule *row = &selected[i];
        row->count_error = row->shared
            ? dppd_flow_store_query_shared(p->flows->store, row->shared_id, row->shared_generation, &row->hits,
                  &row->bytes)
            : dppd_flow_store_query_count(p->flows->store, row->id, row->generation, &row->hits, &row->bytes);
        pthread_mutex_lock(&publication_lock);
        /** 单写入者保证索引仍指向同一版本，仅更新计数，不覆盖查询线程的新采样意愿 */
        struct production_rule *destination = &p->rows[indices[i]];
        destination->hits = row->hits;
        destination->bytes = row->bytes;
        destination->count_error = row->count_error;
        destination->sampled_ms = now_ms;
        pthread_mutex_unlock(&publication_lock);
    }
}

int dppd_production_poll(struct dppd_production *p, uint64_t now_ms)
{
    if (p == NULL)
        return -EINVAL;
    struct dppd_management_request request = {.version = DPPD_MANAGEMENT_VERSION,
        .operation = DPPD_MANAGEMENT_HEALTH_GET,
        .size = sizeof(request)};
    struct dppd_management_response response;
    int rc = dppd_flow_management_handle(p->flows, p->runtime, &request, &response);
    if (rc == 0)
        rc = response.status;
    struct dppd_flow_store_status status;
    if (rc == 0)
        rc = dppd_flow_store_status(p->flows->store, &status);
    if (rc != 0)
        return rc;
    uint64_t interval = p->runtime->config.stats_period_ms == 0 ? 1000U : p->runtime->config.stats_period_ms;
    bool refresh = !p->published || p->revision != status.service.revision || p->snapshot_error != 0;
    int snapshot_error = status.ready ? 0 : -EAGAIN;
    uint32_t count = 0;
    if (refresh && snapshot_error == 0)
        snapshot_error = build_rules(p, status.service.revision, &count);
    bool changed = !p->published || p->health.ready != response.payload.health.ready ||
        p->health.blockers != response.payload.health.blockers || p->status.last_error != status.last_error;
    if (snapshot_error != 0 && snapshot_error != -EAGAIN && snapshot_error != p->snapshot_error)
        dppd_production_log(DPPD_PRODUCTION_SNAPSHOT_FAILED, snapshot_error, status.service.revision);
    pthread_mutex_lock(&publication_lock);
    if (refresh && snapshot_error == 0) {
        struct production_rule *old = p->rows;
        p->rows = p->work;
        p->work = old;
        p->count = count;
        p->pending_samples = 0;
        p->revision = status.service.revision;
        p->refreshed_ms = now_ms;
    }
    p->health = response.payload.health;
    p->status = status;
    p->sampled_ms = now_ms;
    p->snapshot_error = snapshot_error;
    p->monitored = p->runtime->workers_started &&
        !atomic_load_explicit(&p->runtime->stop_requested, memory_order_acquire) &&
        !dppd_devices_io_quiesced(&p->runtime->devices);
    p->published = true;
    pthread_mutex_unlock(&publication_lock);
    if (snapshot_error == 0)
        sample_counts(p, now_ms);
    if (changed)
        dppd_production_log(p->health.ready ? DPPD_PRODUCTION_READY : DPPD_PRODUCTION_ISOLATED, status.last_error,
            status.service.revision);
    if (p->runtime->config.stats_period_ms != 0 && now_ms - p->logged_ms >= interval) {
        struct dppd_stats_values stats;
        dppd_runtime_stats_read(p->runtime, &stats);
        char line[512];
        int length = snprintf(line, sizeof(line),
            "{\"schema\":1,\"component\":\"dppd\",\"event\":\"stats\",\"monotonic_ms\":%" PRIu64
            ",\"revision\":%" PRIu64 ",\"rx_packets\":%" PRIu64 ",\"tx_packets\":%" PRIu64 ",\"rule_drops\":%" PRIu64
            "}\n",
            now_ms, status.service.revision, stats.rx_packets, stats.tx_packets, stats.rule_drops);
        emit_log(line, length, sizeof(line));
        p->logged_ms = now_ms;
    }
    return 0;
}

int dppd_production_start(struct dppd_runtime *runtime, struct dppd_flow_management *flows,
    struct dppd_production **output)
{
    static const struct {
        const char *name, *help;
        telemetry_cb callback;
    } commands[] = {
        {"/dppd/managed/status", "Managed health and persistence. No parameters.", status_callback},
        {"/dppd/managed/stats", "Packet counters. Optional: port,queue; 65535 selects all.", stats_callback},
        {"/dppd/managed/rule", "Published rule and COUNT sample. Required: rule_id.", rule_callback},
        {"/dppd/managed/worker", "Worker heartbeat and NUMA placement. Required: queue_id.", worker_callback},
    };
    if (runtime == NULL || flows == NULL || flows->store == NULL || output == NULL || *output != NULL)
        return -EINVAL;
    pthread_mutex_lock(&publication_lock);
    bool occupied = active != NULL;
    pthread_mutex_unlock(&publication_lock);
    if (occupied)
        return -EALREADY;
    struct dppd_flow_state_config config;
    int rc = dppd_flow_store_config(flows->store, &config);
    if (rc != 0)
        return rc;
    struct dppd_production *p = calloc(1, sizeof(*p));
    if (p == NULL)
        return -ENOMEM;
    p->runtime = runtime;
    p->flows = flows;
    p->capacity = config.rule_capacity;
    p->rows = calloc(p->capacity, sizeof(*p->rows));
    p->work = calloc(p->capacity, sizeof(*p->work));
    rc = p->rows == NULL || p->work == NULL ? -ENOMEM : dppd_production_poll(p, dppd_monotonic_ms());
    while (rc == 0 && registered < sizeof(commands) / sizeof(commands[0])) {
        rc = rte_telemetry_register_cmd(commands[registered].name, commands[registered].callback,
            commands[registered].help);
        if (rc == 0)
            ++registered;
    }
    if (rc != 0) {
        free(p->rows);
        free(p->work);
        free(p);
        return rc;
    }
    pthread_mutex_lock(&publication_lock);
    active = p;
    pthread_mutex_unlock(&publication_lock);
    *output = p;
    dppd_production_log(DPPD_PRODUCTION_START, 0, p->revision);
    for (uint16_t i = 0; i < runtime->nb_workers; ++i) {
        char line[512];
        int length = snprintf(line, sizeof(line),
            "{\"schema\":1,\"component\":\"dppd\",\"event\":\"worker_layout\",\"queue\":%u,"
            "\"lcore\":%u,\"socket\":%d,\"timeout_ms\":%u}\n",
            runtime->workers[i].queue_id, runtime->workers[i].lcore_id, runtime->workers[i].socket_id,
            runtime->config.worker_timeout_ms);
        emit_log(line, length, sizeof(line));
    }
    return 0;
}

void dppd_production_stop(struct dppd_production **output)
{
    if (output == NULL || *output == NULL)
        return;
    struct dppd_production *p = *output;
    pthread_mutex_lock(&publication_lock);
    if (active == p)
        active = NULL;
    pthread_mutex_unlock(&publication_lock);
    dppd_production_log(DPPD_PRODUCTION_STOP, p->status.last_error, p->status.service.revision);
    free(p->rows);
    free(p->work);
    free(p);
    *output = NULL;
}
