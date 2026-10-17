#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dppd/control.h"
#include "dppd/device_safety.h"
#include "dppd/management.h"
#include "dppd/runtime.h"
#include "../../lib/control/reset_internal.h"

struct counter {
    bool active;
    unsigned int epoch, references;
    struct dppd_shared_count_spec spec;
    uint64_t hits, bytes;
};

struct flow {
    bool active;
    unsigned int epoch;
    struct dppd_rule rule;
    struct counter *counter;
    uint64_t hits, bytes;
};

struct fixture {
    struct dppd_runtime runtime;
    struct dppd_control_service control;
    struct counter counters[32];
    struct flow flows[96];
    unsigned int next_counter, next_flow, epoch;
    unsigned int validates, creates, removes, shared_creates, shared_removes, queries;
    unsigned int waits, starts, resets, discoveries, links;
    unsigned int fail_create, remove_failures, fail_discovery, event_create;
    int safety_error, identity_error, reset_error, link_error, start_error, wait_error;
    bool event_links, event_resume, event_start, event_validate, worker_failed, rollback_failure;
    char order[1024];
    size_t order_length;
    uint64_t generations[3], shared_generation;
};

static struct fixture *current;

static void step(char operation)
{
    assert(current->order_length + 1U < sizeof(current->order));
    current->order[current->order_length++] = operation;
    current->order[current->order_length] = '\0';
}

static uint64_t sequence(void)
{
    return atomic_load_explicit(&current->runtime.devices.event_sequence, memory_order_acquire);
}

/** 用原子状态模拟已经发布完毕的真实事件，不调用模拟复位接口 */
static void publish_event(void)
{
    struct dppd_device_set *devices = &current->runtime.devices;
    atomic_store_explicit(&devices->event_io_quiesced, true, memory_order_release);
    atomic_fetch_or_explicit(&devices->reset_event_ports, 1U, memory_order_release);
    atomic_fetch_add_explicit(&devices->event_sequence, 1U, memory_order_release);
}

static void driver_entry(void)
{
    assert(!dppd_rte_flow_backend_is_quarantined(&current->control.rte_flow));
    assert(sequence() == current->runtime.device_recovery.status.approved_sequence);
}

static struct counter *bound_counter(const struct dppd_rule *rule, const struct dppd_flow_binding *binding)
{
    struct counter *counter = NULL;
    for (uint16_t i = 0; i < rule->nb_actions; ++i) {
        if (rule->actions[i].type != DPPD_ACTION_SHARED_COUNT) {
            assert(binding == NULL || binding->actions[i] == NULL);
            continue;
        }
        assert(binding != NULL && binding->actions[i] != NULL && counter == NULL);
        counter = (struct counter *)binding->actions[i];
        assert(counter->active && counter->epoch == current->epoch);
        assert(counter->spec.id == rule->actions[i].conf.shared_count.id &&
            counter->spec.generation == rule->actions[i].conf.shared_count.generation);
    }
    return counter;
}

static int validate(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_error *error)
{
    (void)error;
    driver_entry();
    char reason[128];
    assert(port == 0 && dppd_rule_validate(rule, reason, sizeof(reason)) == 0);
    ++current->validates;
    if (current->event_validate)
        publish_event();
    return 0;
}

static int validate_bound(uint16_t port, const struct dppd_rule *rule, const struct dppd_flow_binding *binding,
    struct dppd_flow_error *error)
{
    assert(bound_counter(rule, binding) != NULL);
    return validate(port, rule, error);
}

static int create_bound(uint16_t port, const struct dppd_rule *rule, const struct dppd_flow_binding *binding,
    struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    driver_entry();
    step('F');
    if (++current->creates == current->fail_create) {
        if (current->rollback_failure)
            current->remove_failures = 1;
        return -ENOSPC;
    }
    assert(current->next_flow < sizeof(current->flows) / sizeof(*current->flows));
    struct flow *flow = &current->flows[current->next_flow++];
    *flow =
        (struct flow){.active = true, .epoch = current->epoch, .rule = *rule, .counter = bound_counter(rule, binding)};
    if (flow->counter != NULL)
        ++flow->counter->references;
    *handle = (struct dppd_flow_handle){.port_id = port,
        .rule_id = rule->id,
        .rule_generation = rule->generation,
        .flow = (struct rte_flow *)flow};
    for (uint16_t i = 0; i < rule->nb_actions; ++i) {
        handle->has_count |= rule->actions[i].type == DPPD_ACTION_COUNT;
        handle->has_age |= rule->actions[i].type == DPPD_ACTION_AGE;
    }
    if (current->creates == current->event_create)
        publish_event();
    return 0;
}

static int create(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_handle *handle,
    struct dppd_flow_error *error)
{
    return create_bound(port, rule, NULL, handle, error);
}

static struct flow *checked_flow(const struct dppd_flow_handle *handle)
{
    struct flow *flow = (struct flow *)handle->flow;
    assert(flow != NULL && flow->active && flow->epoch == current->epoch);
    assert(flow->rule.id == handle->rule_id && flow->rule.generation == handle->rule_generation);
    return flow;
}

static int remove_flow(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    driver_entry();
    struct flow *flow = checked_flow(handle);
    ++current->removes;
    step('f');
    if (current->remove_failures != 0) {
        --current->remove_failures;
        return -EIO;
    }
    if (flow->counter != NULL) {
        assert(flow->counter->active && flow->counter->references != 0);
        --flow->counter->references;
    }
    flow->active = false;
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static int query_count(const struct dppd_flow_handle *handle, uint64_t *hits, uint64_t *bytes,
    struct dppd_flow_error *error)
{
    (void)error;
    driver_entry();
    struct flow *flow = checked_flow(handle);
    assert(handle->has_count);
    ++current->queries;
    *hits = flow->hits;
    *bytes = flow->bytes;
    return 0;
}

static int query_age(const struct dppd_flow_handle *handle, struct dppd_flow_age_result *result,
    struct dppd_flow_error *error)
{
    (void)error;
    driver_entry();
    (void)checked_flow(handle);
    assert(handle->has_age);
    ++current->queries;
    *result = (struct dppd_flow_age_result){.sec_since_last_hit_valid = true};
    return 0;
}

static int shared_create(const struct dppd_shared_count_spec *spec, struct dppd_shared_count_handle *handle,
    struct dppd_flow_error *error)
{
    (void)error;
    driver_entry();
    step('C');
    ++current->shared_creates;
    assert(current->next_counter < sizeof(current->counters) / sizeof(*current->counters));
    struct counter *counter = &current->counters[current->next_counter++];
    *counter = (struct counter){.active = true, .epoch = current->epoch, .spec = *spec};
    *handle = (struct dppd_shared_count_handle){.spec = *spec, .opaque = (struct rte_flow_action_handle *)counter};
    return 0;
}

static int shared_destroy(struct dppd_shared_count_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    driver_entry();
    struct counter *counter = (struct counter *)handle->opaque;
    assert(counter != NULL && counter->active && counter->epoch == current->epoch && counter->references == 0);
    ++current->shared_removes;
    step('d');
    counter->active = false;
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static int shared_query(const struct dppd_shared_count_handle *handle, uint64_t *hits, uint64_t *bytes,
    struct dppd_flow_error *error)
{
    (void)error;
    driver_entry();
    const struct counter *counter = (const struct counter *)handle->opaque;
    assert(counter != NULL && counter->active && counter->epoch == current->epoch);
    ++current->queries;
    *hits = counter->hits;
    *bytes = counter->bytes;
    return 0;
}

int __wrap_dppd_devices_enable_event_tracking(struct dppd_device_set *devices)
{
    assert(devices == &current->runtime.devices && !current->runtime.workers_started);
    atomic_store_explicit(&devices->event_tracking_enabled, true, memory_order_release);
    return 0;
}

int __wrap_dppd_devices_check_reset_safety(const struct dppd_device_set *devices)
{
    assert(devices == &current->runtime.devices);
    step('S');
    return current->safety_error;
}

int __wrap_dppd_devices_reset_identity_ready(const struct dppd_device_set *devices, const struct dppd_config *config)
{
    assert(devices == &current->runtime.devices && config == &current->runtime.config);
    return current->identity_error;
}

int __wrap_dppd_topology_rediscover_verified(const struct dppd_config *config, const struct dppd_topology *expected,
    struct dppd_topology *output)
{
    assert(config == &current->runtime.config && expected == &current->runtime.devices.topology);
    step('T');
    if (++current->discoveries == current->fail_discovery)
        return -EXDEV;
    *output = *expected;
    return 0;
}

int __wrap_dppd_devices_pause_io(struct dppd_device_set *devices, uint64_t expected)
{
    assert(devices == &current->runtime.devices);
    step('P');
    atomic_store_explicit(&devices->event_io_quiesced, true, memory_order_release);
    return expected == sequence() ? 0 : -ESTALE;
}

int __wrap_dppd_runtime_wait(struct dppd_runtime *runtime)
{
    assert(runtime == &current->runtime && atomic_load_explicit(&runtime->stop_requested, memory_order_acquire));
    step('W');
    ++current->waits;
    for (uint16_t i = 0; i < runtime->nb_workers; ++i) {
        runtime->workers[i].launched = false;
        atomic_store_explicit(&runtime->workers[i].state, DPPD_WORKER_STOPPED, memory_order_release);
    }
    runtime->workers_started = false;
    int error = current->wait_error;
    current->wait_error = 0;
    return error;
}

/** 这是设备操作的顺序夹具，只断言旧对象已正常删除，不伪造失效旧句柄的证据 */
int __wrap_dppd_devices_reset_reconfigure(struct dppd_device_set *devices, const struct dppd_config *config,
    uint64_t expected, struct dppd_device_reset_result *result)
{
    assert(devices == &current->runtime.devices && config == &current->runtime.config && expected == sequence());
    assert(!current->runtime.workers_started && dppd_devices_io_quiesced(devices));
    assert(current->control.rte_flow.quarantined && current->control.rte_flow.count == 0 &&
        current->control.rte_flow.shared_count_count == 0);
    for (unsigned int i = 0; i < current->next_flow; ++i)
        assert(!current->flows[i].active);
    for (unsigned int i = 0; i < current->next_counter; ++i)
        assert(!current->counters[i].active && current->counters[i].references == 0);
    step('R');
    ++current->resets;
    *result = (struct dppd_device_reset_result){.phase = DPPD_DEVICE_RESET_RESET,
        .sequence_after = expected,
        .stopped_ports = 1};
    if (current->reset_error != 0)
        return current->reset_error;
    ++current->epoch;
    publish_event();
    result->phase = DPPD_DEVICE_RESET_READY;
    result->sequence_after = sequence();
    result->reset_ports = result->configured_ports = result->started_ports = 1;
    return 0;
}

int __wrap_dppd_devices_poll_links(struct dppd_device_set *devices)
{
    assert(devices == &current->runtime.devices && !current->runtime.workers_started);
    step('L');
    ++current->links;
    if (current->event_links)
        publish_event();
    return current->link_error;
}

int __wrap_dppd_devices_resume_after_event(struct dppd_device_set *devices, uint64_t expected)
{
    assert(devices == &current->runtime.devices && !current->runtime.workers_started);
    assert(current->control.recovery_state == DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED);
    assert(dppd_control_device_reset_pending(&current->control));
    assert(current->control.rte_flow.count == 3 && current->control.rte_flow.shared_count_count == 1);
    step('U');
    if (current->event_resume)
        publish_event();
    if (expected != sequence())
        return -ESTALE;
    atomic_store_explicit(&devices->resumed_event_sequence, expected, memory_order_release);
    atomic_store_explicit(&devices->event_io_quiesced, false, memory_order_release);
    return 0;
}

int __wrap_dppd_runtime_start(struct dppd_runtime *runtime)
{
    assert(runtime == &current->runtime && !runtime->workers_started);
    assert(!dppd_devices_io_quiesced(&runtime->devices));
    assert(current->control.recovery_state == DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED);
    assert(dppd_control_device_reset_pending(&current->control));
    step('A');
    ++current->starts;
    if (current->start_error != 0)
        return current->start_error;
    atomic_store_explicit(&runtime->stop_requested, false, memory_order_release);
    runtime->workers_started = true;
    for (uint16_t i = 0; i < runtime->nb_workers; ++i) {
        runtime->workers[i].launched = true;
        atomic_store_explicit(&runtime->workers[i].state,
            current->worker_failed ? DPPD_WORKER_FAILED : DPPD_WORKER_RUNNING, memory_order_release);
    }
    if (current->event_start)
        publish_event();
    return 0;
}

static struct fixture *initialize(void)
{
    struct fixture *fixture = calloc(1, sizeof(*fixture));
    assert(fixture != NULL);
    current = fixture;
    struct dppd_runtime *runtime = &fixture->runtime;
    runtime->nb_workers = 1;
    runtime->config.nb_ports = runtime->devices.nb_ports = 1;
    runtime->config.nb_queues = 1;
    atomic_init(&runtime->stop_requested, false);
    struct dppd_device_set *devices = &runtime->devices;
    atomic_init(&devices->removal_requested, false);
    atomic_init(&devices->event_tracking_enabled, false);
    atomic_init(&devices->event_io_quiesced, false);
    atomic_init(&devices->event_sequence_overflow, false);
    atomic_init(&devices->event_callbacks_in_flight, 0);
    atomic_init(&devices->event_sequence, 0);
    atomic_init(&devices->resumed_event_sequence, 0);
    atomic_init(&devices->reset_event_ports, 0);
    atomic_init(&devices->removal_event_ports, 0);
    atomic_init(&devices->destroy_event_ports, 0);
    devices->ports[0].started = devices->ports[0].configured = true;
    atomic_init(&devices->ports[0].removed, false);
    atomic_init(&devices->ports[0].link_state, DPPD_LINK_UP);
    atomic_init(&runtime->workers[0].state, DPPD_WORKER_IDLE);
    devices->topology.nb_endpoints = 1;
    struct dppd_endpoint *endpoint = &devices->topology.endpoints[0];
    endpoint->kind = DPPD_ENDPOINT_ETHDEV;
    strcpy(endpoint->device_name, "0000:03:00.0");
    strcpy(endpoint->driver_name, "coordinator-test");
    const struct dppd_flow_api api = {.validate = validate,
        .create = create,
        .remove = remove_flow,
        .query_count = query_count,
        .query_age = query_age,
        .shared_count_create = shared_create,
        .shared_count_query = shared_query,
        .shared_count_destroy = shared_destroy,
        .validate_bound = validate_bound,
        .create_bound = create_bound};
    assert(dppd_control_init(&fixture->control, &devices->topology, 8, &api) == 0);
    assert(fixture->control.rte_flow.api.simulate_reset == NULL);
    assert(dppd_runtime_device_bind(runtime, &fixture->control) == 0);
    struct dppd_shared_count_spec spec = {.id = 100, .domain = DPPD_RULE_DOMAIN_INGRESS};
    struct dppd_shared_count_info info;
    assert(dppd_control_shared_count_create(&fixture->control, &spec, &info) == 0);
    fixture->shared_generation = info.spec.generation;
    for (unsigned int i = 0; i < 3; ++i) {
        struct dppd_rule rule = {.id = (i + 1U) * 10U,
            .fallback = DPPD_FALLBACK_REQUIRE_HARDWARE,
            .nb_matches = 1,
            .matches = {{.type = DPPD_MATCH_ETH}},
            .nb_actions = i == 0 ? 3 : 2};
        rule.actions[0].type = i < 2 ? DPPD_ACTION_SHARED_COUNT : DPPD_ACTION_COUNT;
        if (i < 2) {
            rule.actions[0].conf.shared_count.id = 100;
            rule.actions[0].conf.shared_count.generation = info.spec.generation;
        }
        if (i == 0)
            rule.actions[1] = (struct dppd_action){.type = DPPD_ACTION_AGE, .conf.age_timeout_seconds = 5};
        rule.actions[rule.nb_actions - 1U].type = DPPD_ACTION_DROP;
        struct dppd_control_apply_result result;
        assert(dppd_control_apply(&fixture->control, 0, &rule, 0, &result) == 0);
        fixture->generations[i] = result.generation;
        fixture->flows[i].hits = 7;
        fixture->flows[i].bytes = 98;
    }
    fixture->counters[0].hits = 11;
    fixture->counters[0].bytes = 154;
    runtime->workers_started = runtime->workers[0].launched = true;
    atomic_store_explicit(&runtime->workers[0].state, DPPD_WORKER_RUNNING, memory_order_release);
    fixture->order_length = 0;
    fixture->order[0] = '\0';
    return fixture;
}

/** 测试进程只回收自己的内存，未知句柄不能为方便收尾而交给任何驱动删除 */
static void finish(struct fixture *fixture)
{
    if (dppd_rte_flow_backend_is_quarantined(&fixture->control.rte_flow)) {
        unsigned int removes = fixture->removes, shared = fixture->shared_removes;
        assert(dppd_control_fini(&fixture->control) == -EUCLEAN);
        assert(fixture->removes == removes && fixture->shared_removes == shared);
        free(fixture->control.rte_flow.objects);
        free(fixture->control.rte_flow.shared_counts);
        free(fixture->control.rte_flow.probes);
        fixture->control.rte_flow.objects = NULL;
        fixture->control.rte_flow.shared_counts = NULL;
        fixture->control.rte_flow.probes = NULL;
        dppd_software_backend_fini(&fixture->control.software);
        dppd_rule_repository_destroy(&fixture->control.rules);
        dppd_control_reset_plan_destroy(&fixture->control);
        free(fixture->control.persistence_path);
    } else {
        fixture->remove_failures = 0;
        assert(dppd_control_fini(&fixture->control) == 0);
    }
    free(fixture);
    current = NULL;
}

static struct dppd_device_recovery_status status(void)
{
    struct dppd_device_recovery_status result;
    dppd_runtime_device_status(&current->runtime, &result);
    return result;
}

static void assert_desired(void)
{
    assert(current->control.rules.count == 3 && current->control.rules.generation == 3);
    assert(current->control.shared_count_revision == current->shared_generation);
    for (unsigned int i = 0; i < 3; ++i) {
        struct dppd_rule rule;
        assert(dppd_rule_repository_get(&current->control.rules, (i + 1U) * 10U, &rule) == 0);
        assert(rule.generation == current->generations[i]);
    }
}

struct file_stamp {
    uint64_t hash;
    struct stat metadata;
};

static struct file_stamp stamp(const char *path)
{
    struct file_stamp result = {.hash = UINT64_C(14695981039346656037)};
    assert(stat(path, &result.metadata) == 0);
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    int byte;
    while ((byte = fgetc(file)) != EOF) {
        result.hash ^= (unsigned char)byte;
        result.hash *= UINT64_C(1099511628211);
    }
    assert(!ferror(file) && fclose(file) == 0);
    return result;
}

static void assert_snapshot(const char *path, const struct file_stamp *before)
{
    struct file_stamp after = stamp(path);
    assert(after.hash == before->hash && after.metadata.st_ino == before->metadata.st_ino &&
        after.metadata.st_size == before->metadata.st_size &&
        after.metadata.st_mtim.tv_sec == before->metadata.st_mtim.tv_sec &&
        after.metadata.st_mtim.tv_nsec == before->metadata.st_mtim.tv_nsec);
}

/** 完整成功必须先停线程再清旧对象，重建共享对象后才重建规则，worker 就绪后才 READY */
static void successful_order_and_snapshot(void)
{
    struct fixture *fixture = initialize();
    char directory[] = "/tmp/dppd-device-coordinator-XXXXXX", path[256];
    assert(mkdtemp(directory) != NULL);
    assert(snprintf(path, sizeof(path), "%s/state.bin", directory) > 0);
    assert(dppd_control_persistence_attach(&fixture->control, path) == 0);
    assert(dppd_control_persistence_flush(&fixture->control) == 0);
    struct file_stamp before = stamp(path);
    assert(dppd_runtime_device_reset(&fixture->runtime, 0) == 0);
    assert(strcmp(fixture->order, "STPWfffdRLTCFFFUA") == 0);
    struct dppd_device_recovery_status result = status();
    assert(result.state == DPPD_DEVICE_RECOVERY_RUNNING && !result.planned && !result.quarantined);
    assert(result.attempts == 1 && result.resets == 1 && result.recoveries == 1 && result.approved_sequence == 1);
    assert(result.nb_ports == 1 && result.ports[0] == 0);
    assert(result.desired_rules == 3 && result.actual_rules == 3 && result.desired_shared == 1 &&
        result.actual_shared == 1);
    assert(!dppd_control_device_reset_pending(&fixture->control) && fixture->runtime.workers_started);
    assert(fixture->control.recovery_state == DPPD_CONTROL_RECOVERY_READY);
    assert_desired();
    assert_snapshot(path, &before);
    struct dppd_control_shared_count_result shared;
    assert(dppd_control_shared_count_query(&fixture->control, 100, fixture->shared_generation, &shared) == 0);
    assert(shared.hits == 0 && shared.bytes == 0 && shared.object.references == 2);
    struct dppd_control_count_result count;
    assert(dppd_control_query_count(&fixture->control, 30, fixture->generations[2], &count) == 0);
    assert(count.hits == 0 && count.bytes == 0);
    struct dppd_control_age_result age;
    assert(dppd_control_query_age(&fixture->control, 10, fixture->generations[0], &age) == 0);
    assert(!age.state.aged && age.state.sec_since_last_hit_valid && age.state.sec_since_last_hit == 0);
    assert(fixture->next_flow == 6 && fixture->next_counter == 2 && !fixture->flows[0].active);
    assert(fixture->flows[3].epoch == 1 && &fixture->flows[0] != &fixture->flows[3]);
    finish(fixture);
    assert_snapshot(path, &before);
    assert(unlink(path) == 0 && rmdir(directory) == 0);
}

/** 预检失败没有停止线程或调用驱动，旧序号不能被当成用户已批准的新事件 */
static void rejected_before_side_effects(void)
{
    for (unsigned int mode = 0; mode < 5; ++mode) {
        struct fixture *fixture = initialize();
        int expected;
        if (mode == 0) {
            fixture->safety_error = -EACCES;
            expected = -EACCES;
        } else if (mode == 1) {
            fixture->fail_discovery = 1;
            expected = -EXDEV;
        } else if (mode == 2) {
            fixture->control.rte_flow.api.create_bound = NULL;
            expected = -ENOTSUP;
        } else if (mode == 3)
            expected = -ESTALE;
        else {
            fixture->identity_error = -ENODATA;
            expected = -ENODATA;
        }
        assert(dppd_runtime_device_reset(&fixture->runtime, mode == 3 ? 1 : 0) == expected);
        assert(fixture->waits == 0 && fixture->resets == 0 && fixture->removes == 0 && fixture->starts == 0);
        assert(fixture->runtime.workers_started && fixture->control.recovery_state == DPPD_CONTROL_RECOVERY_READY);
        assert(!dppd_control_device_reset_pending(&fixture->control));
        assert_desired();
        finish(fixture);
    }
}

/** 旧句柄清理失败时保留完整计划，下一次先清掉剩余有效句柄再执行设备复位 */
static void cleanup_retry(void)
{
    struct fixture *fixture = initialize();
    fixture->remove_failures = 1;
    assert(dppd_runtime_device_reset(&fixture->runtime, 0) == -EUCLEAN);
    assert(fixture->resets == 0 && fixture->starts == 0 && status().planned && !status().quarantined);
    assert(status().actual_rules == 1 && status().actual_shared == 1 && !fixture->runtime.workers_started);
    assert(dppd_control_reconciliation_retry(&fixture->control) == -EBUSY);
    assert_desired();
    assert(dppd_runtime_device_reset(&fixture->runtime, 0) == 0);
    assert(fixture->resets == 1 && status().recoveries == 1 && status().attempts == 2);
    assert_desired();
    finish(fixture);
}

/** 重建第二条失败会补偿第一条和共享对象，重试仍沿用原来的精确版本 */
static void rebuild_retry(void)
{
    struct fixture *fixture = initialize();
    fixture->fail_create = fixture->creates + 2U;
    assert(dppd_runtime_device_reset(&fixture->runtime, 0) == -ENOSPC);
    assert(status().planned && !status().quarantined && status().actual_rules == 0 && status().actual_shared == 0);
    assert(fixture->starts == 0 && fixture->shared_removes == 2 && fixture->removes == 4);
    assert_desired();
    fixture->fail_create = 0;
    assert(dppd_runtime_device_reset(&fixture->runtime, 1) == 0);
    assert(status().recoveries == 1 && fixture->resets == 2 && status().approved_sequence == 2);
    assert_desired();
    finish(fixture);
}

/** 补偿删除失败仍保留新句柄与引用，后续清理只使用这些已知有效对象，磁盘不重写 */
static void rollback_retry_preserves_snapshot(void)
{
    struct fixture *fixture = initialize();
    char directory[] = "/tmp/dppd-device-rollback-XXXXXX", path[256];
    assert(mkdtemp(directory) != NULL);
    assert(snprintf(path, sizeof(path), "%s/state.bin", directory) > 0);
    assert(dppd_control_persistence_attach(&fixture->control, path) == 0);
    assert(dppd_control_persistence_flush(&fixture->control) == 0);
    struct file_stamp before = stamp(path);
    fixture->rollback_failure = true;
    fixture->fail_create = fixture->creates + 2U;
    assert(dppd_runtime_device_reset(&fixture->runtime, 0) == -EUCLEAN);
    assert(status().planned && !status().quarantined && status().actual_rules == 1 && status().actual_shared == 1);
    assert(fixture->counters[1].active && fixture->counters[1].references == 1 && fixture->starts == 0);
    assert_desired();
    assert_snapshot(path, &before);
    fixture->rollback_failure = false;
    fixture->fail_create = 0;
    assert(dppd_runtime_device_reset(&fixture->runtime, 1) == 0);
    assert(!fixture->counters[1].active && fixture->counters[1].references == 0);
    assert(fixture->resets == 2 && status().recoveries == 1);
    assert_desired();
    assert_snapshot(path, &before);
    finish(fixture);
    assert_snapshot(path, &before);
    assert(unlink(path) == 0 && rmdir(directory) == 0);
}

/** PMD 不支持复位时也不能丢掉 desired 或提前恢复 worker，空仓库仍保持隔离等待核对 */
static void unsupported_and_device_failures(void)
{
    for (unsigned int mode = 0; mode < 4; ++mode) {
        struct fixture *fixture = initialize();
        int expected = -EIO;
        if (mode == 0) {
            fixture->reset_error = -ENOTSUP;
            expected = -ENOTSUP;
        } else if (mode == 1)
            fixture->link_error = -EIO;
        else if (mode == 2) {
            fixture->fail_discovery = 2;
            expected = -EXDEV;
        } else
            fixture->wait_error = -EIO;
        assert(dppd_runtime_device_reset(&fixture->runtime, 0) == expected);
        assert(status().planned && status().state == DPPD_DEVICE_RECOVERY_ISOLATED && fixture->starts == 0);
        assert(!fixture->runtime.workers_started && fixture->control.recovery_state != DPPD_CONTROL_RECOVERY_READY);
        assert_desired();
        if (mode == 0) {
            assert(
                status().quarantined && status().actual_rules == 0 && status().device_phase == DPPD_DEVICE_RESET_RESET);
        }
        finish(fixture);
    }
}

/** 事件出现在链路、重建、解除门禁或 worker 启动窗口时，都不能提交成功恢复 */
static void event_windows(void)
{
    for (unsigned int mode = 0; mode < 4; ++mode) {
        struct fixture *fixture = initialize();
        fixture->event_links = mode == 0;
        fixture->event_create = mode == 1 ? fixture->creates + 1U : 0;
        fixture->event_resume = mode == 2;
        fixture->event_start = mode == 3;
        assert(dppd_runtime_device_reset(&fixture->runtime, 0) < 0);
        assert(status().planned && status().quarantined && status().recoveries == 0);
        assert(!fixture->runtime.workers_started && fixture->control.recovery_state != DPPD_CONTROL_RECOVERY_READY);
        assert(fixture->starts == (mode == 3 ? 1U : 0U));
        assert_desired();
        unsigned int removes = fixture->removes, shared = fixture->shared_removes, resets = fixture->resets;
        if (mode != 0) {
            assert(status().actual_rules != 0 && dppd_runtime_device_reset(&fixture->runtime, sequence()) == -EUCLEAN);
            assert(fixture->removes == removes && fixture->shared_removes == shared && fixture->resets == resets);
        }
        assert(dppd_runtime_device_poll(&fixture->runtime) == 0);
        finish(fixture);
    }
}

/** 启动调用返回成功并不代表 worker 真正可用，失败状态必须保留计划供再次完整重建 */
static void workers_must_be_ready(void)
{
    for (unsigned int mode = 0; mode < 2; ++mode) {
        struct fixture *fixture = initialize();
        fixture->start_error = mode == 0 ? -EAGAIN : 0;
        fixture->worker_failed = mode != 0;
        assert(dppd_runtime_device_reset(&fixture->runtime, 0) == (mode == 0 ? -EAGAIN : -EIO));
        assert(status().planned && status().actual_rules == 3 && !status().quarantined && status().recoveries == 0);
        assert(!fixture->runtime.workers_started && fixture->control.recovery_state != DPPD_CONTROL_RECOVERY_READY);
        fixture->start_error = 0;
        fixture->worker_failed = false;
        assert(dppd_runtime_device_reset(&fixture->runtime, 1) == 0 && status().recoveries == 1);
        assert_desired();
        finish(fixture);
    }
}

/** 正式管理分发每条请求前重新看事件，未知句柄不能由后续请求或退出清理再次触碰 */
static void management_event_gate(void)
{
    struct fixture *fixture = initialize();
    struct dppd_management_request request = {.version = DPPD_MANAGEMENT_VERSION,
        .size = sizeof(request),
        .operation = DPPD_MANAGEMENT_DEVICE_STATUS};
    struct dppd_management_response response;
    assert(dppd_management_handle(&fixture->control, &fixture->runtime.devices, &fixture->runtime, &request,
               &response) == 0 &&
        response.status == 0 && response.payload.device.actual_rules == 3);
    request.operation = DPPD_MANAGEMENT_DEVICE_RESET;
    request.version = DPPD_MANAGEMENT_VERSION - 1U;
    assert(dppd_management_handle(&fixture->control, &fixture->runtime.devices, &fixture->runtime, &request,
               &response) == 0 &&
        response.status == -EPROTO && fixture->resets == 0);
    request.version = DPPD_MANAGEMENT_VERSION;
    publish_event();
    unsigned int queries = fixture->queries;
    request.operation = DPPD_MANAGEMENT_RULE_COUNT_QUERY;
    request.payload.count_query.rule_id = 30;
    request.payload.count_query.expected_generation = fixture->generations[2];
    assert(dppd_management_handle(&fixture->control, &fixture->runtime.devices, &fixture->runtime, &request,
               &response) == 0 &&
        response.status == -EUCLEAN);
    assert(fixture->queries == queries && fixture->removes == 0 && fixture->resets == 0);
    request.operation = DPPD_MANAGEMENT_DEVICE_RESET;
    request.payload.device_reset.expected_sequence = sequence();
    assert(dppd_management_handle(&fixture->control, &fixture->runtime.devices, &fixture->runtime, &request,
               &response) == 0 &&
        response.status == -EUCLEAN);
    assert(response.payload.device.quarantined && response.payload.device.actual_rules == 3);
    assert_desired();
    finish(fixture);
}

/** 缓存命中和驱动探测都不能越过事件门禁，探测中到达的事件也必须使成功答复失效 */
static void probe_cache_event_gate(void)
{
    for (unsigned int mode = 0; mode < 3; ++mode) {
        struct fixture *fixture = initialize();
        struct dppd_rule rule;
        assert(dppd_rule_repository_get(&fixture->control.rules, 30, &rule) == 0);
        struct dppd_flow_probe_result result;
        if (mode == 0) {
            assert(dppd_control_probe_rule(&fixture->control, 0, &rule, false, &result) == 0 && !result.cached);
            assert(dppd_control_probe_rule(&fixture->control, 0, &rule, false, &result) == 0 && result.cached);
            publish_event();
        } else if (mode == 1)
            fixture->event_validate = true;
        else
            atomic_store_explicit(&fixture->runtime.devices.event_callbacks_in_flight, 1, memory_order_release);
        unsigned int validates = fixture->validates;
        memset(&result, 0xa5, sizeof(result));
        assert(dppd_control_probe_rule(&fixture->control, 0, &rule, false, &result) == -EUCLEAN);
        assert(fixture->validates == validates + (mode == 1 ? 1U : 0U));
        assert(result.rule_id == 0 && !result.cached && status().quarantined);
        struct dppd_flow_probe_statistics statistics;
        assert(dppd_flow_probe_cache_statistics(fixture->control.rte_flow.probes, 0, &statistics) == 0);
        assert(statistics.cache_entries == 0 && fixture->removes == 0);
        /** 回调仍在途也要先封仓库并等待 worker，不能等序号写完后才开始隔离 */
        if (mode == 2)
            assert(dppd_runtime_device_poll(&fixture->runtime) == 0);
        atomic_store_explicit(&fixture->runtime.devices.event_callbacks_in_flight, 0, memory_order_release);
        assert(dppd_runtime_device_poll(&fixture->runtime) == 0);
        assert(!fixture->runtime.workers_started);
        finish(fixture);
    }
}

/** 全局移除通知先到而事件序号尚未发布时，也不能继续查询或复位，更不能删除旧句柄 */
static void global_removal_without_event(void)
{
    struct fixture *fixture = initialize();
    char directory[] = "/tmp/dppd-device-removal-XXXXXX", path[256];
    assert(mkdtemp(directory) != NULL);
    assert(snprintf(path, sizeof(path), "%s/state.bin", directory) > 0);
    assert(dppd_control_persistence_attach(&fixture->control, path) == 0);
    assert(dppd_control_persistence_flush(&fixture->control) == 0);
    struct file_stamp before = stamp(path);
    const struct dppd_flow_handle *handle =
        dppd_rte_flow_backend_find_version(&fixture->control.rte_flow, 30, fixture->generations[2]);
    assert(handle != NULL);
    struct dppd_flow_handle old = *handle;
    unsigned int validates = fixture->validates, creates = fixture->creates;
    unsigned int shared_creates = fixture->shared_creates, queries = fixture->queries;
    atomic_store_explicit(&fixture->runtime.devices.removal_requested, true, memory_order_release);
    assert(sequence() == 0 && status().removal_event_ports == 0 && status().reset_event_ports == 0);
    /** stable_event 必须在任何管理链路检查、清理和设备操作之前拒绝这次请求 */
    assert(dppd_runtime_device_reset(&fixture->runtime, 0) == -ENODEV);
    assert(fixture->order_length == 0 && fixture->resets == 0 && fixture->waits == 0);
    uint64_t hits = 71, bytes = 72;
    assert(dppd_rte_flow_backend_query_count(&fixture->control.rte_flow, 30, fixture->generations[2], &hits, &bytes,
               NULL) == -EUCLEAN);
    assert(hits == 71 && bytes == 72 && status().quarantined);
    assert(dppd_runtime_device_poll(&fixture->runtime) == 0);
    assert(status().state == DPPD_DEVICE_RECOVERY_ISOLATED && status().last_error == -ENODEV);
    assert(!fixture->runtime.workers_started && !status().planned && status().actual_rules == 3 &&
        status().actual_shared == 1);
    handle = dppd_rte_flow_backend_find_version(&fixture->control.rte_flow, 30, fixture->generations[2]);
    assert(handle != NULL && memcmp(handle, &old, sizeof(old)) == 0 && fixture->flows[2].active);
    assert(fixture->counters[0].active && fixture->counters[0].references == 2);
    assert(fixture->validates == validates && fixture->creates == creates &&
        fixture->shared_creates == shared_creates && fixture->queries == queries && fixture->removes == 0 &&
        fixture->shared_removes == 0 && fixture->resets == 0);
    assert_desired();
    assert_snapshot(path, &before);
    finish(fixture);
    assert_snapshot(path, &before);
    assert(unlink(path) == 0 && rmdir(directory) == 0);
}

int main(void)
{
    successful_order_and_snapshot();
    rejected_before_side_effects();
    cleanup_retry();
    rebuild_retry();
    rollback_retry_preserves_snapshot();
    unsupported_and_device_failures();
    event_windows();
    workers_must_be_ready();
    management_event_gate();
    probe_cache_event_gate();
    global_removal_without_event();
    return 0;
}
