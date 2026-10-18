#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "flow_state_fixture.h"

static bool fail_sync, fail_destroy, fail_created;
int __real_fsync(int fd);
int __wrap_fsync(int fd);
int __wrap_fsync(int fd)
{
    if (fail_sync) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}
int __real_dppd_flow_template_create(struct dppd_flow_templates *, const struct dppd_flow_template_spec *,
    struct dppd_flow_template_id *);
int __wrap_dppd_flow_template_create(struct dppd_flow_templates *, const struct dppd_flow_template_spec *,
    struct dppd_flow_template_id *);
int __wrap_dppd_flow_template_create(struct dppd_flow_templates *tables, const struct dppd_flow_template_spec *spec,
    struct dppd_flow_template_id *id)
{
    int rc = __real_dppd_flow_template_create(tables, spec, id);
    return rc == 0 && fail_created ? -EUCLEAN : rc;
}
int __real_dppd_flow_template_destroy(struct dppd_flow_templates *, const struct dppd_flow_template_id *);
int __wrap_dppd_flow_template_destroy(struct dppd_flow_templates *, const struct dppd_flow_template_id *);
int __wrap_dppd_flow_template_destroy(struct dppd_flow_templates *tables, const struct dppd_flow_template_id *id)
{
    return fail_destroy ? -EIO : __real_dppd_flow_template_destroy(tables, id);
}

static struct dppd_flow_job_result finish(struct dppd_flow_store *store, const struct dppd_flow_job_id *job)
{
    struct dppd_flow_job_result result;
    for (uint32_t i = 0; i < 64; ++i) {
        int rc = dppd_flow_store_collect(store, job, &result);
        if (rc == 0)
            return result;
        assert(rc == -EAGAIN);
        assert(dppd_flow_store_process(store, 2, 64) == 0);
    }
    assert(!"job unfinished");
    return result;
}

static struct dppd_flow_config_change add_template(const struct dppd_flow_state *state, uint64_t key, bool shared)
{
    struct dppd_flow_config_change change = {.operation = DPPD_FLOW_CONFIG_TEMPLATE_ADD, .key = key};
    change.spec.table = state->tables[0].spec;
    if (shared)
        change.spec.table.example.actions[1] =
            (struct dppd_action){.type = DPPD_ACTION_SHARED_COUNT, .conf.shared_count = {99, 0}};
    return change;
}

/** 同一共享对象跨两条规则累计，配置变化不改变旧规则版本或清空已经累计的值 */
static void test_live(const char *path)
{
    struct dppd_flow_state *state = flow_state_fixture(0);
    struct dppd_flow_store *store = NULL;
    assert(dppd_flow_state_save(path, state) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
    struct dppd_flow_config_change changes[2] = {
        {.operation = DPPD_FLOW_CONFIG_COUNT_ADD, .key = 99, .spec.count = {7, DPPD_RULE_DOMAIN_INGRESS}},
        add_template(state, 101, true)};
    struct dppd_flow_config_result result;
    changes[0].spec.count.domain = (enum dppd_rule_domain) - 1;
    assert(dppd_flow_store_configure(store, 91, changes, 2, &result) == -EINVAL);
    changes[0].spec.count.domain = DPPD_RULE_DOMAIN_INGRESS;
    assert(dppd_flow_store_configure(store, 90, changes, 2, &result) == -ESTALE);
    assert(dppd_flow_store_configure(store, 91, changes, 2, &result) == 0 && result.applied && result.durable);
    assert(result.generations[0] == 50 && result.revision == 92);
    struct dppd_flow_config_page page;
    assert(
        dppd_flow_store_configuration(store, 92, 0, 0, &page) == 0 && page.table_count == 2 && page.shared_count == 1);
    struct dppd_flow_state_rule rules[2] = {{.table_key = 101}, {.table_key = 101}};
    for (uint32_t i = 0; i < 2; ++i) {
        rules[i].rule = page.tables[1].spec.example;
        rules[i].rule.id = i + 1U;
    }
    struct dppd_flow_job_id job;
    assert(dppd_flow_store_submit(store, 0, rules, 2, 1, &job) == 0);
    assert(dppd_flow_store_configure(store, 92, changes, 2, &result) == -EBUSY);
    struct dppd_flow_job_result installed = finish(store, &job);
    assert(installed.applied);
    struct dppd_software_backend *backend = dppd_flow_store_software(store);
    struct dppd_packet packet = {.packet_len = 64};
    struct dppd_software_decision decision;
    assert(dppd_software_backend_worker_register(backend, 0) == 0);
    for (uint32_t i = 0; i < 3; ++i)
        dppd_software_backend_decide(backend, 7, &packet, &decision);
    assert(decision.matched && decision.drop);
    dppd_software_backend_worker_quiescent(backend, 0);
    dppd_software_backend_worker_unregister(backend, 0);
    uint64_t hits, bytes;
    assert(dppd_flow_store_query_shared(store, 99, 50, &hits, &bytes) == 0 && hits == 3 && bytes == 192);
    struct dppd_flow_store_status status;
    dppd_flow_store_status(store, &status);
    changes[0] = (struct dppd_flow_config_change){.operation = DPPD_FLOW_CONFIG_TEMPLATE_DELETE, .key = 101};
    assert(dppd_flow_store_configure(store, status.service.revision, changes, 1, &result) == -EBUSY);
    changes[0] = (struct dppd_flow_config_change){.operation = DPPD_FLOW_CONFIG_COUNT_DELETE,
        .key = 99,
        .expected_generation = 50};
    assert(dppd_flow_store_configure(store, status.service.revision, changes, 1, &result) == -EBUSY);
    struct dppd_flow_remove_request remove = {1, installed.generations[0]};
    assert(dppd_flow_store_remove(store, 0, &remove, 1, 1, &job) == 0 && finish(store, &job).applied);
    assert(dppd_software_backend_worker_register(backend, 0) == 0);
    for (uint32_t i = 0; i < 4; ++i)
        dppd_software_backend_decide(backend, 7, &packet, &decision);
    dppd_software_backend_worker_quiescent(backend, 0);
    dppd_software_backend_worker_unregister(backend, 0);
    assert(dppd_flow_store_query_shared(store, 99, 50, &hits, &bytes) == 0 && hits == 7 && bytes == 448);
    assert(dppd_flow_store_close(&store) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
    assert(dppd_flow_store_query_shared(store, 99, 50, &hits, &bytes) == 0 && hits == 0 && bytes == 0);
    struct dppd_rule rule;
    assert(dppd_flow_store_get(store, 2, &rule) == 0 && rule.generation == installed.generations[1]);
    backend = dppd_flow_store_software(store);
    assert(dppd_software_backend_worker_register(backend, 0) == 0);
    remove = (struct dppd_flow_remove_request){2, rule.generation};
    assert(dppd_flow_store_remove(store, 0, &remove, 1, 1, &job) == 0 && finish(store, &job).applied);
    dppd_flow_store_status(store, &status);
    changes[1] = (struct dppd_flow_config_change){.operation = DPPD_FLOW_CONFIG_TEMPLATE_DELETE, .key = 101};
    /** 规则已摘下仍不能释放旧快照的计数指针，读者跨过静默点后才允许完成回收 */
    assert(dppd_flow_store_configure(store, status.service.revision, changes, 2, &result) == -EUCLEAN);
    assert(result.applied && result.durable);
    dppd_software_backend_worker_quiescent(backend, 0);
    dppd_software_backend_worker_unregister(backend, 0);
    assert(dppd_flow_store_retry(store) == 0);
    assert(dppd_flow_store_query_shared(store, 99, 50, &hits, &bytes) == -ESTALE);
    changes[0] = (struct dppd_flow_config_change){.operation = DPPD_FLOW_CONFIG_COUNT_ADD,
        .key = 99,
        .spec.count = {7, DPPD_RULE_DOMAIN_INGRESS}};
    assert(dppd_flow_store_configure(store, result.revision, changes, 1, &result) == 0 && result.generations[0] == 51);
    assert(dppd_flow_store_close(&store) == 0);
    dppd_flow_state_free(&state);
}

/** 准备残留与提交后残留都保留凭据，写盘故障区分尚未应用和已经采用的新配置 */
static void test_failures(const char *path)
{
    struct dppd_flow_state *state = flow_state_fixture(0);
    struct dppd_flow_store *store = NULL;
    assert(dppd_flow_state_save(path, state) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
    struct dppd_flow_config_change changes[2] = {
        {.operation = DPPD_FLOW_CONFIG_COUNT_ADD, .key = 99, .spec.count = {7, DPPD_RULE_DOMAIN_INGRESS}},
        add_template(state, 101, true)};
    struct dppd_flow_config_result result;
    fail_sync = true;
    assert(dppd_flow_store_configure(store, 91, changes, 2, &result) == -EUCLEAN && !result.applied);
    fail_sync = false;
    assert(dppd_flow_store_flush(store) == 0);
    fail_created = fail_destroy = true;
    assert(dppd_flow_store_configure(store, 91, changes, 2, &result) == -EUCLEAN && !result.applied);
    struct dppd_flow_store_status status;
    dppd_flow_store_status(store, &status);
    assert(status.residual && !status.ready && status.service.revision == 91);
    assert(dppd_flow_store_retry(store) == -EUCLEAN);
    fail_created = fail_destroy = false;
    assert(dppd_flow_store_retry(store) == 0);
    assert(dppd_flow_store_configure(store, 91, changes, 2, &result) == 0 && result.generations[0] == 52);
    changes[0] = add_template(state, 102, false);
    fail_sync = true;
    assert(dppd_flow_store_configure(store, result.revision, changes, 1, &result) == -EUCLEAN && result.applied &&
        !result.durable);
    fail_sync = false;
    assert(dppd_flow_store_flush(store) == 0);
    changes[0] = (struct dppd_flow_config_change){.operation = DPPD_FLOW_CONFIG_TEMPLATE_DELETE, .key = 102};
    fail_destroy = true;
    assert(dppd_flow_store_configure(store, result.revision, changes, 1, &result) == -EUCLEAN && result.applied &&
        result.durable);
    fail_destroy = false;
    assert(dppd_flow_store_retry(store) == 0);
    assert(dppd_flow_store_close(&store) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
    struct dppd_flow_config_page page;
    assert(dppd_flow_store_configuration(store, UINT64_MAX, 0, 0, &page) == 0 && page.table_count == 2 &&
        page.shared_count == 1);
    assert(dppd_flow_store_close(&store) == 0);
    dppd_flow_state_free(&state);
}

/** 先建立新定义，再精确迁移现有规则，旧共享累计保留，新对象独立从零开始 */
static void test_migration(const char *path)
{
    struct dppd_flow_state *state = flow_state_fixture(0);
    struct dppd_flow_store *store = NULL;
    assert(dppd_flow_state_save(path, state) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
    struct dppd_flow_config_change changes[2] = {
        {.operation = DPPD_FLOW_CONFIG_COUNT_ADD, .key = 99, .spec.count = {7, DPPD_RULE_DOMAIN_INGRESS}},
        add_template(state, 101, true)};
    struct dppd_flow_config_result configured;
    assert(dppd_flow_store_configure(store, 91, changes, 2, &configured) == 0);
    struct dppd_flow_config_page page;
    assert(dppd_flow_store_configuration(store, UINT64_MAX, 100, 0, &page) == 0);
    struct dppd_flow_state_rule rule = {.table_key = 101, .rule = page.tables[0].spec.example};
    rule.rule.id = 1;
    struct dppd_flow_job_id job;
    assert(dppd_flow_store_submit(store, 0, &rule, 1, 1, &job) == 0);
    struct dppd_flow_job_result result = finish(store, &job);
    uint64_t old_generation = result.generations[0];
    struct dppd_software_backend *backend = dppd_flow_store_software(store);
    struct dppd_packet packet = {.packet_len = 64};
    struct dppd_software_decision decision;
    assert(dppd_software_backend_worker_register(backend, 0) == 0);
    dppd_software_backend_decide(backend, 7, &packet, &decision);
    dppd_software_backend_worker_quiescent(backend, 0);
    dppd_software_backend_worker_unregister(backend, 0);
    struct dppd_flow_store_status status;
    dppd_flow_store_status(store, &status);
    changes[0].key = 100;
    changes[1].key = 102;
    changes[1].spec.table.example.actions[1].conf.shared_count.id = 100;
    assert(dppd_flow_store_configure(store, status.service.revision, changes, 2, &configured) == 0);
    assert(dppd_flow_store_configuration(store, UINT64_MAX, 101, 0, &page) == 0);
    rule.table_key = 102;
    rule.rule = page.tables[0].spec.example;
    rule.rule.id = 1;
    assert(dppd_flow_store_update(store, 1, &rule, &old_generation, 1, 1, &job) == -EINVAL);
    rule.queue = 1;
    assert(dppd_flow_store_update(store, 1, &rule, &old_generation, 1, 1, &job) == 0 && finish(store, &job).applied);
    assert(dppd_software_backend_worker_register(backend, 0) == 0);
    dppd_software_backend_decide(backend, 7, &packet, &decision);
    dppd_software_backend_worker_quiescent(backend, 0);
    dppd_software_backend_worker_unregister(backend, 0);
    uint64_t hits, bytes;
    assert(dppd_flow_store_query_shared(store, 99, 50, &hits, &bytes) == 0 && hits == 1);
    assert(dppd_flow_store_query_shared(store, 100, 51, &hits, &bytes) == 0 && hits == 1);
    dppd_flow_store_status(store, &status);
    changes[0] = (struct dppd_flow_config_change){.operation = DPPD_FLOW_CONFIG_COUNT_DELETE,
        .key = 99,
        .expected_generation = 50};
    changes[1] = (struct dppd_flow_config_change){.operation = DPPD_FLOW_CONFIG_TEMPLATE_DELETE, .key = 101};
    assert(dppd_flow_store_configure(store, status.service.revision, changes, 2, &configured) == 0);
    assert(dppd_flow_store_close(&store) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
    assert(dppd_flow_store_query_shared(store, 100, 51, &hits, &bytes) == 0 && hits == 0 && bytes == 0);
    assert(dppd_flow_store_close(&store) == 0);
    dppd_flow_state_free(&state);
}

struct concurrent_change {
    struct dppd_flow_store *store;
    pthread_barrier_t *barrier;
    struct dppd_flow_config_change change;
    int result;
};

static void *configure_thread(void *context)
{
    struct concurrent_change *change = context;
    pthread_barrier_wait(change->barrier);
    struct dppd_flow_config_result result;
    change->result = dppd_flow_store_configure(change->store, 91, &change->change, 1, &result);
    return NULL;
}

/** 两个生产者携带同一个旧版本同时提交，只允许一笔保存成功，另一笔必须报告过期 */
static void test_concurrency(const char *path)
{
    struct dppd_flow_state *state = flow_state_fixture(0);
    struct dppd_flow_store *store = NULL;
    assert(dppd_flow_state_save(path, state) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
    pthread_barrier_t barrier;
    pthread_t threads[2];
    assert(pthread_barrier_init(&barrier, NULL, 3) == 0);
    struct concurrent_change changes[2] = {{store, &barrier, add_template(state, 101, false), 0},
        {store, &barrier, add_template(state, 102, false), 0}};
    for (uint32_t i = 0; i < 2; ++i)
        assert(pthread_create(&threads[i], NULL, configure_thread, &changes[i]) == 0);
    pthread_barrier_wait(&barrier);
    for (uint32_t i = 0; i < 2; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    assert((changes[0].result == 0 && changes[1].result == -ESTALE) ||
        (changes[1].result == 0 && changes[0].result == -ESTALE));
    assert(pthread_barrier_destroy(&barrier) == 0);
    assert(dppd_flow_store_close(&store) == 0);
    dppd_flow_state_free(&state);
}

int main(void)
{
    char directory[] = "/tmp/dppd-online-configuration-XXXXXX", path[128], lock[140];
    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/state", directory);
    snprintf(lock, sizeof(lock), "%s.lock", path);
    test_live(path);
    test_failures(path);
    test_migration(path);
    test_concurrency(path);
    assert(unlink(path) == 0 && unlink(lock) == 0 && rmdir(directory) == 0);
    return 0;
}
