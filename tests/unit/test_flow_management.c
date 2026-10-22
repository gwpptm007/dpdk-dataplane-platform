#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "flow_state_fixture.h"
#include "dppd/flow_management.h"

static bool fail_sync;
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

static struct dppd_management_response exchange(struct dppd_flow_management *flows,
    struct dppd_management_request *request)
{
    struct dppd_management_response response;
    assert(dppd_flow_management_handle(flows, NULL, request, &response) == 0);
    assert(response.version == DPPD_MANAGEMENT_VERSION && response.request_id == request->request_id);
    return response;
}

int main(void)
{
    char directory[] = "/tmp/dppd-flow-management-XXXXXX", path[128], lock[140];
    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/state", directory);
    snprintf(lock, sizeof(lock), "%s.lock", path);
    struct dppd_flow_state *state = flow_state_fixture(0);
    struct dppd_flow_store *store = NULL;
    assert(dppd_flow_state_save(path, state) == 0);
    assert(dppd_flow_store_open(path, NULL, &store) == 0);
    struct dppd_flow_management *flows = calloc(1, sizeof(*flows));
    assert(flows != NULL && dppd_flow_management_init(flows, store) == 0 && flows->session != 0);
    struct dppd_management_request request = {.version = DPPD_MANAGEMENT_VERSION,
        .size = sizeof(request),
        .operation = DPPD_MANAGEMENT_FLOW_CREATE,
        .request_id = 99};
    request.payload.flow_change.count = 1;
    request.payload.flow_change.rules[0] = (struct dppd_flow_state_rule){.table_key = state->tables[0].key};
    struct dppd_rule *rule = &request.payload.flow_change.rules[0].rule;
    *rule = state->tables[0].spec.example;
    rule->id = 10;
    struct dppd_management_response response = exchange(flows, &request);
    assert(response.status == 0);
    struct dppd_management_flow_token token = response.payload.flow_token;
    request.operation = DPPD_MANAGEMENT_FLOW_COLLECT;
    request.payload.flow_token = token;
    assert(exchange(flows, &request).status == -EAGAIN);
    fail_sync = true;
    for (uint32_t i = 0; i < 16; ++i) {
        (void)dppd_flow_store_process(store, 2, 64);
        struct dppd_flow_store_status status;
        assert(dppd_flow_store_status(store, &status) == 0);
        if (status.dirty)
            break;
    }
    assert(exchange(flows, &request).status == -EUCLEAN);
    request.operation = DPPD_MANAGEMENT_FLOW_STATUS;
    response = exchange(flows, &request);
    assert(response.status == 0 && response.payload.flow_status.status.dirty);
    assert(!response.payload.flow_status.status.ready);
    request.operation = DPPD_MANAGEMENT_HEALTH_GET;
    response = exchange(flows, &request);
    assert(!response.payload.health.ready && response.payload.health.persistence_dirty);
    fail_sync = false;
    request.operation = DPPD_MANAGEMENT_PERSISTENCE_FLUSH;
    assert(exchange(flows, &request).status == 0);
    request.operation = DPPD_MANAGEMENT_FLOW_COLLECT;
    request.payload.flow_token = token;
    response = exchange(flows, &request);
    assert(response.status == 0 && response.payload.flow_result.status == 0 && response.payload.flow_result.applied);
    assert(response.payload.flow_result.count == 1 && response.payload.flow_result.ids[0] == 10);
    assert(exchange(flows, &request).status == -ENOENT);
    /** 整批更新包含一个过期条件时不能发布另一条，最终失败与受理成功必须分开 */
    struct dppd_rule original;
    assert(dppd_flow_store_get(store, 10, &original) == 0);
    request.operation = DPPD_MANAGEMENT_FLOW_CREATE;
    memset(&request.payload, 0, sizeof(request.payload));
    request.payload.flow_change.count = 2;
    for (uint32_t i = 0; i < 2; ++i) {
        request.payload.flow_change.rules[i] =
            (struct dppd_flow_state_rule){.table_key = state->tables[0].key, .rule = state->tables[0].spec.example};
        request.payload.flow_change.rules[i].rule.id = 11 + i;
    }
    response = exchange(flows, &request);
    assert(response.status == 0);
    struct dppd_management_flow_token batch = response.payload.flow_token;
    for (uint32_t i = 0; i < 16; ++i)
        assert(dppd_flow_store_process(store, 2, 64) == 0);
    request.operation = DPPD_MANAGEMENT_FLOW_COLLECT;
    request.payload.flow_token = batch;
    response = exchange(flows, &request);
    assert(response.status == 0 && response.payload.flow_result.applied && response.payload.flow_result.count == 2);
    struct dppd_flow_state_rule page[1];
    uint32_t count;
    uint64_t revision;
    bool more;
    assert(dppd_flow_store_list(store, 0, UINT64_MAX, page, 1, &count, &revision, &more) == 0);
    assert(count == 1 && more && page[0].rule.id == 10);
    assert(dppd_flow_store_list(store, 10, revision, page, 1, &count, &revision, &more) == 0);
    assert(count == 1 && more && page[0].rule.id == 11);
    assert(dppd_flow_store_list(store, 11, revision + 1, page, 1, &count, &revision, &more) == -ESTALE);
    request.operation = DPPD_MANAGEMENT_FLOW_UPDATE;
    memset(&request.payload, 0, sizeof(request.payload));
    request.payload.flow_change.count = 2;
    for (uint32_t i = 0; i < 2; ++i) {
        struct dppd_rule rule;
        assert(dppd_flow_store_get(store, 10 + i, &rule) == 0);
        request.payload.flow_change.rules[i] = (struct dppd_flow_state_rule){.table_key = 100, .rule = rule};
        request.payload.flow_change.rules[i].rule.generation = 0;
        request.payload.flow_change.rules[i].rule.actions[0].conf.mark_id = 99;
        request.payload.flow_change.expected[i] = rule.generation + i;
    }
    response = exchange(flows, &request);
    if (response.status == 0) {
        batch = response.payload.flow_token;
        for (uint32_t i = 0; i < 16; ++i)
            assert(dppd_flow_store_process(store, 2, 64) == 0);
        request.operation = DPPD_MANAGEMENT_FLOW_COLLECT;
        request.payload.flow_token = batch;
        response = exchange(flows, &request);
        assert(
            response.status == 0 && !response.payload.flow_result.applied && response.payload.flow_result.status != 0);
    } else
        assert(response.status == -ESTALE);
    assert(dppd_flow_store_get(store, 10, &original) == 0 && original.actions[0].conf.mark_id == 42);
    uint64_t hits, bytes;
    assert(dppd_flow_store_query_count(store, 10, original.generation, &hits, &bytes) == 0 && hits == 0 && bytes == 0);
    request.operation = DPPD_MANAGEMENT_FLOW_COLLECT;
    request.payload.flow_token = token;
    ++request.payload.flow_token.session;
    assert(exchange(flows, &request).status == -ESTALE);
    request.operation = DPPD_MANAGEMENT_RULE_APPLY;
    assert(exchange(flows, &request).status == -ENOTSUP);
    --request.version;
    assert(exchange(flows, &request).status == -EPROTO);
    ++request.version;
    request.operation = DPPD_MANAGEMENT_FLOW_REMOVE;
    memset(&request.payload, 0, sizeof(request.payload));
    request.payload.flow_remove.count = 5;
    assert(exchange(flows, &request).status == -EINVAL);
    struct dppd_rule installed;
    assert(dppd_flow_store_get(store, 10, &installed) == 0);
    request.payload.flow_remove.count = 1;
    request.payload.flow_remove.rules[0] = (struct dppd_flow_remove_request){10, installed.generation};
    assert(exchange(flows, &request).status == 0);
    /** 客户端放弃领取也必须能关闭，未执行删除取消后保留可靠的旧配置 */
    assert(dppd_flow_management_drain(flows) == 0);
    /** 持久入口不依赖随机进程 token，停机取消也要留下可重复查询的终态 */
    request.operation = DPPD_MANAGEMENT_REQUEST_SUBMIT;
    memset(&request.payload, 0, sizeof(request.payload));
    request.payload.durable_request =
        (struct dppd_flow_request){.key = {81, 1}, .operation = DPPD_FLOW_REQUEST_REMOVE, .count = 1};
    request.payload.durable_request.payload.remove[0] = (struct dppd_flow_remove_request){10, installed.generation};
    response = exchange(flows, &request);
    assert(response.status == 0 && response.payload.durable_result.phase == DPPD_FLOW_REQUEST_PENDING);
    assert(dppd_flow_management_drain(flows) == 0);
    request.operation = DPPD_MANAGEMENT_REQUEST_GET;
    request.payload.request_key = (struct dppd_flow_request_key){81, 1};
    response = exchange(flows, &request);
    assert(response.status == 0 && response.payload.durable_result.phase == DPPD_FLOW_REQUEST_DONE);
    assert(response.payload.durable_result.status == -ECANCELED && response.payload.durable_result.durable);
    assert(exchange(flows, &request).status == 0);
    assert(dppd_flow_store_close(&store) == 0);
    free(flows);
    dppd_flow_state_free(&state);
    assert(unlink(path) == 0 && unlink(lock) == 0 && rmdir(directory) == 0);
    return 0;
}
