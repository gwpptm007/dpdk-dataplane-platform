#define _GNU_SOURCE
#include "flow_state_fixture.h"
#include "dppd/flow_request.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static bool fail_sync, fail_regular_sync, fail_directory_sync, fail_queue_once;
static uint32_t drain_completions;
int __real_fsync(int fd);
int __wrap_fsync(int fd);
int __wrap_fsync(int fd)
{
    struct stat info;
    assert(fstat(fd, &info) == 0);
    if (fail_sync || (fail_regular_sync && S_ISREG(info.st_mode)) || (fail_directory_sync && S_ISDIR(info.st_mode))) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

int __real_dppd_flow_service_process(struct dppd_flow_service *, uint64_t, uint32_t);
int __wrap_dppd_flow_service_process(struct dppd_flow_service *, uint64_t, uint32_t);
int __wrap_dppd_flow_service_process(struct dppd_flow_service *service, uint64_t now, uint32_t budget)
{
    /** 同一个 store 消费轮次内推进多笔真实任务，检查领取时仍保留各自提交的版本 */
    for (uint32_t step = 0; step < 32; ++step) {
        int rc = __real_dppd_flow_service_process(service, now + step, budget);
        if (drain_completions == 0 || rc != 0)
            return rc;
        struct dppd_flow_service_status status;
        assert(dppd_flow_service_status(service, &status) == 0);
        if (status.completed >= drain_completions)
            return 0;
    }
    assert(!"completion budget exhausted");
    return -ETIMEDOUT;
}

int __real_dppd_async_flow_process(struct dppd_async_flow_queue *, uint64_t, uint64_t, uint32_t);
int __wrap_dppd_async_flow_process(struct dppd_async_flow_queue *, uint64_t, uint64_t, uint32_t);
int __wrap_dppd_async_flow_process(struct dppd_async_flow_queue *queue, uint64_t now, uint64_t timeout, uint32_t budget)
{
    /** 空队列的一次推进故障使真实服务进入隔离，不伪造完成结果或改动内部状态 */
    if (fail_queue_once) {
        fail_queue_once = false;
        return -EIO;
    }
    return __real_dppd_async_flow_process(queue, now, timeout, budget);
}

static struct dppd_flow_request_result finish(struct dppd_flow_store *store, struct dppd_flow_request_key key)
{
    struct dppd_flow_request_result result;
    for (uint32_t i = 0; i < 32; ++i) {
        assert(dppd_flow_store_process(store, 100 + i, 64) >= 0);
        assert(dppd_flow_store_request_get(store, key, &result) == 0);
        if (result.phase == DPPD_FLOW_REQUEST_DONE) {
            assert(result.durable);
            return result;
        }
    }
    assert(false);
    return result;
}

struct producer {
    struct dppd_flow_store *store;
    const struct dppd_flow_request *request;
    pthread_barrier_t *barrier;
};

static void *duplicate(void *argument)
{
    struct producer *p = argument;
    struct dppd_flow_request_result result;
    pthread_barrier_wait(p->barrier);
    assert(dppd_flow_store_request(p->store, p->request, 1, &result) == 0);
    assert(result.phase == DPPD_FLOW_REQUEST_PENDING && result.durable);
    return NULL;
}

static struct dppd_flow_request create_request(const struct dppd_flow_state *state, uint64_t sequence, uint64_t id)
{
    struct dppd_flow_request request = {.key = {901, sequence}, .operation = DPPD_FLOW_REQUEST_CREATE, .count = 1};
    request.payload.change.rules[0] =
        (struct dppd_flow_state_rule){.table_key = 100, .rule = state->tables[0].spec.example};
    request.payload.change.rules[0].rule.id = id;
    return request;
}

/** 两种失败窗口都直接结束子进程，只从真实磁盘文件判断策略与回执的恢复结果 */
static void test_crash_saves(const char *path)
{
    for (uint32_t renamed = 0; renamed < 2; ++renamed) {
        struct dppd_flow_state *state = flow_state_fixture(0);
        assert(dppd_flow_state_save(path, state) == 0);
        struct dppd_flow_request request = create_request(state, 1, 40);
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            struct dppd_flow_store *store = NULL;
            struct dppd_flow_request_result result;
            assert(dppd_flow_store_open(path, NULL, &store) == 0);
            assert(dppd_flow_store_request(store, &request, 1, &result) == 0 && result.durable);
            fail_regular_sync = renamed == 0;
            fail_directory_sync = renamed != 0;
            struct dppd_flow_store_status status;
            for (uint32_t i = 0; i < 32; ++i) {
                (void)dppd_flow_store_process(store, 100 + i, 64);
                assert(dppd_flow_store_status(store, &status) == 0);
                if (status.dirty)
                    break;
            }
            assert(status.dirty && !status.ready);
            assert(dppd_flow_store_request_get(store, request.key, &result) == -EUCLEAN);
            assert(result.phase == DPPD_FLOW_REQUEST_DONE && result.applied && !result.durable);
            /** 不调用 close 或 flush，确保重启不会借用本进程尚未保存的内存回执 */
            _Exit(0);
        }
        int code;
        assert(waitpid(child, &code, 0) == child && WIFEXITED(code) && WEXITSTATUS(code) == 0);
        struct dppd_flow_store *store = NULL;
        struct dppd_flow_request_result result;
        struct dppd_rule rule;
        assert(dppd_flow_store_open(path, NULL, &store) == 0);
        assert(dppd_flow_store_request(store, &request, 1, &result) == 0 && result.durable);
        assert(result.phase == DPPD_FLOW_REQUEST_DONE && result.applied == (renamed != 0));
        assert(result.status == (renamed ? 0 : -ECANCELED));
        assert(result.revision == (renamed ? 92U : 91U));
        assert(dppd_flow_store_get(store, 40, &rule) == (renamed ? 0 : -ENOENT));
        if (renamed)
            assert(rule.generation == 92 && result.generations[0] == 92);
        assert(dppd_flow_store_close(&store) == 0);
        dppd_flow_state_free(&state);
    }
}

/** 同批新共享对象的零代次是请求内容的一部分，解析得到的实际代次不能改变重试内容 */
static void test_shared_canonical(const char *path)
{
    struct dppd_flow_state *state = flow_state_fixture(0);
    assert(dppd_flow_state_save(path, state) == 0);
    struct dppd_flow_store *store = NULL;
    assert(dppd_flow_store_open(path, NULL, &store) == 0);
    struct dppd_flow_request request = {.key = {902, 1},
        .operation = DPPD_FLOW_REQUEST_CONFIGURE,
        .count = 2,
        .expected_revision = 91};
    request.payload.configuration[0] = (struct dppd_flow_config_change){.operation = DPPD_FLOW_CONFIG_COUNT_ADD,
        .key = 99,
        .spec.count = {7, DPPD_RULE_DOMAIN_INGRESS}};
    request.payload.configuration[1] = (struct dppd_flow_config_change){.operation = DPPD_FLOW_CONFIG_TEMPLATE_ADD,
        .key = 101,
        .spec.table = state->tables[0].spec};
    struct dppd_rule *example = &request.payload.configuration[1].spec.table.example;
    example->actions[1] = (struct dppd_action){.type = DPPD_ACTION_SHARED_COUNT, .conf.shared_count = {99, 0}};
    struct dppd_flow_request_result result;
    assert(dppd_flow_store_request(store, &request, 1, &result) == 0);
    assert(result.phase == DPPD_FLOW_REQUEST_DONE && result.applied && result.durable && result.revision == 92);
    assert(result.generations[0] == 50 && example->actions[1].conf.shared_count.generation == 0);
    /** 未使用的数组尾部不影响语义，不能靠填充字节制造同键不同请求 */
    request.payload.configuration[7].key = UINT64_MAX;
    example->actions[DPPD_RULE_MAX_ACTIONS - 1].conf.mark_id = 73;
    assert(dppd_flow_store_request(store, &request, 1, &result) == 0 && result.generations[0] == 50);
    assert(dppd_flow_store_close(&store) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
    assert(dppd_flow_store_request(store, &request, 1, &result) == 0 && result.revision == 92);
    struct dppd_flow_config_page page;
    assert(dppd_flow_store_configuration(store, 92, 100, 0, &page) == 0);
    assert(page.shared_count == 1 && page.counts[0].id == 99 && page.counts[0].generation == 50);
    assert(page.tables_returned == 1 && page.tables[0].key == 101 &&
        page.tables[0].spec.example.actions[1].conf.shared_count.generation == 50);
    example->actions[1].conf.shared_count.generation = 50;
    assert(dppd_flow_store_request(store, &request, 1, &result) == -EEXIST);
    example->actions[1].conf.shared_count.generation = 0;
    assert(dppd_flow_store_request(store, &request, 1, &result) == 0);
    assert(dppd_flow_store_close(&store) == 0);
    dppd_flow_state_free(&state);
}

static void test_completion_revisions(const char *path)
{
    struct dppd_flow_state *state = flow_state_fixture(0);
    assert(dppd_flow_state_save(path, state) == 0);
    struct dppd_flow_store *store = NULL;
    assert(dppd_flow_store_open(path, NULL, &store) == 0);
    struct dppd_flow_request first = create_request(state, 1, 41), second = create_request(state, 2, 42);
    struct dppd_flow_request_result a, b;
    assert(dppd_flow_store_request(store, &first, 1, &a) == 0);
    assert(dppd_flow_store_request(store, &second, 1, &b) == 0);
    drain_completions = 2;
    assert(dppd_flow_store_process(store, 100, 64) == 0);
    drain_completions = 0;
    assert(dppd_flow_store_request_get(store, first.key, &a) == 0 && a.phase == DPPD_FLOW_REQUEST_DONE);
    assert(dppd_flow_store_request_get(store, second.key, &b) == 0 && b.phase == DPPD_FLOW_REQUEST_DONE);
    assert(a.applied && a.durable && a.revision == 92 && a.generations[0] == 92);
    assert(b.applied && b.durable && b.revision == 93 && b.generations[0] == 93);
    assert(dppd_flow_store_close(&store) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
    assert(dppd_flow_store_request(store, &first, 100, &a) == 0 && a.revision == 92);
    assert(dppd_flow_store_request(store, &second, 100, &b) == 0 && b.revision == 93);
    assert(dppd_flow_store_close(&store) == 0);
    dppd_flow_state_free(&state);
}

/** 只有回执尚未保存也必须封闭写入，解除队列隔离不能把这项持久化义务一起抹掉 */
static void test_receipt_isolation(const char *path)
{
    struct dppd_flow_state *state = flow_state_fixture(0);
    assert(dppd_flow_state_save(path, state) == 0);
    struct dppd_flow_store *store = NULL;
    assert(dppd_flow_store_open(path, NULL, &store) == 0);
    struct dppd_flow_request request = create_request(state, 1, 43);
    struct dppd_flow_request_result result;
    assert(dppd_flow_store_request(store, &request, 1, &result) == 0);
    assert(dppd_flow_store_requests_cancel(store) == 0);
    fail_queue_once = true;
    assert(dppd_flow_store_process(store, 100, 64) == -EUCLEAN);
    struct dppd_flow_store_status status;
    assert(dppd_flow_store_status(store, &status) == 0);
    assert(status.dirty && !status.ready && status.service.isolated && status.service.revision == 91);
    assert(dppd_flow_store_request_get(store, request.key, &result) == -EUCLEAN);
    assert(result.phase == DPPD_FLOW_REQUEST_DONE && result.status == -ECANCELED && result.revision == 91);
    assert(!result.applied && !result.durable);
    assert(dppd_flow_store_retry(store) == 0);
    assert(dppd_flow_store_status(store, &status) == 0 && !status.service.isolated && status.dirty && !status.ready);
    assert(dppd_flow_store_close(&store) == -EUCLEAN && store != NULL);
    struct dppd_flow_job_id job;
    assert(dppd_flow_store_submit(store, 0, request.payload.change.rules, 1, 100, &job) == -EUCLEAN);
    struct dppd_flow_request newer = create_request(state, 2, 44);
    assert(dppd_flow_store_request(store, &newer, 100, &result) == -EUCLEAN);
    /** process 和显式 flush 是修复入口，不能被普通写入的 dirty 门禁一并堵住 */
    assert(dppd_flow_store_process(store, 101, 64) == 0);
    assert(dppd_flow_store_status(store, &status) == 0 && status.ready && !status.dirty);
    assert(dppd_flow_store_request_get(store, request.key, &result) == 0 && result.durable && result.revision == 91);
    assert(dppd_flow_store_close(&store) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
    assert(dppd_flow_store_request(store, &request, 100, &result) == 0 && result.status == -ECANCELED);
    struct dppd_rule absent;
    assert(dppd_flow_store_get(store, 43, &absent) == -ENOENT && dppd_flow_store_get(store, 44, &absent) == -ENOENT);
    assert(dppd_flow_store_close(&store) == 0);
    dppd_flow_state_free(&state);
}

int main(void)
{
    char directory[] = "/tmp/dppd-flow-request-XXXXXX", path[128], lock[140];
    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/state", directory);
    snprintf(lock, sizeof(lock), "%s.lock", path);
    test_crash_saves(path);
    test_shared_canonical(path);
    test_completion_revisions(path);
    test_receipt_isolation(path);
    struct dppd_flow_state *state = flow_state_fixture(0);
    assert(dppd_flow_state_save(path, state) == 0);
    struct dppd_flow_store *store = NULL;
    assert(dppd_flow_store_open(path, NULL, &store) == 0);
    struct dppd_flow_request request = {.key = {900, 1}, .operation = DPPD_FLOW_REQUEST_CREATE, .count = 1};
    request.payload.change.rules[0] =
        (struct dppd_flow_state_rule){.table_key = 100, .rule = state->tables[0].spec.example};
    request.payload.change.rules[0].rule.id = 10;
    pthread_barrier_t barrier;
    assert(pthread_barrier_init(&barrier, NULL, 2) == 0);
    struct producer producer = {store, &request, &barrier};
    pthread_t threads[2];
    for (uint32_t i = 0; i < 2; ++i)
        assert(pthread_create(&threads[i], NULL, duplicate, &producer) == 0);
    for (uint32_t i = 0; i < 2; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    pthread_barrier_destroy(&barrier);
    struct dppd_flow_store_status status;
    assert(dppd_flow_store_status(store, &status) == 0 && status.service.jobs == 1);
    struct dppd_flow_request_result result = finish(store, request.key);
    assert(result.status == 0 && result.applied && result.generations[0] == 92);
    assert(dppd_flow_store_request(store, &request, 2, &result) == 0 && result.generations[0] == 92);
    request.payload.change.rules[0].rule.actions[0].conf.mark_id = 43;
    assert(dppd_flow_store_request(store, &request, 2, &result) == -EEXIST);
    request.payload.change.rules[0].rule.actions[0].conf.mark_id = 42;
    assert(dppd_flow_store_close(&store) == 0);
    assert(dppd_flow_store_open(path, NULL, &store) == 0);
    assert(dppd_flow_store_request(store, &request, 2, &result) == 0 && result.generations[0] == 92);
    struct dppd_flow_request original = request;
    request.key.sequence = 2;
    request.operation = DPPD_FLOW_REQUEST_UPDATE;
    request.payload.change.expected[0] = 92;
    request.payload.change.rules[0].rule.actions[0].conf.mark_id = 43;
    assert(dppd_flow_store_request(store, &request, 2, &result) == 0);
    result = finish(store, request.key);
    assert(result.applied && result.status == 0 && result.generations[0] == 93);
    request = (struct dppd_flow_request){.key = {900, 3}, .operation = DPPD_FLOW_REQUEST_REMOVE, .count = 1};
    request.payload.remove[0] = (struct dppd_flow_remove_request){10, 93};
    assert(dppd_flow_store_request(store, &request, 2, &result) == 0);
    result = finish(store, request.key);
    assert(result.applied && result.status == 0);
    assert(dppd_flow_store_request(store, &original, 2, &result) == 0 && result.revision == 92);
    struct dppd_rule absent;
    assert(dppd_flow_store_get(store, 10, &absent) == -ENOENT);

    /** 同一原子文件同时保存模板新增和请求结果，重启查询不能再次推进配置版本 */
    request = (struct dppd_flow_request){.key = {900, 4},
        .operation = DPPD_FLOW_REQUEST_CONFIGURE,
        .count = 1,
        .expected_revision = 94};
    request.payload.configuration[0] = (struct dppd_flow_config_change){.operation = DPPD_FLOW_CONFIG_TEMPLATE_ADD,
        .key = 200,
        .spec.table = state->tables[0].spec};
    assert(dppd_flow_store_request(store, &request, 2, &result) == 0);
    assert(result.phase == DPPD_FLOW_REQUEST_DONE && result.applied && result.durable && result.revision == 95);
    assert(dppd_flow_store_close(&store) == 0);
    assert(dppd_flow_store_open(path, NULL, &store) == 0);
    assert(dppd_flow_store_request(store, &request, 2, &result) == 0 && result.revision == 95);

    /** 受理保存失败不入队，修复后同键仍只返回失败，不能把重试变成一次新创建 */
    request = original;
    request.key.sequence = 5;
    fail_sync = true;
    assert(dppd_flow_store_request(store, &request, 2, &result) == -EUCLEAN && !result.durable);
    assert(dppd_flow_store_status(store, &status) == 0 && status.dirty && status.service.jobs == 0);
    fail_sync = false;
    assert(dppd_flow_store_flush(store) == 0);
    assert(dppd_flow_store_request(store, &request, 2, &result) == 0 && result.status == -EIO && !result.applied);
    request.key.sequence = 6;
    assert(dppd_flow_store_request(store, &request, 2, &result) == 0);
    fail_sync = true;
    for (uint32_t i = 0; i < 32; ++i) {
        (void)dppd_flow_store_process(store, 100 + i, 64);
        assert(dppd_flow_store_status(store, &status) == 0);
        if (status.dirty)
            break;
    }
    assert(status.dirty);
    assert(dppd_flow_store_request_get(store, request.key, &result) == -EUCLEAN && result.applied && !result.durable);
    fail_sync = false;
    assert(dppd_flow_store_flush(store) == 0);
    assert(dppd_flow_store_request_get(store, request.key, &result) == 0 && result.applied && result.durable);
    assert(dppd_flow_store_close(&store) == 0);

    /** 子进程在受理或提交后直接退出，父进程仅凭磁盘证据恢复，不继承任何运行时凭据 */
    for (uint32_t committed = 0; committed < 2; ++committed) {
        request = original;
        request.key.sequence = 7 + committed;
        request.payload.change.rules[0].rule.id = 20 + committed;
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            assert(dppd_flow_store_open(path, NULL, &store) == 0);
            assert(dppd_flow_store_request(store, &request, 2, &result) == 0);
            if (committed)
                (void)finish(store, request.key);
            _Exit(0);
        }
        int code;
        assert(waitpid(child, &code, 0) == child && WIFEXITED(code) && WEXITSTATUS(code) == 0);
        assert(dppd_flow_store_open(path, NULL, &store) == 0);
        assert(dppd_flow_store_request(store, &request, 2, &result) == 0 && result.phase == DPPD_FLOW_REQUEST_DONE);
        assert(result.applied == (committed != 0) && result.status == (committed ? 0 : -ECANCELED));
        assert(dppd_flow_store_get(store, 20 + committed, &absent) == (committed ? 0 : -ENOENT));
        assert(dppd_flow_store_close(&store) == 0);
    }

    /** 回执轮转后仍保存客户端水位，旧序号只能报告过期，绝不能重新提交旧策略 */
    assert(dppd_flow_store_open(path, NULL, &store) == 0);
    for (uint32_t sequence = 9; sequence < 140; ++sequence) {
        request = (struct dppd_flow_request){.key = {900, sequence},
            .operation = DPPD_FLOW_REQUEST_CONFIGURE,
            .count = 1,
            .expected_revision = 1};
        request.payload.configuration[0] =
            (struct dppd_flow_config_change){.operation = DPPD_FLOW_CONFIG_TEMPLATE_DELETE, .key = 999};
        assert(dppd_flow_store_request(store, &request, 2, &result) == 0);
        assert(result.phase == DPPD_FLOW_REQUEST_DONE && result.status == -ESTALE && !result.applied);
    }
    assert(dppd_flow_store_request(store, &original, 2, &result) == -ESTALE);
    assert(dppd_flow_store_request_get(store, original.key, &result) == -ESTALE);
    assert(dppd_flow_store_close(&store) == 0);
    assert(dppd_flow_store_open(path, NULL, &store) == 0);
    assert(dppd_flow_store_request(store, &original, 2, &result) == -ESTALE);
    assert(dppd_flow_store_close(&store) == 0);
    dppd_flow_state_free(&state);
    assert(unlink(path) == 0 && unlink(lock) == 0 && rmdir(directory) == 0);
    return 0;
}
