#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rte_telemetry.h>
#include "dppd/control.h"
#include "dppd/runtime.h"
#include "dppd/telemetry.h"

/** 保存生产代码实际注册的回调，用真实 DPDK 数据容器验证输出，不启动额外服务进程 */
static telemetry_cb callbacks[4];
static unsigned int registrations, register_calls, fail_register = 2;
static _Thread_local unsigned int fail_allocation, fail_field;
static _Thread_local struct {
    uint64_t publication, repository_generation, generation, operations, succeeded, failed;
    uint64_t total, returned, ids[64];
    unsigned int id_count;
    int more;
    struct rte_tel_data *container;
} output;
static pthread_mutex_t barrier_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t barrier_condition = PTHREAD_COND_INITIALIZER;
static bool block_create, create_entered, release_create;
static bool block_stats, stats_entered, release_stats;
static atomic_bool stop_reader, detach_entered, detached;
static atomic_uint samples;

void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size);
int __wrap_rte_telemetry_register_cmd(const char *name, telemetry_cb callback, const char *help);
int __real_rte_tel_data_add_dict_u64(struct rte_tel_data *data, const char *name, uint64_t value);
int __wrap_rte_tel_data_add_dict_u64(struct rte_tel_data *data, const char *name, uint64_t value);
int __real_rte_tel_data_add_dict_int(struct rte_tel_data *data, const char *name, int value);
int __wrap_rte_tel_data_add_dict_int(struct rte_tel_data *data, const char *name, int value);
int __real_rte_tel_data_add_array_u64(struct rte_tel_data *data, uint64_t value);
int __wrap_rte_tel_data_add_array_u64(struct rte_tel_data *data, uint64_t value);
int __real_rte_tel_data_add_dict_container(struct rte_tel_data *data, const char *name,
    struct rte_tel_data *value, int keep);
int __wrap_rte_tel_data_add_dict_container(struct rte_tel_data *data, const char *name,
    struct rte_tel_data *value, int keep);
void __wrap_dppd_runtime_stats_read(const struct dppd_runtime *runtime, struct dppd_stats_values *stats);

/** 分配故障只影响调用线程，后台读者的 DPDK 容器仍由共享库正常分配 */
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_allocation != 0 && --fail_allocation == 0)
        return NULL;
    return __real_calloc(count, size);
}

/** 第三条命令首次注册失败，用来证明重试不会重复注册前两条已经成功的命令 */
int __wrap_rte_telemetry_register_cmd(const char *name, telemetry_cb callback, const char *help)
{
    static const char *const names[] = {"/dppd/stats", "/dppd/rules", "/dppd/rule", "/dppd/rule_failures"};

    assert(registrations < 4 && strcmp(name, names[registrations]) == 0 && help != NULL);
    if (register_calls++ == fail_register)
        return -ENOMEM;
    callbacks[registrations++] = callback;
    return 0;
}

/** 检查添加字段时的错误能够返回，同时保留真实容器的容量与类型检查 */
int __wrap_rte_tel_data_add_dict_u64(struct rte_tel_data *data, const char *name, uint64_t value)
{
    if (fail_field != 0 && --fail_field == 0)
        return -E2BIG;
#define CAPTURE(field) if (strcmp(name, #field) == 0) output.field = value;
    CAPTURE(publication) CAPTURE(repository_generation) CAPTURE(generation)
    CAPTURE(operations) CAPTURE(succeeded) CAPTURE(failed) CAPTURE(total) CAPTURE(returned)
#undef CAPTURE
    return __real_rte_tel_data_add_dict_u64(data, name, value);
}

/** 分页标志与 ID 数组分别采集，不通过复制一份 JSON 实现来伪造生产输出 */
int __wrap_rte_tel_data_add_dict_int(struct rte_tel_data *data, const char *name, int value)
{
    if (strcmp(name, "has_more") == 0)
        output.more = value;
    return __real_rte_tel_data_add_dict_int(data, name, value);
}

/** 每页最多六十四个 ID，完整无符号整数不得经过浮点数转换 */
int __wrap_rte_tel_data_add_array_u64(struct rte_tel_data *data, uint64_t value)
{
    assert(output.id_count < 64);
    output.ids[output.id_count++] = value;
    return __real_rte_tel_data_add_array_u64(data, value);
}

/** 正式服务会在 JSON 序列化后释放子容器，直接调用回调的测试负责模拟这一释放 */
int __wrap_rte_tel_data_add_dict_container(struct rte_tel_data *data, const char *name,
    struct rte_tel_data *value, int keep)
{
    int rc = __real_rte_tel_data_add_dict_container(data, name, value, keep);

    assert(keep == 0 && strcmp(name, "rule_ids") == 0);
    if (rc == 0)
        output.container = value;
    return rc;
}

/** 把一次统计读取停在生命周期锁内，验证解绑必须等旧借用结束才能返回 */
void __wrap_dppd_runtime_stats_read(const struct dppd_runtime *runtime, struct dppd_stats_values *stats)
{
    assert(runtime != NULL);
    pthread_mutex_lock(&barrier_lock);
    if (block_stats) {
        stats_entered = true;
        pthread_cond_broadcast(&barrier_condition);
        while (!release_stats)
            pthread_cond_wait(&barrier_condition, &barrier_lock);
    }
    pthread_mutex_unlock(&barrier_lock);
    memset(stats, 0, sizeof(*stats));
}

/** 每次回调使用独立容器，线程局部输出防止测试采集本身发生数据竞争 */
static int query(unsigned int command, const char *parameters)
{
    struct rte_tel_data *data = rte_tel_data_alloc();
    int rc;

    assert(data != NULL);
    memset(&output, 0, sizeof(output));
    rc = callbacks[command]("test", parameters, data);
    if (output.container != NULL)
        rte_tel_data_free(output.container);
    rte_tel_data_free(data);
    output.container = NULL;
    return rc;
}

/** 驱动校验成功，创建是否等待由屏障控制，验证查询无需等 PMD 操作结束 */
static int validate(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_error *error)
{
    (void)port; (void)rule; (void)error;
    return 0;
}

/** 暂停真实控制路径的创建调用，查询线程应仍能读到上一份完整发布 */
static int create(uint16_t port, const struct dppd_rule *rule,
    struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    pthread_mutex_lock(&barrier_lock);
    if (block_create) {
        create_entered = true;
        pthread_cond_broadcast(&barrier_condition);
        while (!release_create)
            pthread_cond_wait(&barrier_condition, &barrier_lock);
    }
    pthread_mutex_unlock(&barrier_lock);
    *handle = (struct dppd_flow_handle){.rule_id = rule->id, .rule_generation = rule->generation,
        .port_id = port, .flow = (struct rte_flow *)(uintptr_t)1};
    return 0;
}

/** 无故障清理实际对象，测试退出时不遗留后端槽位 */
static int remove_flow(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    memset(handle, 0, sizeof(*handle));
    return 0;
}

/** 规则 ID 与策略来自参数，其余内容固定以便持续测试版本变化 */
static struct dppd_rule rule(uint64_t id, enum dppd_fallback_policy policy)
{
    struct dppd_rule value = {.id = id, .fallback = policy, .nb_matches = 1, .nb_actions = 1};

    value.matches[0].type = DPPD_MATCH_ETH;
    value.actions[0].type = DPPD_ACTION_DROP;
    return value;
}

/** 读者只访问已注册回调，不读取 control，累计和规则版本必须来自同一完整副本 */
static void *read_snapshots(void *unused)
{
    (void)unused;
    do {
        assert(query(3, NULL) == 0);
        assert(output.operations == output.succeeded + output.failed);
        assert(query(2, "1") == 0);
        assert(output.generation <= output.repository_generation && output.publication > 0);
        assert(query(1, NULL) == 0);
        assert(output.returned == 64 && output.total == 70 && output.more);
        atomic_fetch_add(&samples, 1);
    } while (!atomic_load(&stop_reader));
    return NULL;
}

/** 该线程暂时是 control 的唯一写者，PMD 返回后完成请求并发布副本 */
static void *create_blocked(void *argument)
{
    struct dppd_control_service *control = argument;
    struct dppd_control_apply_result result;
    struct dppd_rule value = rule(500, DPPD_FALLBACK_REQUIRE_HARDWARE);

    assert(dppd_control_apply(control, 5, &value, 0, &result) == 0);
    assert(dppd_telemetry_publish_rules(control) == 0);
    return NULL;
}

/** 统计查询单独执行，主线程等待它进入生命周期保护范围 */
static void *read_blocked_stats(void *unused)
{
    (void)unused;
    assert(query(0, NULL) == 0);
    return NULL;
}

/** 解绑允许与查询并发，但不能与管理线程的注册或发布并发 */
static void *detach_runtime(void *unused)
{
    (void)unused;
    atomic_store(&detach_entered, true);
    dppd_telemetry_unregister_runtime();
    atomic_store(&detached, true);
    return NULL;
}

/** 覆盖分页、严格参数、失败重试、并发完整性、慢驱动查询和退出生命周期 */
int main(void)
{
    static struct dppd_runtime runtime;
    struct dppd_control_service control;
    const struct dppd_topology topology = {.nb_endpoints = 1, .endpoints = {{.ethdev_port_id = 5}}};
    const struct dppd_flow_api api = {.validate = validate, .create = create, .remove = remove_flow};
    struct dppd_control_apply_result result;
    pthread_t reader, writer, detacher;
    uint64_t publication, operations;
    char parameters[64];

    assert(dppd_control_init(&control, &topology, 72, &api) == 0);
    for (uint64_t id = 1; id <= 70; ++id) {
        struct dppd_rule value = rule(id == 70 ? UINT64_MAX : id, DPPD_FALLBACK_SOFTWARE_ONLY);
        assert(dppd_control_apply(&control, 5, &value, 0, &result) == 0);
    }
    for (unsigned int allocation = 1; allocation <= 2; ++allocation) {
        fail_allocation = allocation;
        assert(dppd_telemetry_register(&runtime, &control) == -ENOMEM);
        assert(register_calls == 0 && fail_allocation == 0);
    }
    assert(dppd_telemetry_register(&runtime, &control) == -ENOMEM);
    assert(registrations == 2 && query(1, NULL) == -EAGAIN);
    assert(dppd_telemetry_register(&runtime, &control) == 0 && registrations == 4 && register_calls == 5);
    assert(dppd_telemetry_register(&runtime, &control) == -EALREADY);
    assert(query(1, NULL) == 0 && output.id_count == 64 && output.ids[63] == 64 && output.more);
    publication = output.publication;
    assert(query(1, "64,70") == 0 && output.id_count == 6 && !output.more);
    assert(output.ids[5] == UINT64_MAX && output.repository_generation == 70);
    assert(query(2, "18446744073709551615") == 0 && output.generation == 70);
    const char *invalid[] = {"-1", " 1", "1,", "1,2,3", "18446744073709551616", "1,18446744073709551615"};
    for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(query(1, invalid[i]) == -EINVAL);
    assert(query(1, "64,69") == -ESTALE);
    assert(query(2, NULL) == -EINVAL && query(2, "0") == -EINVAL && query(2, "1,2") == -EINVAL);
    assert(query(2, "999") == -ENOENT && query(3, "1") == -EINVAL && query(0, "1") == -EINVAL);
    fail_field = 1;
    assert(query(1, NULL) == -E2BIG);
    fail_allocation = 1;
    assert(dppd_telemetry_publish_rules(&control) == 0 && fail_allocation == 1);
    fail_allocation = 0;
    assert(query(1, NULL) == 0 && output.publication == publication);

    /** 反复整批更新两条软件规则，读者同时检查完整发布的累计关系和版本边界 */
    assert(pthread_create(&reader, NULL, read_snapshots, NULL) == 0);
    while (atomic_load(&samples) == 0)
        sched_yield();
    for (unsigned int iteration = 0; iteration < 300; ++iteration) {
        struct dppd_control_batch_update_request requests[2] = {0};
        struct dppd_control_apply_result results[2];

        for (unsigned int index = 0; index < 2; ++index) {
            assert(dppd_rule_repository_get(&control.rules, index + 1, &requests[index].rule) == 0);
            requests[index].expected_generation = requests[index].rule.generation;
            requests[index].install_port_id = 5;
            requests[index].rule.priority++;
        }
        assert(dppd_control_update_batch(&control, requests, 2, results) == 0);
        assert(dppd_telemetry_publish_rules(&control) == 0);
    }
    atomic_store(&stop_reader, true);
    assert(pthread_join(reader, NULL) == 0 && atomic_load(&samples) > 0);
    snprintf(parameters, sizeof(parameters), "64,%" PRIu64, control.rules.generation - 1);
    assert(query(1, parameters) == -ESTALE);
    assert(query(3, NULL) == 0);
    operations = output.operations;
    block_create = true;
    assert(pthread_create(&writer, NULL, create_blocked, &control) == 0);
    pthread_mutex_lock(&barrier_lock);
    while (!create_entered)
        pthread_cond_wait(&barrier_condition, &barrier_lock);
    pthread_mutex_unlock(&barrier_lock);
    assert(query(3, NULL) == 0 && output.operations == operations);
    assert(query(1, NULL) == 0 && output.total == 70);
    assert(query(2, "500") == -ENOENT);
    pthread_mutex_lock(&barrier_lock);
    release_create = true;
    pthread_cond_broadcast(&barrier_condition);
    pthread_mutex_unlock(&barrier_lock);
    assert(pthread_join(writer, NULL) == 0);
    assert(query(2, "500") == 0 && query(3, NULL) == 0 && output.operations == operations + 1);

    /** 退出解绑必须等正在借用 runtime 的统计查询离开，随后所有回调只能报告不可用 */
    block_stats = true;
    assert(pthread_create(&reader, NULL, read_blocked_stats, NULL) == 0);
    pthread_mutex_lock(&barrier_lock);
    while (!stats_entered)
        pthread_cond_wait(&barrier_condition, &barrier_lock);
    pthread_mutex_unlock(&barrier_lock);
    assert(pthread_create(&detacher, NULL, detach_runtime, NULL) == 0);
    while (!atomic_load(&detach_entered))
        sched_yield();
    assert(!atomic_load(&detached));
    pthread_mutex_lock(&barrier_lock);
    release_stats = true;
    pthread_cond_broadcast(&barrier_condition);
    pthread_mutex_unlock(&barrier_lock);
    assert(pthread_join(reader, NULL) == 0 && pthread_join(detacher, NULL) == 0);
    for (unsigned int callback = 0; callback < 4; ++callback)
        assert(query(callback, callback == 2 ? "1" : NULL) == -EAGAIN);
    dppd_telemetry_unregister_runtime();
    assert(dppd_control_fini(&control) == 0);
    return 0;
}
