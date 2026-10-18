#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "flow_state_fixture.h"

static unsigned int fail_sync;
static unsigned int fail_allocation, allocation_calls;
static bool count_allocations;
static uint64_t uncertain_revision;
int __real_dppd_flow_service_status(struct dppd_flow_service *service, struct dppd_flow_service_status *status);
int __wrap_dppd_flow_service_status(struct dppd_flow_service *service, struct dppd_flow_service_status *status);
int __wrap_dppd_flow_service_status(struct dppd_flow_service *service, struct dppd_flow_service_status *status)
{
    int rc = __real_dppd_flow_service_status(service, status);
    if (rc == 0 && uncertain_revision != 0 && status->revision == uncertain_revision)
        status->isolated = true;
    return rc;
}
void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size)
{
    if (count_allocations)
        ++allocation_calls;
    if (fail_allocation != 0 && --fail_allocation == 0)
        return NULL;
    return __real_calloc(count, size);
}
int __real_fsync(int fd);
int __wrap_fsync(int fd);
int __wrap_fsync(int fd)
{
    if (fail_sync != 0 && --fail_sync == 0) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

static struct dppd_flow_job_result finish(struct dppd_flow_store *store, struct dppd_flow_job_id job)
{
    struct dppd_flow_job_result result;
    for (unsigned int i = 0; i < 128; ++i) {
        int rc = dppd_flow_store_collect(store, &job, &result);
        if (rc == 0)
            return result;
        assert(rc == -EAGAIN);
        assert(dppd_flow_store_process(store, 2, 64) == 0);
    }
    assert(!"store job did not finish");
    return result;
}

static void check_packet(struct dppd_flow_store *store, uint64_t generation, uint32_t mark)
{
    struct dppd_software_backend *software = dppd_flow_store_software(store);
    struct dppd_packet packet = {.packet_len = 64};
    struct dppd_software_decision decision;
    uint64_t hits, bytes;
    assert(software != NULL);
    assert(dppd_software_backend_query_count(software, 1, generation, &hits, &bytes) == 0 && hits == 0 && bytes == 0);
    assert(dppd_software_backend_worker_register(software, 0) == 0);
    dppd_software_backend_decide(software, 7, &packet, &decision);
    assert(decision.matched && decision.drop && decision.has_mark && decision.mark_id == mark);
    dppd_software_backend_worker_quiescent(software, 0);
    dppd_software_backend_worker_unregister(software, 0);
    assert(dppd_software_backend_query_count(software, 1, generation, &hits, &bytes) == 0 && hits == 1 && bytes == 64);
}

static void verify_restarted(const char *path)
{
    struct dppd_flow_store *store = NULL;
    struct dppd_flow_store_status status;
    struct dppd_rule rule;
    assert(dppd_flow_store_open(path, NULL, &store) == 0);
    assert(dppd_flow_store_status(store, &status) == 0 && status.ready && !status.dirty);
    assert(status.service.rules == 129 && status.service.revision == 1383 && status.durable_revision == 1383);
    assert(dppd_flow_store_get(store, 1, &rule) == 0 && rule.generation == 1382 && rule.actions[0].conf.mark_id == 99);
    assert(dppd_flow_store_get(store, 129, &rule) == 0 && rule.generation == 1383);
    assert(dppd_flow_store_get(store, 2, &rule) == 0 && rule.generation == 13);
    check_packet(store, 1382, 99);
    assert(dppd_flow_store_close(&store) == 0 && store == NULL);
}

static void test_restart_and_writer_lock(const char *path)
{
    struct dppd_flow_state *state = flow_state_fixture(129);
    struct dppd_flow_store *store = NULL, *other = NULL;
    assert(dppd_flow_state_save(path, state) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
    assert(dppd_flow_store_open(path, NULL, &other) == -EBUSY && other == NULL);
    assert(dppd_flow_state_save(path, state) == -EBUSY);
    pid_t contender = fork();
    assert(contender >= 0);
    if (contender == 0) {
        execl("/proc/self/exe", "test_flow_store", "--locked", path, (char *)NULL);
        _exit(127);
    }
    int contender_status;
    assert(waitpid(contender, &contender_status, 0) == contender && WIFEXITED(contender_status) &&
        WEXITSTATUS(contender_status) == 0);
    check_packet(store, 3, 42);
    assert(dppd_flow_store_close(&store) == 0);
    struct dppd_flow_state *loaded = NULL;
    assert(dppd_flow_state_load(path, &loaded) == 0 && loaded->revision == 1381);
    dppd_flow_state_free(&loaded);
    int ready[2];
    assert(pipe(ready) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(ready[0]);
        assert(dppd_flow_store_open(path, NULL, &store) == 0);
        struct dppd_flow_state_rule updates[2] = {state->rules[0], state->rules[128]};
        uint64_t expected[2] = {3, 1283};
        for (unsigned int i = 0; i < 2; ++i) {
            updates[i].queue = 1;
            updates[i].rule.generation = 0;
            updates[i].rule.actions[0].conf.mark_id = 99;
        }
        struct dppd_flow_job_id job;
        assert(dppd_flow_store_update(store, 1, updates, expected, 2, 1, &job) == 0);
        assert(finish(store, job).applied);
        check_packet(store, 1382, 99);
        assert(write(ready[1], "1", 1) == 1);
        for (;;)
            pause();
    }
    close(ready[1]);
    char signal;
    assert(read(ready[0], &signal, 1) == 1);
    close(ready[0]);
    assert(kill(child, SIGKILL) == 0);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    assert(dppd_flow_state_load(path, &loaded) == 0 && loaded->revision == 1383);
    assert(loaded->rules[0].queue == 1 && loaded->rules[128].queue == 1);
    dppd_flow_state_free(&loaded);
    /** exec 清除整个进程地址空间，验证恢复不依赖父进程里的指针、计数或静态分配器 */
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        execl("/proc/self/exe", "test_flow_store", "--verify", path, (char *)NULL);
        _exit(127);
    }
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    dppd_flow_state_free(&state);
}

static void test_durable_receipts(const char *path)
{
    for (unsigned int fault = 1; fault <= 2; ++fault) {
        struct dppd_flow_state *state = flow_state_fixture(2), *loaded = NULL;
        struct dppd_flow_store *store = NULL;
        assert(dppd_flow_state_save(path, state) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
        struct dppd_flow_state_rule update = state->rules[0];
        uint64_t expected = 3;
        update.rule.generation = 0;
        update.rule.actions[0].conf.mark_id = 99;
        struct dppd_flow_job_id job, unchanged = {.owner = 999};
        assert(dppd_flow_store_update(store, 0, &update, &expected, 1, 1, &job) == 0);
        fail_sync = fault;
        struct dppd_flow_store_status status;
        for (unsigned int i = 0; i < 32; ++i) {
            dppd_flow_store_process(store, 2, 64);
            assert(dppd_flow_store_status(store, &status) == 0);
            if (status.dirty)
                break;
        }
        assert(status.dirty && !status.ready && status.durable_revision == 111 && status.service.revision == 112);
        struct dppd_flow_job_result result = {.status = 999};
        assert(dppd_flow_store_collect(store, &job, &result) == -EUCLEAN && result.status == 999);
        assert(dppd_flow_store_submit(store, 0, &update, 1, 1, &unchanged) == -EUCLEAN && unchanged.owner == 999);
        assert(dppd_flow_store_close(&store) == -EUCLEAN && store != NULL);
        assert(dppd_flow_state_load(path, &loaded) == 0 && loaded->revision == (fault == 1 ? 111 : 112));
        dppd_flow_state_free(&loaded);
        assert(dppd_flow_store_flush(store) == 0 && finish(store, job).applied);
        assert(dppd_flow_store_status(store, &status) == 0 && status.ready && status.durable_revision == 112);
        assert(dppd_flow_store_close(&store) == 0);
        assert(dppd_flow_store_open(path, NULL, &store) == 0);
        struct dppd_rule rule;
        assert(
            dppd_flow_store_get(store, 1, &rule) == 0 && rule.generation == 112 && rule.actions[0].conf.mark_id == 99);
        check_packet(store, 112, 99);
        assert(dppd_flow_store_close(&store) == 0);
        dppd_flow_state_free(&state);
    }
}

struct faults {
    struct dppd_async_flow_driver base;
    unsigned int creates, fail_create;
    bool fail_destroy, withhold;
};

static int create(void *context, uint16_t port, uint32_t queue, void *table, const struct dppd_rule *rule,
    const struct dppd_flow_binding *binding, uintptr_t tag, void **output)
{
    struct faults *f = context;
    if (++f->creates == f->fail_create)
        return -ENOMEM;
    return f->base.create(f->base.context, port, queue, table, rule, binding, tag, output);
}

static int destroy(void *context, uint16_t port, uint32_t queue, void *object, uintptr_t tag)
{
    struct faults *f = context;
    return f->fail_destroy ? -EIO : f->base.destroy(f->base.context, port, queue, object, tag);
}

static int push(void *context, uint16_t port, uint32_t queue)
{
    struct faults *f = context;
    return f->base.push(f->base.context, port, queue);
}

static int pull(void *context, uint16_t port, uint32_t queue, struct dppd_async_driver_result *results, uint16_t count)
{
    struct faults *f = context;
    return f->withhold ? 0 : f->base.pull(f->base.context, port, queue, results, count);
}

static void release(void *context, void *object)
{
    struct faults *f = context;
    f->base.release(f->base.context, object);
}

static void test_restore_failures(const char *path)
{
    for (unsigned int fault = 0; fault < 3; ++fault) {
        struct dppd_flow_state *state = flow_state_fixture(fault == 0 ? 129 : 4);
        struct dppd_flow_store *store = NULL, *other = NULL;
        struct dppd_software_async_flows *software = NULL;
        struct faults f = {0};
        state->config.timeout_ns = fault == 2 ? UINT64_C(10000000) : UINT64_C(1000000000);
        assert(dppd_flow_state_save(path, state) == 0);
        assert(dppd_software_async_flows_create(&software, &f.base) == 0);
        struct dppd_flow_store_runtime runtime = {
            .flows =
                {.context = &f, .create = create, .destroy = destroy, .push = push, .pull = pull, .release = release}};
        f.fail_create = fault == 0 ? 70 : fault == 1 ? 2 : 0;
        f.fail_destroy = fault == 1;
        f.withhold = fault == 2;
        int rc = dppd_flow_store_open(path, &runtime, &store);
        if (fault == 0) {
            if (rc != -ENOMEM || store != NULL) {
                struct dppd_flow_store_status status;
                dppd_flow_store_status(store, &status);
                fprintf(stderr, "restore fault rc=%d creates=%u rows=%u jobs=%u isolated=%d error=%d\n", rc, f.creates,
                    status.service.rules, status.service.jobs, status.service.isolated, status.last_error);
            }
            assert(rc == -ENOMEM && store == NULL);
        } else {
            assert(rc == -EUCLEAN && store != NULL && dppd_flow_store_software(store) == NULL);
            struct dppd_flow_store_status status;
            assert(dppd_flow_store_status(store, &status) == 0 && !status.ready && status.residual);
            assert(dppd_flow_store_open(path, NULL, &other) == -EBUSY && other == NULL);
            f.fail_destroy = f.withhold = false;
            for (unsigned int attempt = 0; attempt < 8; ++attempt) {
                rc = dppd_flow_store_close(&store);
                if (rc == 0)
                    break;
                assert(rc == -EUCLEAN && store != NULL);
            }
            if (rc != 0) {
                dppd_flow_store_status(store, &status);
                fprintf(stderr, "close fault=%u rc=%d rows=%u jobs=%u isolated=%d error=%d\n", fault, rc,
                    status.service.rules, status.service.jobs, status.service.isolated, status.last_error);
            }
            assert(rc == 0 && store == NULL);
        }
        assert(dppd_software_async_flows_close(&software) == 0);
        assert(dppd_flow_store_open(path, NULL, &store) == 0);
        assert(dppd_flow_store_close(&store) == 0);
        dppd_flow_state_free(&state);
    }
}

static void test_exhausted_revision_can_close(const char *path)
{
    struct dppd_flow_state *state = flow_state_fixture(2);
    struct dppd_flow_store *store = NULL;
    state->revision = UINT64_MAX - 1U;
    state->rules[0].rule.generation = UINT64_MAX - 2U;
    state->rules[1].rule.generation = UINT64_MAX - 1U;
    assert(dppd_flow_state_save(path, state) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
    struct dppd_flow_state_rule rule = state->rules[0];
    rule.rule.id = 3;
    rule.rule.generation = 0;
    struct dppd_flow_job_id job;
    assert(dppd_flow_store_submit(store, 0, &rule, 1, 1, &job) == 0);
    assert(finish(store, job).status == -EOVERFLOW);
    assert(dppd_flow_store_close(&store) == 0);
    dppd_flow_state_free(&state);
}

/** 每个 calloc 位置都独立失败一次，恢复未完成时必须撤销前缀并释放文件锁，不能留下悬空对象 */
static void test_restore_allocation_failures(const char *path)
{
    struct dppd_flow_state *state = flow_state_fixture(4);
    struct dppd_flow_store *store = NULL;
    assert(dppd_flow_state_save(path, state) == 0);
    allocation_calls = 0;
    count_allocations = true;
    assert(dppd_flow_store_open(path, NULL, &store) == 0);
    count_allocations = false;
    unsigned int calls = allocation_calls;
    assert(calls > 20 && calls < 200);
    assert(dppd_flow_store_close(&store) == 0);
    for (unsigned int i = 1; i <= calls; ++i) {
        fail_allocation = i;
        int rc = dppd_flow_store_open(path, NULL, &store);
        assert(rc == -ENOMEM && store == NULL && fail_allocation == 0);
    }
    assert(dppd_flow_store_open(path, NULL, &store) == 0 && dppd_flow_store_close(&store) == 0);
    dppd_flow_state_free(&state);
}

static void test_relative_path_survives_chdir(const char *directory, const char *path)
{
    char *previous = getcwd(NULL, 0);
    struct dppd_flow_state *state = flow_state_fixture(2), *loaded = NULL;
    struct dppd_flow_store *store = NULL;
    assert(previous != NULL && dppd_flow_state_save(path, state) == 0);
    assert(chdir(directory) == 0 && dppd_flow_store_open("state", NULL, &store) == 0);
    assert(chdir("/") == 0);
    struct dppd_flow_state_rule update = state->rules[0];
    update.rule.generation = 0;
    update.rule.actions[0].conf.mark_id = 88;
    uint64_t expected = 3;
    struct dppd_flow_job_id job;
    assert(dppd_flow_store_update(store, 0, &update, &expected, 1, 1, &job) == 0);
    assert(finish(store, job).applied && dppd_flow_store_close(&store) == 0);
    assert(dppd_flow_state_load(path, &loaded) == 0 && loaded->rules[0].rule.actions[0].conf.mark_id == 88);
    assert(chdir(previous) == 0);
    free(previous);
    dppd_flow_state_free(&loaded);
    dppd_flow_state_free(&state);
}

/** 下层报告修订号已变但发布仍不确定时，不能用普通 flush 产生一份貌似可靠的新配置 */
static void test_unknown_publication_preserves_checkpoint(const char *path)
{
    struct dppd_flow_state *state = flow_state_fixture(2);
    assert(dppd_flow_state_save(path, state) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        struct dppd_flow_store *store = NULL;
        assert(dppd_flow_store_open(path, NULL, &store) == 0);
        struct dppd_flow_state_rule update = state->rules[0];
        update.rule.generation = 0;
        update.rule.actions[0].conf.mark_id = 99;
        uint64_t expected = 3;
        struct dppd_flow_job_id job;
        assert(dppd_flow_store_update(store, 0, &update, &expected, 1, 1, &job) == 0);
        uncertain_revision = 112;
        struct dppd_flow_store_status status;
        for (unsigned int i = 0; i < 32; ++i) {
            dppd_flow_store_process(store, 2, 64);
            dppd_flow_store_status(store, &status);
            if (status.residual)
                break;
        }
        assert(status.residual && !status.ready && status.durable_revision == 111);
        struct dppd_flow_job_result result = {.status = 999};
        assert(dppd_flow_store_flush(store) == -EUCLEAN);
        assert(dppd_flow_store_collect(store, &job, &result) == -EUCLEAN && result.status == 999);
        _exit(0);
    }
    int result;
    assert(waitpid(child, &result, 0) == child && WIFEXITED(result) && WEXITSTATUS(result) == 0);
    struct dppd_flow_store *store = NULL;
    struct dppd_rule restored;
    assert(dppd_flow_store_open(path, NULL, &store) == 0);
    assert(dppd_flow_store_get(store, 1, &restored) == 0 && restored.generation == 3 &&
        restored.actions[0].conf.mark_id == 42);
    assert(dppd_flow_store_close(&store) == 0);
    dppd_flow_state_free(&state);
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--locked") == 0) {
        struct dppd_flow_store *store = NULL;
        assert(dppd_flow_store_open(argv[2], NULL, &store) == -EBUSY && store == NULL);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--verify") == 0) {
        verify_restarted(argv[2]);
        return 0;
    }
    char directory[] = "/tmp/dppd-flow-store-XXXXXX", path[256], lock[270];
    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/state", directory);
    snprintf(lock, sizeof(lock), "%s.lock", path);
    if (argc == 2 && strcmp(argv[1], "--scale") == 0) {
        struct dppd_flow_state *state = flow_state_fixture(16384);
        struct dppd_flow_store *store = NULL;
        struct dppd_flow_store_status status;
        state->config.timeout_ns = UINT64_C(1000000000);
        assert(dppd_flow_state_save(path, state) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
        assert(dppd_flow_store_status(store, &status) == 0 && status.ready && status.service.rules == 16384);
        for (unsigned int i = 0; i < 16384; ++i) {
            struct dppd_rule rule;
            assert(dppd_flow_store_get(store, i + 1U, &rule) == 0 && rule.generation == i * 10U + 3);
        }
        assert(dppd_flow_store_close(&store) == 0);
        struct dppd_flow_state *loaded = NULL;
        assert(dppd_flow_state_load(path, &loaded) == 0 && loaded->rule_count == 16384 && loaded->revision == 163931);
        dppd_flow_state_free(&loaded);
        dppd_flow_state_free(&state);
        unlink(path);
        unlink(lock);
        assert(rmdir(directory) == 0);
        puts("PASS M5 16384 rules exact generations replay queried teardown and durable configuration retained");
        return 0;
    }
    test_restart_and_writer_lock(path);
    test_durable_receipts(path);
    test_restore_failures(path);
    test_exhausted_revision_can_close(path);
    test_restore_allocation_failures(path);
    test_relative_path_survives_chdir(directory, path);
    test_unknown_publication_preserves_checkpoint(path);
    unlink(path);
    unlink(lock);
    assert(rmdir(directory) == 0);
    return 0;
}
