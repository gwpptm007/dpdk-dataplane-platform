#define ALLOW_EXPERIMENTAL_API
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <rte_byteorder.h>
#include <rte_flow.h>
#include <rte_version.h>
#include <rte_errno.h>
#include "dppd/flow_service.h"
#include "dppd/flow_state.h"

struct model_counter {
    uint64_t hits, bytes;
};

static unsigned int counter_live;
static struct model_counter *last_counter;
static bool fail_counter_create, fail_counter_destroy;

struct rte_flow_action_handle *__wrap_rte_flow_action_handle_create(uint16_t port,
    const struct rte_flow_indir_action_conf *conf, const struct rte_flow_action *action, struct rte_flow_error *error);
int __wrap_rte_flow_action_handle_query(uint16_t port, const struct rte_flow_action_handle *handle, void *data,
    struct rte_flow_error *error);
int __wrap_rte_flow_action_handle_destroy(uint16_t port, struct rte_flow_action_handle *handle,
    struct rte_flow_error *error);

struct rte_flow_action_handle *__wrap_rte_flow_action_handle_create(uint16_t port,
    const struct rte_flow_indir_action_conf *conf, const struct rte_flow_action *action, struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && conf->ingress && !conf->egress && !conf->transfer);
    if (fail_counter_create) {
        rte_errno = ENOMEM;
        return NULL;
    }
    assert(action->type == RTE_FLOW_ACTION_TYPE_COUNT && ((const struct rte_flow_action_count *)action->conf)->id == 0);
    last_counter = calloc(1, sizeof(*last_counter));
    assert(last_counter != NULL);
    ++counter_live;
    return (struct rte_flow_action_handle *)last_counter;
}

int __wrap_rte_flow_action_handle_query(uint16_t port, const struct rte_flow_action_handle *handle, void *data,
    struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7);
    const struct model_counter *counter = (const struct model_counter *)handle;
    struct rte_flow_query_count *query = data;
    assert(!query->reset);
    query->hits_set = query->bytes_set = 1;
    query->hits = counter->hits;
    query->bytes = counter->bytes;
    return 0;
}

int __wrap_rte_flow_action_handle_destroy(uint16_t port, struct rte_flow_action_handle *handle,
    struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && counter_live != 0);
    if (fail_counter_destroy)
        return -EIO;
    --counter_live;
    free(handle);
    return 0;
}

#if RTE_VERSION >= RTE_VERSION_NUM(24, 11, 0, 0)
struct model_flow {
    struct model_counter *counter;
    const struct rte_flow_action_age *age;
    bool completed;
};

struct model_completion {
    void *tag;
    struct model_flow *flow;
    bool destroy, failure;
};

static struct model_flow *flows[128];
static struct model_completion completions[128];
static unsigned int flow_count, completion_count, ready;
static bool emit_age, fail_create_completion, fail_destroy_completion, fail_table_destroy;
static int pattern_object, actions_object, table_object;
static struct model_counter *expected_counter;
static bool restoring_store, deny_empty;

int __wrap_rte_flow_info_get(uint16_t port, struct rte_flow_port_info *info, struct rte_flow_queue_info *queue,
    struct rte_flow_error *error);
int __wrap_rte_flow_configure(uint16_t port, const struct rte_flow_port_attr *attr, uint16_t queues,
    const struct rte_flow_queue_attr *attributes[], struct rte_flow_error *error);
struct rte_flow_pattern_template *__wrap_rte_flow_pattern_template_create(uint16_t port,
    const struct rte_flow_pattern_template_attr *attr, const struct rte_flow_item pattern[],
    struct rte_flow_error *error);
struct rte_flow_actions_template *__wrap_rte_flow_actions_template_create(uint16_t port,
    const struct rte_flow_actions_template_attr *attr, const struct rte_flow_action actions[],
    const struct rte_flow_action masks[], struct rte_flow_error *error);
struct rte_flow_template_table *__wrap_rte_flow_template_table_create(uint16_t port,
    const struct rte_flow_template_table_attr *attr, struct rte_flow_pattern_template *patterns[], uint8_t np,
    struct rte_flow_actions_template *actions[], uint8_t na, struct rte_flow_error *error);
int __wrap_rte_flow_template_table_destroy(uint16_t port, struct rte_flow_template_table *table,
    struct rte_flow_error *error);
int __wrap_rte_flow_actions_template_destroy(uint16_t port, struct rte_flow_actions_template *actions,
    struct rte_flow_error *error);
int __wrap_rte_flow_pattern_template_destroy(uint16_t port, struct rte_flow_pattern_template *pattern,
    struct rte_flow_error *error);
struct rte_flow *__wrap_rte_flow_async_create(uint16_t port, uint32_t queue, const struct rte_flow_op_attr *attr,
    struct rte_flow_template_table *table, const struct rte_flow_item pattern[], uint8_t pi,
    const struct rte_flow_action actions[], uint8_t ai, void *tag, struct rte_flow_error *error);
int __wrap_rte_flow_async_destroy(uint16_t port, uint32_t queue, const struct rte_flow_op_attr *attr,
    struct rte_flow *flow, void *tag, struct rte_flow_error *error);
int __wrap_rte_flow_push(uint16_t port, uint32_t queue, struct rte_flow_error *error);
int __wrap_rte_flow_pull(uint16_t port, uint32_t queue, struct rte_flow_op_result results[], uint16_t count,
    struct rte_flow_error *error);
int __wrap_rte_flow_get_q_aged_flows(uint16_t port, uint32_t queue, void **contexts, uint32_t count,
    struct rte_flow_error *error);

int __wrap_rte_flow_info_get(uint16_t port, struct rte_flow_port_info *info, struct rte_flow_queue_info *queue,
    struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7);
    *info = (struct rte_flow_port_info){.max_nb_queues = 1,
        .max_nb_counters = 2,
        .max_nb_aging_objects = 128,
        .supported_flags = RTE_FLOW_PORT_FLAG_STRICT_QUEUE};
    queue->max_size = 64;
    return 0;
}

int __wrap_rte_flow_configure(uint16_t port, const struct rte_flow_port_attr *attr, uint16_t queues,
    const struct rte_flow_queue_attr *attributes[], struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && queues == 1 && attributes[0]->size == 64 && attr->nb_counters == 2);
    assert(attr->nb_aging_objects == 128 && attr->flags == RTE_FLOW_PORT_FLAG_STRICT_QUEUE);
    return 0;
}

struct rte_flow_pattern_template *__wrap_rte_flow_pattern_template_create(uint16_t port,
    const struct rte_flow_pattern_template_attr *attr, const struct rte_flow_item pattern[],
    struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && attr->ingress && !attr->egress && !attr->transfer);
    assert(pattern[0].type == RTE_FLOW_ITEM_TYPE_ETH && pattern[1].type == RTE_FLOW_ITEM_TYPE_IPV4);
    assert(((const struct rte_flow_item_ipv4 *)pattern[1].mask)->hdr.src_addr == UINT32_MAX);
    return (struct rte_flow_pattern_template *)&pattern_object;
}

struct rte_flow_actions_template *__wrap_rte_flow_actions_template_create(uint16_t port,
    const struct rte_flow_actions_template_attr *attr, const struct rte_flow_action actions[],
    const struct rte_flow_action masks[], struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && attr->ingress);
    if (restoring_store)
        expected_counter = last_counter;
    /** mask 必须写真实 COUNT 类型，句柄位置可变，不能把逻辑 ID 或临时指针地址作为 INDIRECT conf */
    assert(actions[0].type == RTE_FLOW_ACTION_TYPE_INDIRECT && actions[0].conf == expected_counter);
    assert(masks[0].type == RTE_FLOW_ACTION_TYPE_COUNT && masks[0].conf == NULL);
    assert(actions[1].type == RTE_FLOW_ACTION_TYPE_AGE && actions[2].type == RTE_FLOW_ACTION_TYPE_DROP);
    return (struct rte_flow_actions_template *)&actions_object;
}

struct rte_flow_template_table *__wrap_rte_flow_template_table_create(uint16_t port,
    const struct rte_flow_template_table_attr *attr, struct rte_flow_pattern_template *patterns[], uint8_t np,
    struct rte_flow_actions_template *actions[], uint8_t na, struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && np == 1 && na == 1 && attr->flow_attr.group == 1 && attr->nb_flows == 128);
    assert(patterns[0] == (struct rte_flow_pattern_template *)&pattern_object);
    assert(actions[0] == (struct rte_flow_actions_template *)&actions_object);
    return (struct rte_flow_template_table *)&table_object;
}

int __wrap_rte_flow_template_table_destroy(uint16_t port, struct rte_flow_template_table *table,
    struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && table == (struct rte_flow_template_table *)&table_object);
    return fail_table_destroy ? -EIO : 0;
}

int __wrap_rte_flow_actions_template_destroy(uint16_t port, struct rte_flow_actions_template *actions,
    struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && actions == (struct rte_flow_actions_template *)&actions_object);
    return 0;
}

int __wrap_rte_flow_pattern_template_destroy(uint16_t port, struct rte_flow_pattern_template *pattern,
    struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && pattern == (struct rte_flow_pattern_template *)&pattern_object);
    return 0;
}

struct rte_flow *__wrap_rte_flow_async_create(uint16_t port, uint32_t queue, const struct rte_flow_op_attr *attr,
    struct rte_flow_template_table *table, const struct rte_flow_item pattern[], uint8_t pi,
    const struct rte_flow_action actions[], uint8_t ai, void *tag, struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && queue == 0 && attr->postpone && pi == 0 && ai == 0 && tag != NULL);
    assert(table == (struct rte_flow_template_table *)&table_object && pattern[1].type == RTE_FLOW_ITEM_TYPE_IPV4);
    assert(actions[0].type == RTE_FLOW_ACTION_TYPE_INDIRECT && actions[0].conf == expected_counter);
    struct model_flow *flow = calloc(1, sizeof(*flow));
    assert(flow != NULL && flow_count < 128 && completion_count < 128);
    flow->counter = (struct model_counter *)actions[0].conf;
    flow->age = actions[1].conf;
    assert(actions[1].type == RTE_FLOW_ACTION_TYPE_AGE && flow->age->context == tag);
    flows[flow_count++] = flow;
    completions[completion_count++] = (struct model_completion){tag, flow, false, fail_create_completion};
    fail_create_completion = false;
    return (struct rte_flow *)flow;
}

int __wrap_rte_flow_async_destroy(uint16_t port, uint32_t queue, const struct rte_flow_op_attr *attr,
    struct rte_flow *handle, void *tag, struct rte_flow_error *error)
{
    (void)error;
    struct model_flow *flow = (struct model_flow *)handle;
    assert(port == 7 && queue == 0 && attr->postpone && flow->completed && completion_count < 128);
    completions[completion_count++] = (struct model_completion){tag, flow, true, fail_destroy_completion};
    fail_destroy_completion = false;
    return 0;
}

int __wrap_rte_flow_push(uint16_t port, uint32_t queue, struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && queue == 0);
    ready = completion_count;
    return 0;
}

int __wrap_rte_flow_pull(uint16_t port, uint32_t queue, struct rte_flow_op_result results[], uint16_t count,
    struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && queue == 0);
    unsigned int copied = 0;
    while (ready != 0 && copied < count) {
        struct model_completion op = completions[--completion_count];
        --ready;
        assert(op.flow->age->context != NULL);
        results[copied++] = (struct rte_flow_op_result){.user_data = op.tag,
            .status = op.failure ? RTE_FLOW_OP_ERROR : RTE_FLOW_OP_SUCCESS};
        if (op.destroy && !op.failure) {
            unsigned int i;
            for (i = 0; i < flow_count; ++i)
                if (flows[i] == op.flow)
                    break;
            assert(i < flow_count);
            flows[i] = flows[--flow_count];
            free(op.flow);
        } else if (!op.destroy) {
            op.flow->completed = true;
        }
    }
    return (int)copied;
}

int __wrap_rte_flow_get_q_aged_flows(uint16_t port, uint32_t queue, void **contexts, uint32_t count,
    struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && queue == 0);
    if (!emit_age)
        return 0;
    assert(flow_count <= count);
    for (unsigned int i = 0; i < flow_count; ++i)
        contexts[i] = flows[i]->age->context;
    emit_age = false;
    return (int)flow_count;
}

static struct dppd_flow_job_result finish(struct dppd_flow_service *service, const struct dppd_flow_job_id *job,
    uint64_t now_ns)
{
    struct dppd_flow_job_result result;
    for (unsigned int i = 0; i < 64; ++i) {
        dppd_flow_service_process(service, now_ns, 64);
        int rc = dppd_flow_service_collect(service, job, &result);
        if (rc == 0)
            return result;
        assert(rc == -EAGAIN);
    }
    assert(!"incomplete native batch");
    return result;
}

static int verify_empty(void *context, uint16_t port)
{
    (void)context;
    assert(port == 7);
    return deny_empty || counter_live != 0 || flow_count != 0 ? -EUCLEAN : 0;
}

/** 正式原生工厂按旧代次重建共享对象和模板，AGE 删除后配置也必须落盘，不能只清内存 */
static void test_native_store_restart(void)
{
    char directory[] = "/tmp/dppd-native-store-XXXXXX", path[256], lock[270];
    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/state", directory);
    snprintf(lock, sizeof(lock), "%s.lock", path);
    struct dppd_flow_state *state = calloc(1, sizeof(*state)), *loaded = NULL;
    assert(state != NULL);
    state->config = (struct dppd_flow_state_config){.mode = DPPD_FLOW_STATE_NATIVE,
        .port_id = 7,
        .rule_capacity = 64,
        .queue_count = 1,
        .depth = 64,
        .queue_capacity = {128},
        .template_leases = 256,
        .shared_leases = 512,
        .native_counters = 2,
        .native_aging = 128,
        .timeout_ns = UINT64_C(1000000000)};
    state->config.admission = (struct dppd_admission_config){.ticket_capacity = 320, .port_count = 1};
    state->config.admission.ports[0].port_id = 7;
    for (unsigned int k = 0; k < DPPD_RESOURCE_KIND_COUNT; ++k)
        state->config.admission.limits[k] = state->config.admission.ports[0].limits[k] = 128;
    state->config.admission.limits[DPPD_RESOURCE_COUNTER] =
        state->config.admission.ports[0].limits[DPPD_RESOURCE_COUNTER] = 2;
    state->shared_count = 2;
    state->counts[0] = (struct dppd_shared_count_spec){700, 9, 7, DPPD_RULE_DOMAIN_INGRESS};
    state->counts[1] = (struct dppd_shared_count_spec){800, 3, 7, DPPD_RULE_DOMAIN_INGRESS};
    state->shared_next_generation = 20;
    state->table_count = 1;
    state->tables[0].key = 100;
    state->tables[0].spec.capacity = 128;
    state->tables[0].spec.dynamic_actions = 1;
    struct dppd_rule *example = &state->tables[0].spec.example;
    *example = (struct dppd_rule){.id = 1, .install_port_id = 7, .group = 1, .nb_matches = 2, .nb_actions = 3};
    example->matches[0].type = DPPD_MATCH_ETH;
    example->matches[1].type = DPPD_MATCH_IPV4;
    example->matches[1].spec.ipv4.src_mask_be = UINT32_MAX;
    example->actions[0] = (struct dppd_action){.type = DPPD_ACTION_SHARED_COUNT, .conf.shared_count = {700, 9}};
    example->actions[1] = (struct dppd_action){.type = DPPD_ACTION_AGE, .conf.age_timeout_seconds = 1};
    example->actions[2].type = DPPD_ACTION_DROP;
    state->rule_count = 2;
    state->revision = 100;
    state->rules = calloc(2, sizeof(*state->rules));
    assert(state->rules != NULL);
    for (unsigned int i = 0; i < 2; ++i) {
        state->rules[i] = (struct dppd_flow_state_rule){100, 0, *example};
        state->rules[i].rule.id = i + 11U;
        state->rules[i].rule.generation = i == 0 ? 42 : 71;
    }
    assert(dppd_flow_state_save(path, state) == 0);
    struct dppd_flow_store_runtime runtime = {.verify_empty = verify_empty};
    struct dppd_flow_store *store = NULL, *other = NULL;
    uint64_t hits, bytes;
    restoring_store = true;
    assert(dppd_flow_store_open(path, NULL, &store) == -ENOTSUP && store == NULL && counter_live == 0);
    deny_empty = true;
    assert(dppd_flow_store_open(path, &runtime, &store) == -EUCLEAN && store == NULL && counter_live == 0);
    deny_empty = false;
    assert(dppd_flow_store_open(path, &runtime, &store) == 0 && flow_count == 2 && counter_live == 2);
    assert(dppd_flow_store_query_shared(store, 700, 9, &hits, &bytes) == 0 && hits == 0 && bytes == 0);
    assert(dppd_flow_store_query_shared(store, 700, 3, &hits, &bytes) == -ESTALE);
    for (unsigned int i = 0; i < flow_count; ++i) {
        flows[i]->counter->hits += 2;
        flows[i]->counter->bytes += 128;
    }
    assert(dppd_flow_store_query_shared(store, 700, 9, &hits, &bytes) == 0 && hits == 4 && bytes == 256);
    assert(dppd_flow_store_close(&store) == 0 && counter_live == 0 && flow_count == 0);
    assert(dppd_flow_store_open(path, &runtime, &store) == 0 && flow_count == 2);
    assert(dppd_flow_store_query_shared(store, 700, 9, &hits, &bytes) == 0 && hits == 0 && bytes == 0);
    emit_age = true;
    for (unsigned int i = 0; i < 16; ++i)
        assert(dppd_flow_store_process(store, UINT64_C(200000000), 64) == 0);
    assert(flow_count == 0);
    assert(dppd_flow_state_load(path, &loaded) == 0 && loaded->rule_count == 0 && loaded->revision == 102);
    assert(
        loaded->shared_next_generation == 20 && loaded->counts[0].generation == 9 && loaded->counts[1].generation == 3);
    dppd_flow_state_free(&loaded);
    /** 在线定义继续使用正式原生工厂，清理失败必须保留模板借用和计数器句柄 */
    struct dppd_flow_config_result configured;
    struct dppd_flow_config_change changes[2] = {
        {.operation = DPPD_FLOW_CONFIG_COUNT_DELETE, .key = 800, .expected_generation = 3}};
    assert(dppd_flow_store_configure(store, 102, changes, 1, &configured) == 0 && counter_live == 1);
    changes[0] = (struct dppd_flow_config_change){.operation = DPPD_FLOW_CONFIG_COUNT_ADD,
        .key = 900,
        .spec.count = {7, DPPD_RULE_DOMAIN_INGRESS}};
    changes[1] = (struct dppd_flow_config_change){.operation = DPPD_FLOW_CONFIG_TEMPLATE_ADD, .key = 101};
    changes[1].spec.table = state->tables[0].spec;
    changes[1].spec.table.example.actions[0].conf.shared_count.id = 900;
    changes[1].spec.table.example.actions[0].conf.shared_count.generation = 0;
    fail_counter_create = true;
    assert(dppd_flow_store_configure(store, 103, changes, 2, &configured) == -ENOMEM && !configured.applied);
    fail_counter_create = false;
    assert(dppd_flow_store_configure(store, 103, changes, 2, &configured) == 0 && counter_live == 2);
    uint64_t new_generation = configured.generations[0];
    assert(new_generation == 21);
    changes[0] = (struct dppd_flow_config_change){.operation = DPPD_FLOW_CONFIG_COUNT_DELETE,
        .key = 900,
        .expected_generation = new_generation};
    assert(dppd_flow_store_configure(store, 104, changes, 1, &configured) == -EBUSY);
    changes[1] = (struct dppd_flow_config_change){.operation = DPPD_FLOW_CONFIG_TEMPLATE_DELETE, .key = 101};
    fail_table_destroy = true;
    assert(dppd_flow_store_configure(store, 104, changes, 2, &configured) == -EUCLEAN && configured.applied &&
        configured.durable);
    assert(counter_live == 2);
    fail_table_destroy = false;
    fail_counter_destroy = true;
    assert(dppd_flow_store_retry(store) == -EUCLEAN && counter_live == 2);
    fail_counter_destroy = false;
    assert(dppd_flow_store_retry(store) == 0 && counter_live == 1);
    assert(dppd_flow_store_close(&store) == 0 && counter_live == 0);
    assert(dppd_flow_state_save(path, state) == 0);
    fail_create_completion = fail_table_destroy = true;
    assert(dppd_flow_store_open(path, &runtime, &store) == -EUCLEAN && store != NULL && flow_count == 0 &&
        counter_live == 2);
    assert(dppd_flow_store_open(path, &runtime, &other) == -EBUSY && other == NULL);
    fail_table_destroy = false;
    assert(dppd_flow_store_close(&store) == 0 && counter_live == 0);
    assert(dppd_flow_store_open(path, &runtime, &store) == 0 && dppd_flow_store_close(&store) == 0);
    restoring_store = false;
    dppd_flow_state_free(&state);
    unlink(path);
    unlink(lock);
    assert(rmdir(directory) == 0);
}
#endif

int main(void)
{
    struct dppd_admission_config limits = {.ticket_capacity = 320, .port_count = 1};
    struct dppd_admission *admission = NULL;
    struct dppd_flow_shared_counts *counts = NULL;
    struct dppd_flow_templates *templates = NULL;
    struct dppd_flow_shared_count_info a, b;
    struct dppd_flow_template_id table;
    uint64_t hits, bytes;
    limits.ports[0].port_id = 7;
    for (unsigned int k = 0; k < DPPD_RESOURCE_KIND_COUNT; ++k)
        limits.limits[k] = limits.ports[0].limits[k] = 128;
    limits.limits[DPPD_RESOURCE_COUNTER] = limits.ports[0].limits[DPPD_RESOURCE_COUNTER] = 2;
    assert(dppd_admission_create(&limits, &admission) == 0);
#if RTE_VERSION >= RTE_VERSION_NUM(24, 11, 0, 0)
    assert(dppd_native_flow_configure(7, 1, 64, 2, 128) == 0);
#else
    assert(dppd_native_flow_configure(7, 1, 64, 2, 128) == -ENOTSUP);
#endif
    struct dppd_flow_shared_count_driver counter_driver = dppd_native_flow_shared_count_driver();
    assert(dppd_flow_shared_counts_create(admission, 512, &counter_driver, &counts) == 0);
    assert(dppd_flow_shared_counts_add(counts, UINT64_C(0x100000001), 7, DPPD_RULE_DOMAIN_INGRESS, &a) == 0);
    struct model_counter *counter_a = last_counter;
    assert(dppd_flow_shared_counts_add(counts, UINT64_C(0x200000001), 7, DPPD_RULE_DOMAIN_INGRESS, &b) == 0);
    struct dppd_flow_template_spec shape = {.capacity = 128, .dynamic_actions = 1};
    shape.example = (struct dppd_rule){.id = 1, .install_port_id = 7, .group = 1, .nb_matches = 2, .nb_actions = 3};
    shape.example.matches[0].type = DPPD_MATCH_ETH;
    shape.example.matches[1].type = DPPD_MATCH_IPV4;
    shape.example.matches[1].spec.ipv4.src_mask_be = UINT32_MAX;
    shape.example.actions[0].type = DPPD_ACTION_SHARED_COUNT;
    shape.example.actions[0].conf.shared_count.id = a.spec.id;
    shape.example.actions[0].conf.shared_count.generation = a.spec.generation;
    shape.example.actions[1] = (struct dppd_action){.type = DPPD_ACTION_AGE, .conf.age_timeout_seconds = 1};
    shape.example.actions[2].type = DPPD_ACTION_DROP;
    struct dppd_flow_template_driver template_driver = dppd_native_flow_template_driver_shared(counts);
    assert(dppd_flow_templates_create(admission, 256, &template_driver, &templates) == 0);
#if RTE_VERSION >= RTE_VERSION_NUM(24, 11, 0, 0)
    expected_counter = counter_a;
    assert(dppd_flow_template_create(templates, &shape, &table) == 0);
    struct dppd_async_flow_driver driver = dppd_native_async_flow_driver();
    struct dppd_async_flow_queue *queue = NULL;
    struct dppd_async_flow_config queue_config = {.admission = admission,
        .templates = templates,
        .driver = driver,
        .flow_resource = DPPD_RESOURCE_HARDWARE_FLOW,
        .port_id = 7,
        .depth = 64,
        .flow_capacity = 128};
    assert(dppd_async_flow_queue_create(&queue_config, &queue) == 0);
    struct dppd_flow_service *service = NULL;
    struct dppd_flow_service_config service_config = {.admission = admission,
        .templates = templates,
        .queues = {queue},
        .rule_capacity = 64,
        .queue_count = 1,
        .depth = 64,
        .timeout_ns = UINT64_C(1000000000),
        .port_id = 7};
    assert(dppd_flow_service_create(&service_config, &service) == 0);
    struct dppd_async_flow_create_request requests[64];
    for (unsigned int i = 0; i < 64; ++i) {
        requests[i] = (struct dppd_async_flow_create_request){table, shape.example};
        requests[i].rule.id = i + 1U;
        requests[i].rule.matches[1].spec.ipv4.src_be = rte_cpu_to_be_32(0x0a000001U + i);
        requests[i].rule.actions[0].conf.shared_count.id = b.spec.id;
        requests[i].rule.actions[0].conf.shared_count.generation = b.spec.generation;
    }
    expected_counter = last_counter;
    struct dppd_flow_job_id job;
    assert(dppd_flow_service_submit(service, 0, requests, 64, 1, &job) == 0);
    struct dppd_flow_shared_count_info info;
    assert(dppd_flow_shared_counts_get(counts, b.spec.id, b.spec.generation, &info) == 0 && info.references == 64);
    assert(dppd_flow_shared_counts_remove(counts, b.spec.id, b.spec.generation) == -EBUSY);
    assert(finish(service, &job, 2).applied && flow_count == 64);
    assert(dppd_flow_shared_counts_get(counts, b.spec.id, b.spec.generation, &info) == 0 && info.references == 128);
    for (unsigned int i = 0; i < flow_count; ++i) {
        flows[i]->counter->hits += 2;
        flows[i]->counter->bytes += 128;
    }
    assert(dppd_flow_shared_counts_query(counts, b.spec.id, b.spec.generation, &hits, &bytes) == 0 && hits == 128 &&
        bytes == 8192);
    assert(dppd_flow_shared_counts_query(counts, a.spec.id, a.spec.generation, &hits, &bytes) == 0 && hits == 0 &&
        bytes == 0);
    /** 整批从 B 改绑到 A，旧 pin 在全部旧流删除前持续保护 B，物理计数器额度仍只占两个 */
    struct dppd_flow_update_request updates[64];
    for (unsigned int i = 0; i < 64; ++i) {
        updates[i] = (struct dppd_flow_update_request){i + 1U, requests[i]};
        updates[i].replacement.rule.actions[0].conf.shared_count.id = a.spec.id;
        updates[i].replacement.rule.actions[0].conf.shared_count.generation = a.spec.generation;
    }
    expected_counter = counter_a;
    assert(dppd_flow_service_update(service, 0, updates, 64, 3, &job) == 0);
    assert(dppd_flow_shared_counts_remove(counts, b.spec.id, b.spec.generation) == -EBUSY);
    struct dppd_flow_job_result updated = finish(service, &job, 4);
    if (!updated.applied)
        fprintf(stderr, "shared update status=%d flows=%u\n", updated.status, flow_count);
    assert(updated.applied && flow_count == 64);
    assert(dppd_flow_shared_counts_get(counts, b.spec.id, b.spec.generation, &info) == 0 && info.references == 0);
    assert(dppd_flow_shared_counts_get(counts, a.spec.id, a.spec.generation, &info) == 0 && info.references == 129);
    emit_age = true;
    for (unsigned int i = 0; i < 16; ++i)
        assert(dppd_flow_service_process(service, UINT64_C(200000000), 64) == 0);
    assert(flow_count == 0);
    assert(dppd_flow_shared_counts_get(counts, b.spec.id, b.spec.generation, &info) == 0 && info.references == 0);
    assert(dppd_flow_shared_counts_query(counts, b.spec.id, b.spec.generation, &hits, &bytes) == 0 && hits == 128 &&
        bytes == 8192);
    assert(dppd_flow_shared_counts_get(counts, a.spec.id, a.spec.generation, &info) == 0 && info.references == 1);
    expected_counter = last_counter;
    requests[0].rule.id = 100;
    requests[1].rule.id = 101;
    fail_create_completion = fail_destroy_completion = true;
    assert(dppd_flow_service_submit(service, 0, requests, 2, UINT64_C(200000001), &job) == 0);
    struct dppd_flow_job_result result;
    int rc = -EAGAIN;
    for (unsigned int i = 0; i < 16 && rc == -EAGAIN; ++i) {
        dppd_flow_service_process(service, UINT64_C(200000002), 64);
        rc = dppd_flow_service_collect(service, &job, &result);
    }
    assert(rc == -EUCLEAN && flow_count == 1);
    assert(dppd_flow_shared_counts_get(counts, b.spec.id, b.spec.generation, &info) == 0 && info.references == 3);
    assert(dppd_flow_shared_counts_remove(counts, b.spec.id, b.spec.generation) == -EBUSY);
    assert(dppd_flow_service_retry(service) == 0);
    result = finish(service, &job, UINT64_C(200000003));
    assert(!result.applied && result.status == -EIO && flow_count == 0);
    assert(dppd_flow_service_retry(service) == 0);
    assert(dppd_flow_shared_counts_remove(counts, b.spec.id, b.spec.generation) == 0);
    assert(dppd_flow_shared_counts_add(counts, b.spec.id, 7, DPPD_RULE_DOMAIN_INGRESS, &b) == 0);
    assert(dppd_flow_service_submit(service, 0, requests, 1, UINT64_C(200000004), &job) == -ESTALE);
    assert(dppd_flow_service_close(&service) == 0 && dppd_async_flow_queue_close(&queue) == 0);
    fail_table_destroy = true;
    assert(dppd_flow_template_destroy(templates, &table) == -EUCLEAN);
    assert(dppd_flow_shared_counts_remove(counts, a.spec.id, a.spec.generation) == -EBUSY);
    fail_table_destroy = false;
    assert(dppd_flow_template_destroy(templates, &table) == 0);
    shape.dynamic_actions = 0;
    expected_counter = counter_a;
    assert(dppd_flow_template_create(templates, &shape, &table) == 0);
    requests[0].rule.actions[0].conf.shared_count.generation = b.spec.generation;
    struct dppd_flow_template_lease lease;
    assert(dppd_flow_template_pin(templates, &table, &requests[0].rule, &lease) == -EINVAL);
    assert(dppd_flow_template_destroy(templates, &table) == 0);
#else
    (void)counter_a;
    assert(dppd_flow_template_create(templates, &shape, &table) == -ENOTSUP);
    assert(dppd_flow_shared_counts_query(counts, b.spec.id, b.spec.generation, &hits, &bytes) == 0 && hits == 0 &&
        bytes == 0);
#endif
    assert(dppd_flow_templates_close(&templates) == 0);
    assert(dppd_flow_shared_counts_remove(counts, a.spec.id, a.spec.generation) == 0);
    assert(dppd_flow_shared_counts_remove(counts, b.spec.id, b.spec.generation) == 0);
    assert(dppd_flow_shared_counts_close(&counts) == 0 && counter_live == 0);
    assert(dppd_admission_close(&admission) == 0);
#if RTE_VERSION >= RTE_VERSION_NUM(24, 11, 0, 0)
    test_native_store_restart();
#endif
    return 0;
}
