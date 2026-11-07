#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "flow_state_fixture.h"
#include "dppd/flow_management.h"
#include "dppd/runtime.h"

static bool fail_sync;
static int wait_error;
static unsigned int wait_calls;
static int prepare_error, rollback_error;
static unsigned int prepare_calls, rollback_calls;
int __real_dppd_runtime_wait(struct dppd_runtime *runtime);
int __wrap_dppd_runtime_wait(struct dppd_runtime *runtime);
int __wrap_dppd_runtime_prepare_nd6(struct dppd_runtime *runtime, const struct dppd_nd6_policy *policy,
    struct dppd_nd6_rx_preparation *prepared);
int __wrap_dppd_runtime_rollback_nd6(struct dppd_runtime *runtime, struct dppd_nd6_rx_preparation *prepared);
/** 模拟包线程不能按时交回控制权，验证这种故障不能被当成普通配置失败继续运行 */
int __wrap_dppd_runtime_wait(struct dppd_runtime *runtime)
{
    ++wait_calls;
    return wait_error != 0 ? wait_error : __real_dppd_runtime_wait(runtime);
}

/** 这里关注管理事务边界，驱动组播开关的具体行为由 runtime 单测覆盖 */
int __wrap_dppd_runtime_prepare_nd6(struct dppd_runtime *runtime, const struct dppd_nd6_policy *policy,
    struct dppd_nd6_rx_preparation *prepared)
{
    assert(runtime != NULL && !runtime->workers_started && policy != NULL && prepared != NULL);
    ++prepare_calls;
    return prepare_error;
}

int __wrap_dppd_runtime_rollback_nd6(struct dppd_runtime *runtime, struct dppd_nd6_rx_preparation *prepared)
{
    assert(runtime != NULL && !runtime->workers_started && prepared != NULL);
    ++rollback_calls;
    return rollback_error;
}
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

/** 策略修改留下持久回执，会话清空仍是运行态操作，两类版本必须分别核对 */
static void test_managed_conntrack(struct dppd_flow_management *flows)
{
    struct dppd_management_request query = {.version = DPPD_MANAGEMENT_VERSION,
        .size = sizeof(query), .operation = DPPD_MANAGEMENT_CONNTRACK_POLICY_GET, .request_id = 103};
    struct dppd_management_response before = exchange(flows, &query);
    assert(before.status == 0 && before.payload.conntrack_policy.policy.capacity == 0);
    const struct dppd_flow_conntrack_view initial = before.payload.conntrack_policy;
    assert(initial.session_epoch != 0 && initial.session_revision != 0);
    struct dppd_management_request request = {.version = DPPD_MANAGEMENT_VERSION,
        .size = sizeof(request), .operation = DPPD_MANAGEMENT_REQUEST_SUBMIT, .request_id = 104};
    struct dppd_flow_request *replacement = &request.payload.durable_request;
    *replacement = (struct dppd_flow_request){.key = {83, 1}, .operation = DPPD_FLOW_REQUEST_CONNTRACK_REPLACE,
        .count = 1, .expected_revision = initial.revision};
    replacement->payload.conntrack.expected_session_epoch = initial.session_epoch;
    replacement->payload.conntrack.expected_session_revision = initial.session_revision;
    replacement->payload.conntrack.policy = (struct dppd_conntrack_policy){.capacity = 8, .timeout_s = 60};
    struct dppd_management_response response = exchange(flows, &request);
    assert(response.status == 0 && response.payload.durable_result.status == 0);
    assert(response.payload.durable_result.applied && response.payload.durable_result.durable);
    assert(response.payload.durable_result.generations[0] == initial.generation + 1);
    assert(response.payload.durable_result.revision == initial.revision + 1);
    assert(exchange(flows, &request).payload.durable_result.revision == initial.revision + 1);
    replacement->payload.conntrack.policy.capacity = 9;
    assert(exchange(flows, &request).status == -EEXIST);
    response = exchange(flows, &query);
    assert(response.status == 0 && response.payload.conntrack_policy.policy.capacity == 8);
    assert(response.payload.conntrack_policy.session_epoch == initial.session_epoch);
    assert(response.payload.conntrack_policy.revision == initial.revision + 1);
    query.operation = DPPD_MANAGEMENT_NAT_STATUS;
    response = exchange(flows, &query);
    assert(response.status == 0 && response.payload.conntrack.enabled && response.payload.conntrack.active == 0);

    struct dppd_packet packet = {.l3_type = DPPD_L3_IPV4, .l4_type = DPPD_L4_UDP, .ip_protocol = 17,
        .ipv4_src_be = htonl(0x0a000001), .ipv4_dst_be = htonl(0xc6336401),
        .l4_src_port_be = htons(1234), .l4_dst_port_be = htons(443), .packet_len = 64};
    struct dppd_connection_attempt attempt;
    assert(dppd_conntrack_begin(dppd_flow_store_conntrack(flows->store), 7, &packet, dppd_monotonic_ms(), &attempt) == 0);
    assert(dppd_conntrack_finish(&attempt, 7, true) == 0);
    query.operation = DPPD_MANAGEMENT_CONNTRACK_LIST;
    response = exchange(flows, &query);
    assert(response.status == 0 && response.payload.connections.count == 1);
    uint64_t epoch = response.payload.connections.epoch, revision = response.payload.connections.revision;
    query.payload.conntrack.expected_revision = revision;
    assert(exchange(flows, &query).status == -EINVAL);
    query.payload.conntrack.expected_epoch = epoch == 1 ? 2 : 1;
    assert(exchange(flows, &query).status == -ESTALE);
    query.payload.conntrack.expected_epoch = epoch;
    assert(exchange(flows, &query).status == 0);
    query.operation = DPPD_MANAGEMENT_CONNTRACK_FLUSH;
    query.payload.conntrack.expected_epoch = 0;
    assert(exchange(flows, &query).status == -EINVAL);
    query.payload.conntrack.expected_epoch = epoch == 1 ? 2 : 1;
    assert(exchange(flows, &query).status == -ESTALE);
    query.payload.conntrack.expected_epoch = epoch;
    response = exchange(flows, &query);
    assert(response.status == 0 && response.payload.conntrack.active == 0 && response.payload.conntrack.flushed == 1);
    assert(exchange(flows, &query).status == -ESTALE);
    query.operation = DPPD_MANAGEMENT_CONNTRACK_POLICY_GET;
    response = exchange(flows, &query);
    assert(response.status == 0 && response.payload.conntrack_policy.revision == initial.revision + 1);
    assert(response.payload.conntrack_policy.generation == initial.generation + 1);
}

static int discard_nd_probe(void *context, uint16_t port, const uint8_t *wire, uint16_t length)
{
    (void)context;
    (void)port;
    (void)wire;
    (void)length;
    return 0;
}

/** 致命故障先告知客户端，再交给 daemon 主循环，不能发送完响应就把错误吞掉 */
static void test_nd6_fatal_socket(struct dppd_flow_management *flows, struct dppd_runtime *runtime,
    const struct dppd_management_request *request, struct dppd_management_response *response)
{
    char directory[] = "/tmp/dppd-nd6-socket-XXXXXX", path[128];
    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/control.sock", directory);
    struct dppd_management_server server = {0};
    assert(dppd_management_start_flow(&server, flows, runtime, path) == 0);
    int client = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(client >= 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    assert(strlen(path) < sizeof(address.sun_path));
    memcpy(address.sun_path, path, strlen(path) + 1U);
    assert(connect(client, (const struct sockaddr *)&address, sizeof(address)) == 0);
    assert(send(client, request, sizeof(*request), 0) == (ssize_t)sizeof(*request));
    assert(dppd_management_poll(&server) == -ETIMEDOUT);
    assert(recv(client, response, sizeof(*response), 0) == (ssize_t)sizeof(*response));
    assert(response->status == -ETIMEDOUT && response->request_id == request->request_id);
    close(client);
    dppd_management_stop(&server);
    assert(rmdir(directory) == 0);
}

/** 先交回包线程再改 ND，重复提交不重跑检测，检测未完成也不能阻止 worker 继续收发 */
static void test_managed_nd6(struct dppd_flow_management *flows)
{
    const struct dppd_nd6_port port = {.port_id = 7, .mac = {2, 0, 0, 0, 0, 7}};
    assert(dppd_flow_store_nd6_bind(flows->store, &port, 1, dppd_monotonic_ms()) == 0);
    struct dppd_management_request query = {.version = DPPD_MANAGEMENT_VERSION,
        .size = sizeof(query), .operation = DPPD_MANAGEMENT_ND6_POLICY_GET, .request_id = 105};
    struct dppd_management_response response = exchange(flows, &query);
    assert(response.status == 0 && response.payload.nd6_policy.policy.local_count == 0);
    const struct dppd_flow_nd6_view initial = response.payload.nd6_policy;
    assert(initial.neighbor_epoch != 0 && initial.neighbor_revision != 0);
    struct dppd_management_request request = {.version = DPPD_MANAGEMENT_VERSION,
        .size = sizeof(request), .operation = DPPD_MANAGEMENT_REQUEST_SUBMIT, .request_id = 106};
    struct dppd_flow_request *replacement = &request.payload.durable_request;
    *replacement = (struct dppd_flow_request){.key = {84, 1}, .operation = DPPD_FLOW_REQUEST_ND6_REPLACE,
        .count = 1, .expected_revision = initial.revision};
    replacement->payload.nd6.expected_neighbor_epoch = initial.neighbor_epoch;
    replacement->payload.nd6.expected_neighbor_revision = initial.neighbor_revision;
    replacement->payload.nd6.policy = (struct dppd_nd6_policy){.local_count = 1, .reachable_ms = 30000};
    replacement->payload.nd6.policy.locals[0].port_id = 7;
    assert(inet_pton(AF_INET6, "fe80::7", replacement->payload.nd6.policy.locals[0].address) == 1);
    struct dppd_runtime *runtime = calloc(1, sizeof(*runtime));
    assert(runtime != NULL);
    runtime->nd6 = dppd_flow_store_nd6(flows->store);
    runtime->workers_started = true;
    atomic_init(&runtime->stop_requested, false);
    wait_calls = 0;
    wait_error = -ETIMEDOUT;
    test_nd6_fatal_socket(flows, runtime, &request, &response);
    assert(response.status == -ETIMEDOUT && wait_calls == 1 && runtime->workers_started);
    assert(exchange(flows, &query).payload.nd6_policy.generation == initial.generation);
    struct dppd_flow_request_result receipt;
    assert(dppd_flow_store_request_get(flows->store, replacement->key, &receipt) == -ENOENT);
    wait_error = 0;
    prepare_error = -ENOTSUP;
    assert(dppd_flow_management_handle(flows, runtime, &request, &response) == 0 && response.status == -ENOTSUP);
    assert(wait_calls == 2 && !runtime->workers_started && prepare_calls == 1);
    assert(dppd_flow_store_request_get(flows->store, replacement->key, &receipt) == -ENOENT);
    prepare_error = -EUCLEAN;
    assert(dppd_flow_management_handle(flows, runtime, &request, &response) == -EUCLEAN && response.status == -EUCLEAN);
    assert(prepare_calls == 2 && dppd_flow_store_request_get(flows->store, replacement->key, &receipt) == -ENOENT);
    prepare_error = 0;
    assert(dppd_flow_management_handle(flows, runtime, &request, &response) == 0 && response.status == 0);
    assert(response.payload.durable_result.applied && response.payload.durable_result.durable);
    assert(response.payload.durable_result.generations[0] == initial.generation + 1);
    assert(wait_calls == 2 && !runtime->workers_started);
    assert(prepare_calls == 3 && rollback_calls == 0);
    runtime->workers_started = true;
    atomic_store(&runtime->stop_requested, false);
    assert(dppd_flow_management_handle(flows, runtime, &request, &response) == 0 && response.status == 0);
    assert(wait_calls == 2 && runtime->workers_started);
    assert(prepare_calls == 3);
    replacement->payload.nd6.policy.reachable_ms = 40000;
    assert(dppd_flow_management_handle(flows, runtime, &request, &response) == 0 && response.status == -EEXIST);
    assert(wait_calls == 2 && runtime->workers_started);
    assert(prepare_calls == 3);
    query.operation = DPPD_MANAGEMENT_HEALTH_GET;
    assert(dppd_flow_management_handle(flows, runtime, &query, &response) == 0 && response.status == 0);
    assert(!response.payload.health.ready && (response.payload.health.blockers & DPPD_HEALTH_ND6_NOT_READY));
    struct dppd_flow_store_status store_status;
    assert(dppd_flow_store_status(flows->store, &store_status) == 0 && store_status.ready);
    uint64_t now = dppd_monotonic_ms();
    dppd_nd6_tick(runtime->nd6, now + 2000, discard_nd_probe, NULL);
    dppd_nd6_tick(runtime->nd6, now + 4000, discard_nd_probe, NULL);
    assert(dppd_flow_management_handle(flows, runtime, &query, &response) == 0 && response.status == 0);
    assert(!(response.payload.health.blockers & DPPD_HEALTH_ND6_NOT_READY));
    query.operation = DPPD_MANAGEMENT_ND6_LIST;
    response = exchange(flows, &query);
    assert(response.status == 0 && response.payload.dynamic_neighbors.count == 0);
    uint64_t epoch = response.payload.dynamic_neighbors.epoch, revision = response.payload.dynamic_neighbors.revision;
    query.payload.conntrack.expected_revision = revision;
    assert(exchange(flows, &query).status == -EINVAL);
    query.payload.conntrack.expected_epoch = epoch == 1 ? 2 : 1;
    assert(exchange(flows, &query).status == -ESTALE);
    query.payload.conntrack.expected_epoch = epoch;
    assert(exchange(flows, &query).status == 0);
    query.operation = DPPD_MANAGEMENT_ND6_FLUSH;
    wait_error = -ETIMEDOUT;
    assert(dppd_flow_management_handle(flows, runtime, &query, &response) == -ETIMEDOUT);
    assert(response.status == -ETIMEDOUT && wait_calls == 3);
    wait_error = 0;
    assert(dppd_flow_management_handle(flows, runtime, &query, &response) == 0 && response.status == 0);
    assert(wait_calls == 4 && !runtime->workers_started);
    assert(response.payload.nd6.epoch == epoch && response.payload.nd6.revision > revision);
    assert(response.payload.nd6.locals[0].state == DPPD_ND6_PREFERRED);
    assert(exchange(flows, &query).status == -ESTALE);
    query.operation = DPPD_MANAGEMENT_ND6_POLICY_GET;
    response = exchange(flows, &query);
    assert(response.payload.nd6_policy.generation == initial.generation + 1);
    assert(response.payload.nd6_policy.revision == initial.revision + 1);
    query.operation = DPPD_MANAGEMENT_CONNTRACK_POLICY_GET;
    assert(exchange(flows, &query).payload.conntrack_policy.policy.capacity == 8);
    /** 版本冲突没有发布策略，撤销组播准备；撤销也失败时必须交给主循环退出 */
    replacement->key.sequence = 2;
    rollback_error = -EIO;
    assert(dppd_flow_management_handle(flows, runtime, &request, &response) == -EUCLEAN && response.status == -EUCLEAN);
    assert(prepare_calls == 4 && rollback_calls == 1);
    rollback_error = 0;
    replacement->key.sequence = 3;
    assert(dppd_flow_management_handle(flows, runtime, &request, &response) == 0);
    assert(response.status == 0 && response.payload.durable_result.status == -ESTALE);
    assert(!response.payload.durable_result.applied && prepare_calls == 5 && rollback_calls == 2);
    free(runtime);
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
    test_managed_conntrack(flows);
    test_managed_nd6(flows);
    assert(dppd_flow_store_close(&store) == 0);
    free(flows);
    dppd_flow_state_free(&state);
    assert(unlink(path) == 0 && unlink(lock) == 0 && rmdir(directory) == 0);
    return 0;
}
