#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dppd/ipv4_router.h"

static _Thread_local unsigned int fail_allocation;
void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_allocation != 0 && --fail_allocation == 0)
        return NULL;
    return __real_calloc(count, size);
}

static uint32_t ip(const char *address)
{
    struct in_addr value;
    assert(inet_pton(AF_INET, address, &value) == 1);
    return value.s_addr;
}

static void basic(struct dppd_ipv4_configuration *configuration)
{
    memset(configuration, 0, sizeof(*configuration));
    configuration->route_count = 3;
    configuration->routes[0] = (struct dppd_ipv4_route){.port_id = 3, .gateway_be = ip("192.0.2.1")};
    configuration->routes[1] =
        (struct dppd_ipv4_route){.prefix_be = ip("10.0.0.0"), .depth = 8, .port_id = 7, .gateway_be = ip("192.0.2.2")};
    configuration->routes[2] = (struct dppd_ipv4_route){.prefix_be = ip("10.2.3.0"), .depth = 24, .port_id = 7};
    configuration->neighbor_count = 3;
    configuration->neighbors[0] =
        (struct dppd_ipv4_neighbor){.address_be = ip("192.0.2.1"), .port_id = 3, .mac = {2, 0, 0, 0, 0, 1}};
    configuration->neighbors[1] =
        (struct dppd_ipv4_neighbor){.address_be = ip("192.0.2.2"), .port_id = 7, .mac = {2, 0, 0, 0, 0, 2}};
    configuration->neighbors[2] =
        (struct dppd_ipv4_neighbor){.address_be = ip("10.2.3.4"), .port_id = 7, .mac = {2, 0, 0, 0, 0, 3}};
}

static struct dppd_ipv4_router *empty_router(void)
{
    struct dppd_ipv4_router *router = NULL;
    const uint16_t ports[] = {3, 7};
    assert(dppd_ipv4_router_create(&router, ports, 2, NULL) == 0);
    return router;
}

/** 分别核对默认路由、聚合前缀、直连主机与同 IP 不同出口的邻居选择 */
static void test_lookup(void)
{
    struct dppd_ipv4_router *router = empty_router();
    struct dppd_ipv4_configuration configuration;
    struct dppd_ipv4_forward forward;
    assert(dppd_ipv4_router_reader_register(router, 0) == 0);
    assert(dppd_ipv4_router_reader_register(router, 0) == -EALREADY);
    assert(dppd_ipv4_router_destroy(router) == -EBUSY);
    assert(dppd_ipv4_router_lookup(router, ip("10.2.3.4"), &forward) == -ENOENT);
    basic(&configuration);
    assert(dppd_ipv4_router_replace(router, 1, &configuration, NULL, NULL) == 0);
    assert(dppd_ipv4_router_lookup(router, ip("203.0.113.1"), &forward) == 0);
    assert(forward.port_id == 3 && forward.destination_mac[5] == 1 && forward.generation == 2);
    assert(dppd_ipv4_router_lookup(router, ip("10.9.8.7"), &forward) == 0);
    assert(forward.port_id == 7 && forward.destination_mac[5] == 2);
    assert(dppd_ipv4_router_lookup(router, ip("10.2.3.4"), &forward) == 0);
    assert(forward.port_id == 7 && forward.destination_mac[5] == 3 && forward.next_hop_be == ip("10.2.3.4"));
    /* 最长前缀的邻居缺失时不能偷偷改走短前缀，否则会绕过用户选定的出口 */
    assert(dppd_ipv4_router_lookup(router, ip("10.2.3.5"), &forward) == -EHOSTUNREACH);
    configuration.routes[3] = (struct dppd_ipv4_route){.prefix_be = ip("10.2.3.4"), .depth = 32, .port_id = 3};
    configuration.route_count++;
    configuration.neighbors[3] =
        (struct dppd_ipv4_neighbor){.address_be = ip("10.2.3.4"), .port_id = 3, .mac = {2, 0, 0, 0, 0, 4}};
    configuration.neighbor_count++;
    assert(dppd_ipv4_router_replace(router, 2, &configuration, NULL, NULL) == 0);
    assert(dppd_ipv4_router_lookup(router, ip("10.2.3.4"), &forward) == 0);
    assert(forward.port_id == 3 && forward.destination_mac[5] == 4 && forward.generation == 3);
    const char *invalid[] = {"0.1.2.3", "127.0.0.1", "224.0.0.1", "240.0.0.1", "255.255.255.255"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(dppd_ipv4_router_lookup(router, ip(invalid[i]), &forward) == -EAFNOSUPPORT);
    dppd_ipv4_router_reader_unregister(router, 0);
    assert(dppd_ipv4_router_destroy(router) == 0);
}

static unsigned int durable_calls;
static int refuse_save(void *context, const struct dppd_ipv4_configuration *candidate)
{
    uint64_t *generation = context;
    assert(candidate->generation == *generation);
    durable_calls++;
    return -ENOSPC;
}

/** 所有校验、资源和保存错误都发生在发布前，失败后版本及完整配置保持原样 */
static void test_rejection(void)
{
    struct dppd_ipv4_router *router = empty_router();
    struct dppd_ipv4_configuration configuration, before, after;
    assert(dppd_ipv4_router_get(router, &before) == 0 && before.generation == 1);
    for (unsigned int mode = 0; mode < 10; ++mode) {
        basic(&configuration);
        switch (mode) {
        case 0:
            configuration.routes[1].depth = 33;
            break;
        case 1:
            configuration.routes[1].prefix_be = ip("10.0.0.1");
            break;
        case 2:
            configuration.routes[1].port_id = 99;
            break;
        case 3:
            configuration.routes[1].gateway_be = ip("224.0.0.1");
            break;
        case 4:
            configuration.routes[1] = configuration.routes[0];
            break;
        case 5:
            configuration.neighbors[1] = configuration.neighbors[0];
            break;
        case 6:
            configuration.neighbors[1].mac[0] = 1;
            break;
        case 7:
            memset(configuration.neighbors[1].mac, 0, 6);
            break;
        case 8:
            configuration.neighbors[1].address_be = 0;
            break;
        case 9:
            configuration.routes[1].reserved = 1;
            break;
        }
        int expected = mode == 4 || mode == 5 ? -EEXIST : -EINVAL;
        assert(dppd_ipv4_router_replace(router, 1, &configuration, NULL, NULL) == expected);
        assert(dppd_ipv4_router_get(router, &after) == 0 && memcmp(&before, &after, sizeof(before)) == 0);
    }
    basic(&configuration);
    fail_allocation = 1;
    assert(dppd_ipv4_router_replace(router, 1, &configuration, NULL, NULL) == -ENOMEM);
    uint64_t wanted = 2;
    assert(dppd_ipv4_router_replace(router, 1, &configuration, refuse_save, &wanted) == -ENOSPC);
    assert(durable_calls == 1);
    assert(dppd_ipv4_router_get(router, &after) == 0 && memcmp(&before, &after, sizeof(before)) == 0);
    assert(dppd_ipv4_router_replace(router, 9, &configuration, refuse_save, &wanted) == -ESTALE);
    assert(durable_calls == 1);
    configuration.route_count = DPPD_IPV4_ROUTE_LIMIT + 1;
    assert(dppd_ipv4_router_replace(router, 1, &configuration, NULL, NULL) == -E2BIG);
    assert(dppd_ipv4_router_destroy(router) == 0);
    struct dppd_ipv4_router *restored = NULL;
    basic(&configuration);
    configuration.generation = UINT64_MAX;
    const uint16_t ports[] = {3, 7};
    assert(dppd_ipv4_router_create(&restored, ports, 2, &configuration) == 0);
    assert(dppd_ipv4_router_replace(restored, UINT64_MAX, &configuration, NULL, NULL) == -EOVERFLOW);
    assert(dppd_ipv4_router_destroy(restored) == 0);
}

static uint32_t random_state = 0x13579bdf;
static uint32_t random_u32(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

/** 独立线性参考模型与树查询比较，包含所有前缀长度和大量不命中路径 */
static void test_reference(void)
{
    struct dppd_ipv4_router *router = empty_router();
    struct dppd_ipv4_configuration configuration;
    basic(&configuration);
    configuration.route_count = 1;
    while (configuration.route_count < DPPD_IPV4_ROUTE_LIMIT) {
        uint8_t depth = (uint8_t)(1 + random_u32() % 32);
        uint32_t address = random_u32() & (UINT32_MAX << (32U - depth));
        bool duplicate = false;
        for (uint32_t i = 0; i < configuration.route_count; ++i)
            duplicate |= configuration.routes[i].depth == depth && configuration.routes[i].prefix_be == htonl(address);
        if (duplicate)
            continue;
        unsigned int choice = random_u32() & 1;
        configuration.routes[configuration.route_count++] = (struct dppd_ipv4_route){.prefix_be = htonl(address),
            .depth = depth,
            .port_id = choice ? 7 : 3,
            .gateway_be = ip(choice ? "192.0.2.2" : "192.0.2.1")};
    }
    assert(dppd_ipv4_router_replace(router, 1, &configuration, NULL, NULL) == 0);
    assert(dppd_ipv4_router_reader_register(router, 0) == 0);
    for (unsigned int sample = 0; sample < 20000; ++sample) {
        uint32_t destination = (random_u32() & 0x7fffffffU) | 0x01000000U;
        if ((destination >> 24) == 127)
            destination ^= 0x40000000U;
        const struct dppd_ipv4_route *selected = &configuration.routes[0];
        for (uint32_t i = 1; i < configuration.route_count; ++i) {
            const struct dppd_ipv4_route *route = &configuration.routes[i];
            uint32_t mask = UINT32_MAX << (32U - route->depth);
            if ((destination & mask) == ntohl(route->prefix_be) && route->depth > selected->depth)
                selected = route;
        }
        struct dppd_ipv4_forward forward;
        assert(dppd_ipv4_router_lookup(router, htonl(destination), &forward) == 0);
        assert(forward.port_id == selected->port_id && forward.next_hop_be == selected->gateway_be);
        dppd_ipv4_router_reader_quiescent(router, 0);
    }
    dppd_ipv4_router_reader_unregister(router, 0);
    assert(dppd_ipv4_router_destroy(router) == 0);
}

struct concurrent_context {
    struct dppd_ipv4_router *router;
    atomic_bool stop;
    atomic_uint ready;
    atomic_uint_fast64_t observations;
};

struct reader_context {
    struct concurrent_context *shared;
    unsigned int id;
};

static void *read_packets(void *argument)
{
    struct reader_context *reader = argument;
    struct concurrent_context *context = reader->shared;
    assert(dppd_ipv4_router_reader_register(context->router, reader->id) == 0);
    atomic_fetch_add_explicit(&context->ready, 1, memory_order_release);
    uint64_t observations = 0;
    while (!atomic_load_explicit(&context->stop, memory_order_acquire)) {
        struct dppd_ipv4_forward forward;
        assert(dppd_ipv4_router_lookup(context->router, ip("198.18.1.2"), &forward) == 0);
        /* 奇偶版本同时决定路由出口和邻居 MAC，混用两张表会立即暴露 */
        bool alternate = (forward.generation & 1U) != 0;
        assert(forward.port_id == (alternate ? 7 : 3));
        assert(forward.destination_mac[5] == (alternate ? 77 : 33));
        observations++;
        dppd_ipv4_router_reader_quiescent(context->router, reader->id);
    }
    dppd_ipv4_router_reader_unregister(context->router, reader->id);
    atomic_fetch_add_explicit(&context->observations, observations, memory_order_relaxed);
    return NULL;
}

static void test_atomic_replace(void)
{
    struct concurrent_context context = {.router = empty_router()};
    atomic_init(&context.stop, false);
    atomic_init(&context.ready, 0);
    atomic_init(&context.observations, 0);
    struct dppd_ipv4_configuration configuration = {.route_count = 1, .neighbor_count = 1};
    configuration.routes[0] = (struct dppd_ipv4_route){.port_id = 3, .gateway_be = ip("192.0.2.1")};
    configuration.neighbors[0] =
        (struct dppd_ipv4_neighbor){.port_id = 3, .address_be = ip("192.0.2.1"), .mac = {2, 0, 0, 0, 0, 33}};
    assert(dppd_ipv4_router_replace(context.router, 1, &configuration, NULL, NULL) == 0);
    pthread_t threads[3];
    struct reader_context readers[3];
    for (unsigned int i = 0; i < 3; ++i) {
        readers[i] = (struct reader_context){.shared = &context, .id = i};
        assert(pthread_create(&threads[i], NULL, read_packets, &readers[i]) == 0);
    }
    while (atomic_load_explicit(&context.ready, memory_order_acquire) != 3)
        sched_yield();
    for (uint64_t generation = 2; generation < 2002; ++generation) {
        bool alternate = ((generation + 1) & 1U) != 0;
        configuration.routes[0].port_id = configuration.neighbors[0].port_id = alternate ? 7 : 3;
        configuration.neighbors[0].mac[5] = alternate ? 77 : 33;
        int rc;
        do {
            rc = dppd_ipv4_router_replace(context.router, generation, &configuration, NULL, NULL);
            if (rc == -EAGAIN)
                sched_yield();
        } while (rc == -EAGAIN);
        assert(rc == 0);
    }
    atomic_store_explicit(&context.stop, true, memory_order_release);
    for (unsigned int i = 0; i < 3; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    assert(atomic_load_explicit(&context.observations, memory_order_relaxed) > 0);
    assert(dppd_ipv4_router_destroy(context.router) == 0);
}

/** 暂停的合法读者只使更新返回重试，不允许无限累积旧表或提前释放它 */
static void test_reclamation_limit(void)
{
    struct dppd_ipv4_router *router = empty_router();
    assert(dppd_ipv4_router_reader_register(router, 0) == 0);
    struct dppd_ipv4_configuration configuration = {0};
    for (uint64_t generation = 1; generation <= DPPD_IPV4_RETIRED_LIMIT; ++generation)
        assert(dppd_ipv4_router_replace(router, generation, &configuration, NULL, NULL) == 0);
    assert(dppd_ipv4_router_replace(router, DPPD_IPV4_RETIRED_LIMIT + 1, &configuration, NULL, NULL) == -EAGAIN);
    dppd_ipv4_router_reader_quiescent(router, 0);
    assert(dppd_ipv4_router_replace(router, DPPD_IPV4_RETIRED_LIMIT + 1, &configuration, NULL, NULL) == 0);
    dppd_ipv4_router_reader_unregister(router, 0);
    assert(dppd_ipv4_router_destroy(router) == 0);
}

int main(void)
{
    test_lookup();
    test_rejection();
    test_reference();
    test_atomic_replace();
    test_reclamation_limit();
    puts("IPv4 LPM, static neighbors, rejection and concurrent snapshot tests passed");
    return 0;
}
