#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dppd/recovery_guard.h"
#include "dppd/rte_flow_backend.h"

static struct dppd_recovery_guard *active_guard;
static unsigned int sync_calls, fail_sync, creates, destroys, flow_creates, flow_removes;
static bool fail_intent, fail_result, fail_remove_record, fail_driver, partial_driver, missing_handle;
static int destroy_error;
static char order[16];
static unsigned int order_count;

int __real_fsync(int fd);
int __wrap_fsync(int fd);

/** 文件仍然真实写入，只让选定一次同步报错，验证已经产生的对象不会随记录故障丢失 */
int __wrap_fsync(int fd)
{
    if (++sync_calls == fail_sync) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

/** 在假驱动入口重新解码已锁文件，证明先同步意图再进入 PMD，而不是只改内存记录 */
static void check_durable_intent(enum dppd_recovery_object_kind kind)
{
    struct dppd_recovery_record disk;
    assert(dppd_recovery_guard_read_locked(active_guard->fd, active_guard->path, &disk) == 0);
    assert(disk.format == 7 && disk.attempt_count != 0);
    const struct dppd_recovery_attempt *attempt = NULL;
    for (uint32_t index = 0; index < disk.attempt_count; ++index)
        if (disk.attempts[index].id == disk.last_attempt)
            attempt = &disk.attempts[index];
    assert(attempt != NULL);
    assert(attempt->kind == kind && attempt->phase == DPPD_RECOVERY_INTENT);
    assert(attempt->domain == DPPD_RULE_DOMAIN_TRANSFER);
    if (kind == DPPD_RECOVERY_OBJECT_FLOW)
        assert(attempt->shared_id == 900 && attempt->shared_generation == 4);
}

static int shared_create(const struct dppd_shared_count_spec *spec, struct dppd_shared_count_handle *handle,
    struct dppd_flow_error *error)
{
    (void)error;
    ++creates;
    check_durable_intent(DPPD_RECOVERY_OBJECT_SHARED_COUNT);
    if ((!fail_driver || partial_driver) && !missing_handle)
        *handle = (struct dppd_shared_count_handle){.spec = *spec,
            .opaque = (struct rte_flow_action_handle *)(uintptr_t)0x900};
    return fail_driver ? -ENOSPC : 0;
}

static int shared_destroy(struct dppd_shared_count_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    ++destroys;
    order[order_count++] = 'S';
    assert(handle->opaque == (struct rte_flow_action_handle *)(uintptr_t)0x900);
    memset(handle, 0, sizeof(*handle));
    return destroy_error;
}

static int validate(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_error *error)
{
    (void)port;
    (void)rule;
    (void)error;
    return 0;
}

static int ordinary_create(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_handle *handle,
    struct dppd_flow_error *error)
{
    (void)handle;
    assert(false);
    return validate(port, rule, error);
}

static int bound_validate(uint16_t port, const struct dppd_rule *rule, const struct dppd_flow_binding *binding,
    struct dppd_flow_error *error)
{
    assert(binding->actions[0] == (struct rte_flow_action_handle *)(uintptr_t)0x900);
    return validate(port, rule, error);
}

static int bound_create(uint16_t port, const struct dppd_rule *rule, const struct dppd_flow_binding *binding,
    struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    assert(bound_validate(port, rule, binding, error) == 0);
    check_durable_intent(DPPD_RECOVERY_OBJECT_FLOW);
    ++flow_creates;
    *handle = (struct dppd_flow_handle){.flow = (struct rte_flow *)(uintptr_t)0x100,
        .port_id = port,
        .rule_id = rule->id,
        .rule_generation = rule->generation};
    return 0;
}

static int flow_remove(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    assert(handle->flow == (struct rte_flow *)(uintptr_t)0x100);
    ++flow_removes;
    order[order_count++] = 'F';
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static int before_shared(void *context, const struct dppd_shared_count_spec *spec, uint64_t *attempt)
{
    int rc = dppd_recovery_guard_prepare_shared(context, spec, "pci-test", "test-driver", NULL);
    if (rc == 0 && fail_intent)
        fail_sync = sync_calls + 1;
    return rc == 0 ? dppd_recovery_guard_begin_shared(context, spec, attempt) : rc;
}

static int before_flow(void *context, uint16_t port, const struct dppd_rule *rule, uint64_t *attempt,
    uint8_t cookie[DPPD_TAP_COOKIE_SIZE])
{
    memset(cookie, 0, DPPD_TAP_COOKIE_SIZE);
    const struct dppd_action *shared = &rule->actions[0];
    assert(shared->type == DPPD_ACTION_SHARED_COUNT);
    return dppd_recovery_guard_begin_flow(context, port, rule->id, rule->generation, rule->domain,
        shared->conf.shared_count.id, shared->conf.shared_count.generation, attempt);
}

static int after_create(void *context, uint64_t attempt, int error)
{
    if (fail_result)
        fail_sync = sync_calls + 1;
    return dppd_recovery_guard_created(context, attempt, error, DPPD_EVIDENCE_NONE, 0, NULL);
}

static void after_remove(void *context, uint64_t attempt, int error)
{
    if (fail_remove_record)
        fail_sync = sync_calls + 1;
    struct dppd_recovery_guard *guard = context;
    if (dppd_recovery_guard_removed(guard, attempt, error) != 0)
        guard->faulted = true;
}

static const struct dppd_flow_api api = {.validate = validate,
    .create = ordinary_create,
    .remove = flow_remove,
    .shared_count_create = shared_create,
    .shared_count_destroy = shared_destroy,
    .validate_bound = bound_validate,
    .create_bound = bound_create};
static const struct dppd_shared_count_spec spec = {.id = 900,
    .generation = 4,
    .install_port_id = 7,
    .domain = DPPD_RULE_DOMAIN_TRANSFER};

static void configure(struct dppd_rte_flow_backend *backend, struct dppd_recovery_guard *guard, const char *path)
{
    assert(dppd_recovery_guard_open(guard, path, "/tmp/dppd-shared-test-state") == 0);
    active_guard = guard;
    assert(dppd_rte_flow_backend_init(backend, 4, &api) == 0);
    backend->before_create_context = guard;
    backend->before_create = before_flow;
    backend->after_create = after_create;
    backend->after_remove = after_remove;
    backend->before_shared_create = before_shared;
    backend->after_shared_create = after_create;
    backend->after_shared_remove = after_remove;
    creates = destroys = flow_creates = flow_removes = order_count = 0;
    sync_calls = fail_sync = 0;
    fail_intent = fail_result = fail_remove_record = fail_driver = partial_driver = missing_handle = false;
    destroy_error = 0;
}

/** 真实事务引用共享对象，正常退出必须先删规则再删计数器，二者记录都确认后才能 clean */
static void lifecycle(const char *path)
{
    struct dppd_rte_flow_backend backend;
    struct dppd_recovery_guard guard;
    configure(&backend, &guard, path);
    assert(dppd_rte_flow_backend_shared_count_create(&backend, &spec, NULL) == 0);
    struct dppd_transaction_item item = {
        .rule = {.id = 100,
            .generation = 9,
            .install_port_id = 7,
            .domain = DPPD_RULE_DOMAIN_TRANSFER,
            .nb_matches = 1,
            .matches = {{.type = DPPD_MATCH_ETH}},
            .nb_actions = 2,
            .actions = {{.type = DPPD_ACTION_SHARED_COUNT, .conf.shared_count = {.id = 900, .generation = 4}},
                {.type = DPPD_ACTION_DROP}}},
        .plan = {.rule_id = 100, .rule_generation = 9, .install_port_id = 7, .backend = DPPD_PLAN_BACKEND_RTE_FLOW}};
    struct dppd_transaction transaction;
    struct dppd_transaction_backends backends = {.rte_flow = dppd_rte_flow_transaction_backend(&backend)};
    assert(dppd_transaction_init(&transaction, 1, &item, 1) == 0);
    assert(dppd_transaction_run(&transaction, &backends) == 0);
    assert(creates == 1 && flow_creates == 1 && guard.record.attempt_count == 2);
    assert(dppd_recovery_guard_clean(&guard) == -EUCLEAN);
    assert(dppd_rte_flow_backend_shared_count_destroy(&backend, spec.id, spec.generation, NULL) == -EBUSY);
    assert(dppd_rte_flow_backend_fini(&backend) == 0);
    assert(order_count == 2 && order[0] == 'F' && order[1] == 'S');
    assert(flow_removes == 1 && destroys == 1);
    assert(dppd_recovery_guard_clean(&guard) == 0);
    dppd_recovery_guard_close(&guard);
}

/** 旧失败没有句柄时仍可登记新尝试，但删除所有新对象也不能顺便确认旧失败已经无残留 */
static void failed_retry(const char *path)
{
    struct dppd_rte_flow_backend backend;
    struct dppd_recovery_guard guard;
    configure(&backend, &guard, path);
    uint64_t baseline_attempt = guard.record.last_attempt;
    fail_driver = true;
    assert(dppd_rte_flow_backend_shared_count_create(&backend, &spec, NULL) == -ENOSPC);
    fail_driver = false;
    assert(dppd_rte_flow_backend_shared_count_create(&backend, &spec, NULL) == 0);
    struct dppd_transaction_item items[2];
    for (unsigned int index = 0; index < 2; ++index) {
        items[index] = (struct dppd_transaction_item){
            .rule = {.id = 100 + index,
                .generation = 9,
                .install_port_id = 7,
                .domain = DPPD_RULE_DOMAIN_TRANSFER,
                .nb_matches = 1,
                .matches = {{.type = DPPD_MATCH_ETH}},
                .nb_actions = 2,
                .actions = {{.type = DPPD_ACTION_SHARED_COUNT, .conf.shared_count = {.id = 900, .generation = 4}},
                    {.type = DPPD_ACTION_DROP}}},
            .plan = {.rule_id = 100 + index,
                .rule_generation = 9,
                .install_port_id = 7,
                .backend = DPPD_PLAN_BACKEND_RTE_FLOW}};
    }
    struct dppd_transaction transaction;
    struct dppd_transaction_backends backends = {.rte_flow = dppd_rte_flow_transaction_backend(&backend)};
    assert(dppd_transaction_init(&transaction, 1, items, 2) == 0);
    assert(dppd_transaction_run(&transaction, &backends) == 0);
    assert(dppd_rte_flow_backend_remove_version(&backend, 100, 9, NULL) == 0);
    assert(dppd_rte_flow_backend_shared_count_destroy(&backend, spec.id, spec.generation, NULL) == -EBUSY);
    items[0].rule.id = items[0].plan.rule_id = 102;
    assert(dppd_transaction_init(&transaction, 2, items, 1) == 0);
    assert(dppd_transaction_run(&transaction, &backends) == 0);
    assert(guard.record.attempt_count == 4 && guard.record.attempts[2].id == baseline_attempt + 5);
    assert(dppd_rte_flow_backend_fini(&backend) == 0 && flow_creates == 3 && flow_removes == 3 && destroys == 1);
    assert(order_count == 4 && order[3] == 'S');
    assert(dppd_recovery_guard_clean(&guard) == -EUCLEAN);
    dppd_recovery_guard_close(&guard);
    assert(dppd_recovery_guard_open(&guard, path, NULL) == 0);
    assert(guard.record.attempts[0].phase == DPPD_RECOVERY_CREATE_FAILED);
    assert(dppd_recovery_guard_acknowledge(&guard, guard.record.revision) == 0);
    dppd_recovery_guard_close(&guard);
}

/** 同步失败、驱动部分成功和删除失败都保留可重试句柄，未知无句柄结果则要求外部核对 */
static void faults(const char *path)
{
    for (unsigned int mode = 0; mode < 7; ++mode) {
        struct dppd_rte_flow_backend backend;
        struct dppd_recovery_guard guard;
        configure(&backend, &guard, path);
        fail_intent = mode == 0;
        fail_result = mode == 1;
        fail_driver = mode == 2 || mode == 3;
        partial_driver = mode == 3;
        missing_handle = mode == 4;
        int expected = mode == 0 ? -EIO : mode == 2 ? -ENOSPC : mode < 5 ? -EUCLEAN : 0;
        assert(dppd_rte_flow_backend_shared_count_create(&backend, &spec, NULL) == expected);
        assert(creates == (mode == 0 ? 0U : 1U));
        fail_result = false;
        if (mode == 5) {
            destroy_error = -EBUSY;
            assert(dppd_rte_flow_backend_fini(&backend) == -EBUSY && backend.shared_counts != NULL);
            assert(guard.record.attempts[0].remove_error == -EBUSY);
            destroy_error = 0;
        }
        fail_remove_record = mode == 6;
        assert(dppd_rte_flow_backend_fini(&backend) == (mode == 4 ? -EUCLEAN : 0));
        if (mode == 4) {
            /** 成功却不给句柄的异常驱动无法安全删除，保留仓库并只释放测试进程的内存 */
            free(backend.shared_counts);
            free(backend.objects);
            dppd_flow_probe_cache_destroy(backend.probes);
        }
        assert(destroys == (mode == 0 || mode == 2 || mode == 4 ? 0U : mode == 5 ? 2U : 1U));
        int cleaned = dppd_recovery_guard_clean(&guard);
        assert(cleaned == (mode == 3 || mode == 5 ? 0 : -EUCLEAN));
        dppd_recovery_guard_close(&guard);
        assert(dppd_recovery_guard_open(&guard, path, NULL) == 0);
        if (cleaned != 0) {
            assert(guard.record.count == 1 && guard.record.attempt_count == 1);
            assert(dppd_recovery_guard_acknowledge(&guard, guard.record.revision) == 0);
        }
        dppd_recovery_guard_close(&guard);
    }
}

int main(void)
{
    char directory[] = "/tmp/dppd-shared-recovery-backend-XXXXXX", path[256];
    assert(mkdtemp(directory) != NULL);
    assert(snprintf(path, sizeof(path), "%s/recovery", directory) > 0);
    lifecycle(path);
    failed_retry(path);
    faults(path);
    assert(unlink(path) == 0 && rmdir(directory) == 0);
    return 0;
}
