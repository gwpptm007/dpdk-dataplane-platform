#define _GNU_SOURCE
#include "flow_state_fixture.h"
#include "dppd/flow_request.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t fail_regular_nth, fail_directory_nth;
int __real_fsync(int fd);
int __wrap_fsync(int fd);

/** 分别定位受理文件、最终配置文件和目录同步，真实文件写入仍由生产实现完成 */
int __wrap_fsync(int fd)
{
    struct stat information;
    assert(fstat(fd, &information) == 0);
    uint32_t *remaining = S_ISDIR(information.st_mode) ? &fail_directory_nth : &fail_regular_nth;
    if (*remaining != 0 && --*remaining == 0) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

static const struct dppd_routing_binding bindings[] = {
    {.port_id = 7, .device = "net_tap7", .driver = "net_tap", .ifname = "dpr7"},
    {.port_id = 9, .device = "net_tap9", .driver = "net_tap", .ifname = "dpr9"},
    {.port_id = 11, .device = "net_tap11", .driver = "net_tap", .ifname = "dpr11"},
};

static uint32_t ip(const char *text)
{
    uint32_t address;
    assert(inet_pton(AF_INET, text, &address) == 1);
    return address;
}

static void ip6(const char *text, uint8_t address[16])
{
    assert(inet_pton(AF_INET6, text, address) == 1);
}

static unsigned int file_version(const char *path)
{
    uint8_t header[16];
    int fd = open(path, O_RDONLY);
    assert(fd >= 0 && read(fd, header, sizeof(header)) == (ssize_t)sizeof(header));
    assert(close(fd) == 0);
    for (unsigned int i = 9; i < 16; ++i)
        assert(header[i] == 0);
    return header[8];
}

static uint8_t *file_bytes(const char *path, size_t *size)
{
    struct stat information;
    int fd = open(path, O_RDONLY);
    assert(fd >= 0 && fstat(fd, &information) == 0);
    *size = (size_t)information.st_size;
    uint8_t *bytes = malloc(*size);
    assert(bytes != NULL && read(fd, bytes, *size) == (ssize_t)*size);
    assert(close(fd) == 0);
    return bytes;
}

static struct dppd_flow_store *fresh(const char *path, bool routed)
{
    struct dppd_flow_state *state = flow_state_fixture(0);
    struct dppd_flow_store *store = NULL;
    assert(dppd_flow_state_save(path, state) == 0);
    dppd_flow_state_free(&state);
    assert(dppd_flow_store_open(path, NULL, &store) == 0);
    if (routed)
        assert(dppd_flow_store_routing_bind(store, bindings, 3) == 0);
    return store;
}

static struct dppd_flow_request route_request(uint64_t sequence, uint64_t revision)
{
    struct dppd_flow_request request = {.key = {701, sequence},
        .operation = DPPD_FLOW_REQUEST_ROUTING_REPLACE, .count = 1, .expected_revision = revision};
    struct dppd_ipv4_configuration *c = &request.payload.routing;
    c->route_count = c->neighbor_count = c->route6_count = c->neighbor6_count = 1;
    c->routes[0] = (struct dppd_ipv4_route){.prefix_be = ip("192.0.2.0"), .gateway_be = ip("192.0.2.1"),
        .port_id = 9, .depth = 24};
    c->neighbors[0] = (struct dppd_ipv4_neighbor){.address_be = ip("192.0.2.1"), .port_id = 9,
        .mac = {2, 0, 0, 0, 0, 9}};
    ip6("2001:db8:2::", c->routes6[0].prefix);
    ip6("fe80::1", c->routes6[0].gateway);
    c->routes6[0].port_id = 11;
    c->routes6[0].depth = 64;
    ip6("fe80::1", c->neighbors6[0].address);
    c->neighbors6[0].port_id = 11;
    c->neighbors6[0].mac[0] = 2;
    c->neighbors6[0].mac[5] = 11;
    return request;
}

/** 未启用路由时在受理前拒绝，不给旧版文件追加新请求类型或改变任何文件字节 */
static void unsupported_unchanged(struct dppd_flow_store *store, const char *path)
{
    size_t before_size, after_size;
    uint8_t *before = file_bytes(path, &before_size);
    struct dppd_flow_request request = route_request(99, 91);
    struct dppd_flow_request_result result;
    assert(dppd_flow_store_request(store, &request, 1, &result) == -ENOTSUP);
    uint8_t *after = file_bytes(path, &after_size);
    assert(before_size == after_size && memcmp(before, after, before_size) == 0);
    free(before);
    free(after);
}

static void check_versions(struct dppd_flow_store *store, uint64_t revision, uint64_t generation, bool dirty)
{
    struct dppd_flow_store_status state;
    struct dppd_routing_status routes;
    assert(dppd_flow_store_status(store, &state) == 0);
    assert(dppd_flow_store_routing_status(store, &routes) == 0);
    assert(state.service.revision == revision && state.dirty == dirty && state.ready == !dirty);
    assert(routes.enabled && routes.generation == generation && routes.dirty == dirty);
    if (!dirty)
        assert(state.durable_revision == revision && routes.persisted_generation == generation);
}

/** 实际调用双栈查询，确认恢复的不只是配置文本，还能得到正确出口和目的 MAC */
static void check_forwarding(struct dppd_flow_store *store)
{
    struct dppd_ipv4_router *router = dppd_flow_store_router(store);
    struct dppd_ipv4_forward forward;
    uint8_t destination[16];
    assert(router != NULL && dppd_ipv4_router_reader_register(router, 0) == 0);
    assert(dppd_ipv4_router_lookup(router, ip("192.0.2.50"), &forward) == 0);
    assert(forward.port_id == 9 && forward.destination_mac[5] == 9);
    ip6("2001:db8:2::50", destination);
    assert(dppd_ipv6_router_lookup(router, destination, &forward) == 0);
    assert(forward.port_id == 11 && forward.destination_mac[5] == 11);
    dppd_ipv4_router_reader_unregister(router, 0);
}

/** 初次绑定写一次配置，同一组端口换序不重新保存，设备或接口身份变化则明确拒绝 */
static void test_bindings(const char *path)
{
    struct dppd_flow_store *store = fresh(path, false);
    struct dppd_routing_binding changed[3];
    struct dppd_routing_status status;
    assert(dppd_flow_store_routing_status(store, &status) == 0 && !status.enabled);
    assert(dppd_flow_store_routing_bind(store, bindings + 1, 2) == -EINVAL);
    assert(dppd_flow_store_router(store) == NULL);
    memcpy(changed, bindings, sizeof(changed));
    changed[0].ifname[0] = '\0';
    assert(dppd_flow_store_routing_bind(store, changed, 3) == -EINVAL);
    assert(dppd_flow_store_routing_bind(store, bindings, 3) == 0);
    check_versions(store, 92, 1, false);
    assert(file_version(path) == 3);
    struct dppd_ipv4_router *router = dppd_flow_store_router(store);
    struct stat before, after;
    assert(stat(path, &before) == 0);
    changed[0] = bindings[2];
    changed[1] = bindings[0];
    changed[2] = bindings[1];
    assert(dppd_flow_store_routing_bind(store, changed, 3) == 0);
    assert(stat(path, &after) == 0 && before.st_ino == after.st_ino && before.st_dev == after.st_dev);
    assert(dppd_flow_store_router(store) == router);
    check_versions(store, 92, 1, false);
    for (unsigned int field = 0; field < 4; ++field) {
        memcpy(changed, bindings, sizeof(changed));
        if (field == 0)
            changed[0].port_id = 8;
        if (field == 1)
            strcpy(changed[0].device, "net_tap_other");
        if (field == 2)
            strcpy(changed[0].driver, "other_pmd");
        if (field == 3)
            strcpy(changed[0].ifname, "other_iface");
        assert(dppd_flow_store_routing_bind(store, changed, 3) == -ESTALE);
    }
    assert(dppd_flow_store_routing_bind(store, bindings, 2) == -ESTALE);
    memcpy(changed, bindings, sizeof(changed));
    changed[2] = changed[1];
    assert(dppd_flow_store_routing_bind(store, changed, 3) == -EINVAL);
    check_versions(store, 92, 1, false);
    struct dppd_flow_state *disk = NULL;
    assert(dppd_flow_state_load(path, &disk) == 0);
    disk->routing->bindings[0].port_id = 8;
    assert(dppd_flow_state_validate(disk) == -EINVAL);
    dppd_flow_state_free(&disk);
    assert(dppd_flow_store_close(&store) == 0);
}

/** 初次启用保存失败时保留同一个路由器和 dirty，不能让重试换一个对象绕过等待修复 */
static void test_binding_sync_failure(const char *path)
{
    for (unsigned int renamed = 0; renamed < 2; ++renamed) {
        struct dppd_flow_store *store = fresh(path, false);
        if (renamed)
            fail_directory_nth = 1;
        else
            fail_regular_nth = 1;
        assert(dppd_flow_store_routing_bind(store, bindings, 3) == -EUCLEAN);
        struct dppd_ipv4_router *router = dppd_flow_store_router(store);
        assert(router != NULL);
        check_versions(store, 92, 1, true);
        assert(dppd_flow_store_routing_bind(store, bindings, 3) == -EUCLEAN);
        assert(dppd_flow_store_close(&store) == -EUCLEAN && store != NULL);
        struct dppd_flow_state *disk = NULL;
        assert(dppd_flow_state_load(path, &disk) == 0);
        assert((disk->routing != NULL) == (renamed != 0));
        dppd_flow_state_free(&disk);
        assert(dppd_flow_store_flush(store) == 0 && dppd_flow_store_router(store) == router);
        check_versions(store, 92, 1, false);
        assert(dppd_flow_store_close(&store) == 0);
    }
}

/** 旧版文件仍可开启，路由上线后升级为第三版，规则修订号和路由代次分别保留 */
static void test_versions_and_requests(const char *path)
{
    struct dppd_flow_store *store = fresh(path, false);
    assert(file_version(path) == 1);
    unsupported_unchanged(store, path);
    struct dppd_flow_state *fixture = flow_state_fixture(0);
    struct dppd_flow_request definition = {.key = {702, 1}, .operation = DPPD_FLOW_REQUEST_CONFIGURE,
        .count = 1, .expected_revision = 91};
    definition.payload.configuration[0] = (struct dppd_flow_config_change){.operation = DPPD_FLOW_CONFIG_TEMPLATE_ADD,
        .key = 101, .spec.table = fixture->tables[0].spec};
    dppd_flow_state_free(&fixture);
    struct dppd_flow_request_result result;
    assert(dppd_flow_store_request(store, &definition, 1, &result) == 0 && result.applied && result.revision == 92);
    assert(dppd_flow_store_close(&store) == 0 && file_version(path) == 2);
    assert(dppd_flow_store_open(path, NULL, &store) == 0 && dppd_flow_store_router(store) == NULL);
    unsupported_unchanged(store, path);
    assert(dppd_flow_store_routing_bind(store, bindings, 3) == 0);
    check_versions(store, 93, 1, false);
    struct dppd_flow_request request = route_request(1, 93);
    assert(dppd_flow_store_request(store, &request, 1, &result) == 0);
    assert(result.phase == DPPD_FLOW_REQUEST_DONE && result.status == 0 && result.applied && result.durable);
    assert(result.revision == 94 && result.generations[0] == 2);
    check_forwarding(store);
    request.payload.routing.generation = UINT64_MAX;
    request.payload.routing.routes[255].depth = 255;
    assert(dppd_flow_store_request(store, &request, 2, &result) == 0 && result.revision == 94);
    assert(dppd_flow_store_close(&store) == 0 && file_version(path) == 3);
    /** 第三版的路由回执不能脱离路由配置另存成旧版文件 */
    struct dppd_flow_state *disk = NULL;
    assert(dppd_flow_state_load(path, &disk) == 0);
    struct dppd_flow_routing_state *saved = disk->routing;
    disk->routing = NULL;
    assert(dppd_flow_state_validate(disk) == -EINVAL);
    disk->routing = saved;
    dppd_flow_state_free(&disk);
    assert(dppd_flow_store_open(path, NULL, &store) == 0);
    assert(dppd_flow_store_routing_bind(store, bindings, 3) == 0);
    assert(dppd_flow_store_request_get(store, request.key, &result) == 0 && result.applied && result.durable);
    assert(result.revision == 94 && result.generations[0] == 2);
    assert(dppd_flow_store_request(store, &request, 3, &result) == 0 && result.revision == 94);
    check_versions(store, 94, 2, false);
    check_forwarding(store);
    request.payload.routing.neighbors[0].mac[5] = 99;
    assert(dppd_flow_store_request(store, &request, 4, &result) == -EEXIST);
    request = route_request(2, 93);
    assert(dppd_flow_store_request(store, &request, 5, &result) == 0);
    assert(result.status == -ESTALE && !result.applied && result.durable);
    check_versions(store, 94, 2, false);
    request = route_request(3, 94);
    memset(&request.payload.routing, 0, sizeof(request.payload.routing));
    assert(dppd_flow_store_request(store, &request, 6, &result) == 0 && result.applied && result.generations[0] == 3);
    struct dppd_ipv4_configuration configuration;
    uint64_t revision, durable;
    assert(dppd_flow_store_routing_get(store, &configuration, &revision, &durable) == 0);
    assert(revision == 95 && durable == 95 && configuration.route_count == 0 && configuration.neighbor_count == 0);
    assert(configuration.route6_count == 0 && configuration.neighbor6_count == 0);
    assert(dppd_flow_store_close(&store) == 0);
}

/** 无效表不能改变当前版本或查询结果，反例分别触发前缀、出口、邻居与重复项约束 */
static void test_invalid_tables(const char *path)
{
    struct dppd_flow_store *store = fresh(path, true);
    struct dppd_flow_request request = route_request(1, 92);
    struct dppd_flow_request_result result;
    assert(dppd_flow_store_request(store, &request, 1, &result) == 0 && result.applied);
    for (unsigned int invalid = 0; invalid < 8; ++invalid) {
        request = route_request(invalid + 2, 93);
        struct dppd_ipv4_configuration *c = &request.payload.routing;
        if (invalid == 0)
            c->routes[0].prefix_be = ip("192.0.2.1");
        if (invalid == 1)
            c->routes6[0].prefix[15] = 1;
        if (invalid == 2)
            c->routes[0].port_id = 99;
        if (invalid == 3)
            c->neighbors6[0].mac[0] = 1;
        if (invalid == 4)
            memset(c->neighbors[0].mac, 0, 6);
        if (invalid == 5) {
            c->route_count = 2;
            c->routes[1] = c->routes[0];
        }
        if (invalid == 6) {
            c->neighbor6_count = 2;
            c->neighbors6[1] = c->neighbors6[0];
        }
        if (invalid == 7)
            c->routes6[0].depth = 129;
        assert(dppd_flow_store_request(store, &request, 2 + invalid, &result) == 0);
        assert(result.status != 0 && !result.applied && result.durable);
        check_versions(store, 93, 2, false);
        check_forwarding(store);
    }
    assert(dppd_flow_store_close(&store) == 0);
}

/** 受理失败不会发布，最终保存失败保留已发布表；两种窗口都要修复落盘后才恢复 ready */
static void test_sync_failures(const char *path)
{
    for (unsigned int fault = 0; fault < 3; ++fault) {
        struct dppd_flow_store *store = fresh(path, true);
        struct dppd_ipv4_router *router = dppd_flow_store_router(store);
        struct dppd_flow_request request = route_request(1, 92);
        struct dppd_flow_request_result result;
        if (fault < 2)
            fail_regular_nth = fault + 1;
        else
            fail_directory_nth = 2;
        assert(dppd_flow_store_request(store, &request, 1, &result) == -EUCLEAN);
        assert(result.phase == DPPD_FLOW_REQUEST_DONE && !result.durable && result.applied == (fault != 0));
        assert(result.status == (fault == 0 ? -EIO : 0));
        check_versions(store, fault == 0 ? 92 : 93, fault == 0 ? 1 : 2, true);
        struct dppd_routing_status routes;
        assert(dppd_flow_store_routing_status(store, &routes) == 0 && routes.persisted_generation == 1);
        assert(dppd_flow_store_close(&store) == -EUCLEAN && store != NULL);
        assert(dppd_flow_store_router(store) == router);
        struct dppd_flow_state *disk = NULL;
        assert(dppd_flow_state_load(path, &disk) == 0 && disk->routing != NULL);
        assert(disk->routing->configuration.generation == (fault == 2 ? 2U : 1U));
        dppd_flow_state_free(&disk);
        assert(fail_regular_nth == 0 && fail_directory_nth == 0);
        assert(dppd_flow_store_flush(store) == 0);
        assert(dppd_flow_store_router(store) == router);
        check_versions(store, fault == 0 ? 92 : 93, fault == 0 ? 1 : 2, false);
        assert(dppd_flow_store_request_get(store, request.key, &result) == 0 && result.durable);
        assert(dppd_flow_store_close(&store) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
        assert(dppd_flow_store_request(store, &request, 2, &result) == 0 && result.applied == (fault != 0));
        if (fault != 0)
            check_forwarding(store);
        assert(dppd_flow_store_close(&store) == 0);
    }
}

/** 读者停滞时退役表达到上限就拒绝更新，读者报告安全点后再恢复，不无限堆积内存 */
static void test_reader_lifetime(const char *path)
{
    struct dppd_flow_store *store = fresh(path, true);
    struct dppd_ipv4_router *router = dppd_flow_store_router(store);
    assert(dppd_ipv4_router_reader_register(router, 0) == 0);
    struct dppd_flow_request_result result;
    for (uint32_t i = 0; i < DPPD_IPV4_RETIRED_LIMIT; ++i) {
        struct dppd_flow_request request = route_request(i + 1U, 92U + i);
        assert(dppd_flow_store_request(store, &request, i + 1U, &result) == 0 && result.applied);
    }
    struct dppd_flow_request blocked = route_request(65, 156);
    assert(dppd_flow_store_request(store, &blocked, 65, &result) == 0);
    assert(result.status == -EAGAIN && !result.applied);
    check_versions(store, 156, 65, false);
    dppd_ipv4_router_reader_quiescent(router, 0);
    blocked.key.sequence = 66;
    assert(dppd_flow_store_request(store, &blocked, 66, &result) == 0 && result.applied);
    assert(dppd_flow_store_close(&store) == -EBUSY && store != NULL);
    assert(dppd_flow_store_router(store) == router);
    struct dppd_ipv4_forward forward;
    assert(dppd_ipv4_router_lookup(router, ip("192.0.2.50"), &forward) == 0 && forward.port_id == 9);
    dppd_ipv4_router_reader_unregister(router, 0);
    assert(dppd_flow_store_close(&store) == 0);
}

/** 四个数组同时达到各自上限，检查完整请求缓存、磁盘编码和重启查询不截断尾部 */
static void test_full_tables(const char *path)
{
    struct dppd_flow_store *store = fresh(path, true);
    struct dppd_flow_request request = route_request(1, 92);
    struct dppd_ipv4_configuration *c = &request.payload.routing;
    memset(c, 0, sizeof(*c));
    c->route_count = c->route6_count = DPPD_IPV4_ROUTE_LIMIT;
    c->neighbor_count = c->neighbor6_count = DPPD_IPV4_NEIGHBOR_LIMIT;
    for (uint32_t i = 0; i < DPPD_IPV4_ROUTE_LIMIT; ++i) {
        uint32_t address = htonl(UINT32_C(0x0a000001) + i);
        c->routes[i] = (struct dppd_ipv4_route){.prefix_be = address, .port_id = 9, .depth = 32};
        c->neighbors[i] = (struct dppd_ipv4_neighbor){.address_be = address, .port_id = 9,
            .mac = {2, 0, 0, 0, (uint8_t)(i >> 8), (uint8_t)i}};
        ip6("2001:db8:3::", c->routes6[i].prefix);
        c->routes6[i].prefix[14] = (uint8_t)((i + 1U) >> 8);
        c->routes6[i].prefix[15] = (uint8_t)(i + 1U);
        c->routes6[i].port_id = 11;
        c->routes6[i].depth = 128;
        memcpy(c->neighbors6[i].address, c->routes6[i].prefix, 16);
        c->neighbors6[i].port_id = 11;
        c->neighbors6[i].mac[0] = 2;
        c->neighbors6[i].mac[4] = (uint8_t)(i >> 8);
        c->neighbors6[i].mac[5] = (uint8_t)i;
    }
    struct dppd_flow_request_result result;
    assert(dppd_flow_store_request(store, &request, 1, &result) == 0 && result.applied && result.durable);
    assert(dppd_flow_store_close(&store) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
    assert(dppd_flow_store_request(store, &request, 2, &result) == 0 && result.revision == 93);
    struct dppd_ipv4_configuration loaded;
    uint64_t revision, durable;
    assert(dppd_flow_store_routing_get(store, &loaded, &revision, &durable) == 0);
    c->generation = 2;
    assert(revision == 93 && durable == 93 && memcmp(c, &loaded, sizeof(loaded)) == 0);
    struct dppd_ipv4_router *router = dppd_flow_store_router(store);
    struct dppd_ipv4_forward forward;
    assert(dppd_ipv4_router_reader_register(router, 0) == 0);
    assert(dppd_ipv4_router_lookup(router, c->routes[255].prefix_be, &forward) == 0 && forward.port_id == 9);
    assert(dppd_ipv6_router_lookup(router, c->routes6[255].prefix, &forward) == 0 && forward.port_id == 11);
    dppd_ipv4_router_reader_unregister(router, 0);
    assert(dppd_flow_store_close(&store) == 0);
}

int main(void)
{
    char directory[] = "/tmp/dppd-flow-routing-XXXXXX", path[160], lock[170];
    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/state", directory);
    snprintf(lock, sizeof(lock), "%s.lock", path);
    test_bindings(path);
    test_binding_sync_failure(path);
    test_versions_and_requests(path);
    test_invalid_tables(path);
    test_sync_failures(path);
    test_reader_lifetime(path);
    test_full_tables(path);
    assert(unlink(path) == 0 && unlink(lock) == 0 && rmdir(directory) == 0);
    return 0;
}
