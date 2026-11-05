#define _GNU_SOURCE
#include <arpa/inet.h>
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

/** 双栈整表只能通过持久键提交，读到的统一版本用于下一次修改，路由代次只表示表本身 */
static void test_managed_routing(struct dppd_flow_management *flows)
{
    const struct dppd_routing_binding binding = {.port_id = 7, .device = "test7", .driver = "net_ring"};
    assert(dppd_flow_store_routing_bind(flows->store, &binding, 1) == 0);
    struct dppd_management_request query = {.version = DPPD_MANAGEMENT_VERSION,
        .size = sizeof(query), .operation = DPPD_MANAGEMENT_ROUTING_GET, .request_id = 101};
    struct dppd_management_response before = exchange(flows, &query);
    assert(before.status == 0 && before.payload.flow_routing.configuration.route_count == 0);
    uint64_t generation = before.payload.flow_routing.configuration.generation;
    uint64_t revision = before.payload.flow_routing.revision;
    assert(before.payload.flow_routing.durable_revision == revision);
    query.operation = DPPD_MANAGEMENT_ROUTING_STATUS;
    assert(exchange(flows, &query).payload.routing_status.enabled);
    query.operation = DPPD_MANAGEMENT_ROUTING_REPLACE;
    assert(exchange(flows, &query).status == -ENOTSUP);

    struct dppd_management_request request = {.version = DPPD_MANAGEMENT_VERSION,
        .size = sizeof(request), .operation = DPPD_MANAGEMENT_REQUEST_SUBMIT, .request_id = 102};
    struct dppd_flow_request *write_request = &request.payload.durable_request;
    *write_request = (struct dppd_flow_request){.key = {82, 1}, .operation = DPPD_FLOW_REQUEST_ROUTING_REPLACE,
        .count = 1, .expected_revision = revision};
    struct dppd_ipv4_configuration *configuration = &write_request->payload.routing;
    configuration->route_count = configuration->neighbor_count = 1;
    configuration->routes[0] = (struct dppd_ipv4_route){.prefix_be = htonl(0xc0000200),
        .gateway_be = htonl(0xc0000201), .port_id = 7, .depth = 24};
    configuration->neighbors[0] = (struct dppd_ipv4_neighbor){.address_be = htonl(0xc0000201),
        .port_id = 7, .mac = {2, 0, 0, 0, 0, 1}};
    configuration->route6_count = configuration->neighbor6_count = 1;
    configuration->routes6[0].port_id = 7;
    configuration->routes6[0].depth = 64;
    assert(inet_pton(AF_INET6, "2001:db8::", configuration->routes6[0].prefix) == 1);
    assert(inet_pton(AF_INET6, "fe80::1", configuration->routes6[0].gateway) == 1);
    configuration->neighbors6[0].port_id = 7;
    configuration->neighbors6[0].mac[0] = 2;
    configuration->neighbors6[0].mac[5] = 2;
    assert(inet_pton(AF_INET6, "fe80::1", configuration->neighbors6[0].address) == 1);
    struct dppd_management_response response = exchange(flows, &request);
    assert(response.status == 0 && response.payload.durable_result.phase == DPPD_FLOW_REQUEST_DONE);
    assert(response.payload.durable_result.applied && response.payload.durable_result.durable);
    assert(response.payload.durable_result.status == 0 && response.payload.durable_result.revision == revision + 1);
    assert(response.payload.durable_result.ids[0] == 0 && response.payload.durable_result.generations[0] == generation + 1);
    assert(exchange(flows, &request).payload.durable_result.generations[0] == generation + 1);
    configuration->neighbors[0].mac[5] = 9;
    assert(exchange(flows, &request).status == -EEXIST);
    query.operation = DPPD_MANAGEMENT_ROUTING_GET;
    response = exchange(flows, &query);
    assert(response.status == 0 && response.payload.flow_routing.revision == revision + 1);
    assert(response.payload.flow_routing.configuration.route_count == 1);
    assert(response.payload.flow_routing.configuration.neighbor6_count == 1);
    assert(response.payload.flow_routing.configuration.neighbors[0].mac[5] == 1);
    query.operation = DPPD_MANAGEMENT_ROUTING_STATUS;
    response = exchange(flows, &query);
    assert(response.status == 0 && !response.payload.routing_status.dirty);
    assert(response.payload.routing_status.generation == generation + 1);
    assert(response.payload.routing_status.persisted_generation == generation + 1);
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
    test_managed_routing(flows);
    assert(dppd_flow_store_close(&store) == 0);
    free(flows);
    dppd_flow_state_free(&state);
    assert(unlink(path) == 0 && unlink(lock) == 0 && rmdir(directory) == 0);
    return 0;
}
