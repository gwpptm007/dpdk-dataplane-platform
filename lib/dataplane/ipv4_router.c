#include "dppd/ipv4_router.h"
#include "dppd/ipv6.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <rte_common.h>
/** 与既有软件后端保持相同的 DPDK 21.11 实验头文件诊断边界 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#pragma GCC diagnostic ignored "-Wpedantic"
#include <rte_rcu_qsbr.h>
#pragma GCC diagnostic pop

/** 零表示没有子节点或匹配项，路由索引加一保存，根节点始终为零号节点 */
struct route_node {
    uint32_t child[2], route;
};

struct routing_snapshot {
    struct dppd_ipv4_configuration configuration;
    struct routing_snapshot *retired_next;
    uint64_t retired_token;
    uint32_t node_count, root6;
    struct route_node nodes[];
};

struct dppd_ipv4_router {
    _Atomic(struct routing_snapshot *) active;
    struct routing_snapshot *retired;
    struct rte_rcu_qsbr *qsbr;
    pthread_mutex_t writer;
    atomic_uint_fast64_t readers;
    uint32_t retired_count;
    uint16_t port_count, ports[DPPD_MAX_PORTS];
};

static bool port_present(const struct dppd_ipv4_router *router, uint16_t port)
{
    for (uint16_t i = 0; i < router->port_count; ++i)
        if (router->ports[i] == port)
            return true;
    return false;
}

/** 该阶段只做 IPv4 单播转发，零网段、回环、组播和保留高地址不能由默认路由放行 */
static bool unicast_address(uint32_t address_be)
{
    uint32_t address = ntohl(address_be);
    return (address >> 24) != 0 && (address >> 24) != 127 && address < 0xe0000000U;
}

static int compare_route(const void *left, const void *right)
{
    const struct dppd_ipv4_route *a = left, *b = right;
    uint32_t first = ntohl(a->prefix_be), second = ntohl(b->prefix_be);
    if (first != second)
        return first < second ? -1 : 1;
    return (int)a->depth - b->depth;
}

static int compare_neighbor(const void *left, const void *right)
{
    const struct dppd_ipv4_neighbor *a = left, *b = right;
    if (a->port_id != b->port_id)
        return a->port_id < b->port_id ? -1 : 1;
    uint32_t first = ntohl(a->address_be), second = ntohl(b->address_be);
    return first == second ? 0 : first < second ? -1 : 1;
}

static int compare_route6(const void *left, const void *right)
{
    const struct dppd_ipv6_route *a = left, *b = right;
    int address = memcmp(a->prefix, b->prefix, 16);
    return address != 0 ? address : (int)a->depth - b->depth;
}

static int compare_neighbor6(const void *left, const void *right)
{
    const struct dppd_ipv6_neighbor *a = left, *b = right;
    return a->port_id != b->port_id ? (int)a->port_id - b->port_id : memcmp(a->address, b->address, 16);
}

/** 每个地址族有独立根节点，IPv6 按线缆字节逐位建树，非整字节前缀也保持精确 */
static int compile6(const struct dppd_ipv4_router *router, struct routing_snapshot *snapshot)
{
    struct dppd_ipv4_configuration *configuration = &snapshot->configuration;
    qsort(configuration->routes6, configuration->route6_count, sizeof(*configuration->routes6), compare_route6);
    qsort(configuration->neighbors6, configuration->neighbor6_count, sizeof(*configuration->neighbors6),
        compare_neighbor6);
    const uint8_t zero[16] = {0};
    snapshot->root6 = snapshot->node_count++;
    for (uint32_t i = 0; i < configuration->route6_count; ++i) {
        const struct dppd_ipv6_route *route = &configuration->routes6[i];
        if (route->depth > 128 || route->reserved != 0 || !port_present(router, route->port_id) ||
            (memcmp(route->gateway, zero, 16) != 0 && !dppd_ipv6_unicast(route->gateway, true)))
            return -EINVAL;
        for (unsigned int bit = route->depth; bit < 128; ++bit)
            if ((route->prefix[bit / 8] & (0x80U >> (bit % 8))) != 0)
                return -EINVAL;
        if (i != 0 && compare_route6(route - 1, route) == 0)
            return -EEXIST;
        uint32_t node = snapshot->root6;
        for (unsigned int bit = 0; bit < route->depth; ++bit) {
            unsigned int branch = (route->prefix[bit / 8] >> (7U - bit % 8)) & 1U;
            if (snapshot->nodes[node].child[branch] == 0)
                snapshot->nodes[node].child[branch] = snapshot->node_count++;
            node = snapshot->nodes[node].child[branch];
        }
        snapshot->nodes[node].route = i + 1;
    }
    for (uint32_t i = 0; i < configuration->neighbor6_count; ++i) {
        const struct dppd_ipv6_neighbor *neighbor = &configuration->neighbors6[i];
        const uint8_t empty_mac[6] = {0};
        if (!port_present(router, neighbor->port_id) || !dppd_ipv6_unicast(neighbor->address, true) ||
            (neighbor->mac[0] & 1U) != 0 || memcmp(neighbor->mac, empty_mac, 6) == 0)
            return -EINVAL;
        if (i != 0 && compare_neighbor6(neighbor - 1, neighbor) == 0)
            return -EEXIST;
    }
    return 0;
}

/** 控制面先构造并验证全部内容，不能把半张路由表或无效邻居发布给工作线程 */
static int compile(const struct dppd_ipv4_router *router, const struct dppd_ipv4_configuration *input,
    struct routing_snapshot **result)
{
    if (input->route_count > DPPD_IPV4_ROUTE_LIMIT || input->neighbor_count > DPPD_IPV4_NEIGHBOR_LIMIT ||
        input->route6_count > DPPD_IPV4_ROUTE_LIMIT || input->neighbor6_count > DPPD_IPV4_NEIGHBOR_LIMIT)
        return -E2BIG;
    size_t capacity = (size_t)input->route_count * 32 + (size_t)input->route6_count * 128 + 2;
    struct routing_snapshot *snapshot = calloc(1, sizeof(*snapshot) + capacity * sizeof(struct route_node));
    if (snapshot == NULL)
        return -ENOMEM;
    struct dppd_ipv4_configuration *configuration = &snapshot->configuration;
    configuration->generation = input->generation;
    configuration->route_count = input->route_count;
    configuration->neighbor_count = input->neighbor_count;
    memcpy(configuration->routes, input->routes, input->route_count * sizeof(input->routes[0]));
    memcpy(configuration->neighbors, input->neighbors, input->neighbor_count * sizeof(input->neighbors[0]));
    configuration->route6_count = input->route6_count;
    configuration->neighbor6_count = input->neighbor6_count;
    memcpy(configuration->routes6, input->routes6, input->route6_count * sizeof(*input->routes6));
    memcpy(configuration->neighbors6, input->neighbors6, input->neighbor6_count * sizeof(*input->neighbors6));
    qsort(configuration->routes, configuration->route_count, sizeof(configuration->routes[0]), compare_route);
    qsort(configuration->neighbors, configuration->neighbor_count, sizeof(configuration->neighbors[0]),
        compare_neighbor);
    int rc = -EINVAL;
    snapshot->node_count = 1;
    for (uint32_t i = 0; i < configuration->route_count; ++i) {
        const struct dppd_ipv4_route *route = &configuration->routes[i];
        if (route->depth > 32 || route->reserved != 0 || !port_present(router, route->port_id) ||
            (route->gateway_be != 0 && !unicast_address(route->gateway_be)))
            goto fail;
        uint32_t address = ntohl(route->prefix_be);
        uint32_t mask = route->depth == 0 ? 0 : UINT32_MAX << (32U - route->depth);
        if ((address & mask) != address)
            goto fail;
        if (i != 0 && compare_route(route - 1, route) == 0) {
            rc = -EEXIST;
            goto fail;
        }
        uint32_t node = 0;
        for (uint8_t bit = 0; bit < route->depth; ++bit) {
            unsigned int branch = (address >> (31U - bit)) & 1U;
            if (snapshot->nodes[node].child[branch] == 0)
                snapshot->nodes[node].child[branch] = snapshot->node_count++;
            node = snapshot->nodes[node].child[branch];
        }
        snapshot->nodes[node].route = i + 1;
    }
    for (uint32_t i = 0; i < configuration->neighbor_count; ++i) {
        const struct dppd_ipv4_neighbor *neighbor = &configuration->neighbors[i];
        const uint8_t zero[6] = {0};
        if (!port_present(router, neighbor->port_id) || !unicast_address(neighbor->address_be) ||
            (neighbor->mac[0] & 1U) != 0 || memcmp(neighbor->mac, zero, sizeof(zero)) == 0)
            goto fail;
        if (i != 0 && compare_neighbor(neighbor - 1, neighbor) == 0) {
            rc = -EEXIST;
            goto fail;
        }
    }
    rc = compile6(router, snapshot);
    if (rc != 0)
        goto fail;
    *result = snapshot;
    return 0;
fail:
    free(snapshot);
    return rc;
}

/** 不阻塞数据面或等待慢读者，超过退役上限时让下一次配置更新显式重试 */
static void reclaim(struct dppd_ipv4_router *router)
{
    struct routing_snapshot **link = &router->retired;
    while (*link != NULL) {
        struct routing_snapshot *snapshot = *link;
        if (!rte_rcu_qsbr_check(router->qsbr, snapshot->retired_token, false)) {
            link = &snapshot->retired_next;
            continue;
        }
        *link = snapshot->retired_next;
        router->retired_count--;
        free(snapshot);
    }
}

int dppd_ipv4_router_create(struct dppd_ipv4_router **result, const uint16_t *ports, uint16_t count,
    const struct dppd_ipv4_configuration *initial)
{
    if (result == NULL || *result != NULL || ports == NULL || count == 0 || count > DPPD_MAX_PORTS ||
        (initial != NULL && initial->generation == 0))
        return -EINVAL;
    struct dppd_ipv4_router *router = calloc(1, sizeof(*router));
    if (router == NULL)
        return -ENOMEM;
    int rc = -EINVAL;
    router->port_count = count;
    memcpy(router->ports, ports, count * sizeof(*ports));
    for (uint16_t i = 0; i < count; ++i) {
        if (ports[i] == UINT16_MAX)
            goto fail;
        for (uint16_t j = 0; j < i; ++j)
            if (ports[i] == ports[j])
                goto fail;
    }
    struct dppd_ipv4_configuration empty = {.generation = 1};
    struct routing_snapshot *snapshot = NULL;
    rc = compile(router, initial != NULL ? initial : &empty, &snapshot);
    if (rc != 0)
        goto fail;
    size_t size = rte_rcu_qsbr_get_memsize(DPPD_MAX_WORKERS);
    size = RTE_ALIGN_CEIL(size, RTE_CACHE_LINE_SIZE);
    router->qsbr = aligned_alloc(RTE_CACHE_LINE_SIZE, size);
    if (router->qsbr == NULL || rte_rcu_qsbr_init(router->qsbr, DPPD_MAX_WORKERS) != 0) {
        rc = -ENOMEM;
        free(snapshot);
        goto fail;
    }
    int lock_error = pthread_mutex_init(&router->writer, NULL);
    if (lock_error != 0) {
        rc = -lock_error;
        free(snapshot);
        goto fail;
    }
    atomic_init(&router->readers, 0);
    atomic_init(&router->active, snapshot);
    *result = router;
    return 0;
fail:
    free(router->qsbr);
    free(router);
    return rc;
}

int dppd_ipv4_router_destroy(struct dppd_ipv4_router *router)
{
    if (router == NULL)
        return 0;
    if (atomic_load_explicit(&router->readers, memory_order_acquire) != 0)
        return -EBUSY;
    free(atomic_load_explicit(&router->active, memory_order_relaxed));
    struct routing_snapshot *snapshot = router->retired;
    while (snapshot != NULL) {
        struct routing_snapshot *next = snapshot->retired_next;
        free(snapshot);
        snapshot = next;
    }
    pthread_mutex_destroy(&router->writer);
    free(router->qsbr);
    free(router);
    return 0;
}

int dppd_ipv6_router_lookup(struct dppd_ipv4_router *router, const uint8_t destination[16],
    struct dppd_ipv4_forward *forward)
{
    if (router == NULL || destination == NULL || forward == NULL)
        return -EINVAL;
    memset(forward, 0, sizeof(*forward));
    if (!dppd_ipv6_unicast(destination, false))
        return -EAFNOSUPPORT;
    const struct routing_snapshot *snapshot = atomic_load_explicit(&router->active, memory_order_acquire);
    const struct dppd_ipv4_configuration *configuration = &snapshot->configuration;
    uint32_t node = snapshot->root6, selected = snapshot->nodes[node].route;
    for (unsigned int bit = 0; bit < 128; ++bit) {
        node = snapshot->nodes[node].child[(destination[bit / 8] >> (7U - bit % 8)) & 1U];
        if (node == 0)
            break;
        if (snapshot->nodes[node].route != 0)
            selected = snapshot->nodes[node].route;
    }
    if (selected == 0)
        return -ENOENT;
    const struct dppd_ipv6_route *route = &configuration->routes6[selected - 1];
    struct dppd_ipv6_neighbor key = {.port_id = route->port_id};
    const uint8_t zero[16] = {0};
    memcpy(key.address, memcmp(route->gateway, zero, 16) == 0 ? destination : route->gateway, 16);
    const struct dppd_ipv6_neighbor *neighbor =
        bsearch(&key, configuration->neighbors6, configuration->neighbor6_count, sizeof(key), compare_neighbor6);
    forward->generation = configuration->generation;
    forward->port_id = route->port_id;
    memcpy(forward->next_hop6, key.address, 16);
    /** 缺少静态邻居时仍返回已匹配的出口和下一跳，供独立 ND 缓存继续解析 */
    if (neighbor == NULL)
        return -EHOSTUNREACH;
    memcpy(forward->destination_mac, neighbor->mac, 6);
    return 0;
}

int dppd_ipv4_router_get(struct dppd_ipv4_router *router, struct dppd_ipv4_configuration *configuration)
{
    if (router == NULL || configuration == NULL)
        return -EINVAL;
    pthread_mutex_lock(&router->writer);
    *configuration = atomic_load_explicit(&router->active, memory_order_relaxed)->configuration;
    reclaim(router);
    pthread_mutex_unlock(&router->writer);
    return 0;
}

int dppd_ipv4_router_replace(struct dppd_ipv4_router *router, uint64_t expected,
    const struct dppd_ipv4_configuration *configuration,
    int (*durable)(void *context, const struct dppd_ipv4_configuration *candidate), void *context)
{
    if (router == NULL || configuration == NULL || expected == 0)
        return -EINVAL;
    pthread_mutex_lock(&router->writer);
    struct routing_snapshot *old = atomic_load_explicit(&router->active, memory_order_relaxed), *next = NULL;
    int rc = old->configuration.generation != expected ? -ESTALE : expected == UINT64_MAX ? -EOVERFLOW : 0;
    reclaim(router);
    if (rc == 0 && router->retired_count >= DPPD_IPV4_RETIRED_LIMIT)
        rc = -EAGAIN;
    if (rc == 0)
        rc = compile(router, configuration, &next);
    if (rc == 0) {
        next->configuration.generation = expected + 1;
        if (durable != NULL)
            rc = durable(context, &next->configuration);
    }
    if (rc == 0) {
        atomic_store_explicit(&router->active, next, memory_order_release);
        old->retired_token = rte_rcu_qsbr_start(router->qsbr);
        old->retired_next = router->retired;
        router->retired = old;
        router->retired_count++;
        reclaim(router);
    } else
        free(next);
    pthread_mutex_unlock(&router->writer);
    return rc;
}

int dppd_ipv4_router_reader_register(struct dppd_ipv4_router *router, unsigned int reader)
{
    if (router == NULL || reader >= DPPD_MAX_WORKERS)
        return -EINVAL;
    uint64_t bit = UINT64_C(1) << reader;
    if ((atomic_fetch_or_explicit(&router->readers, bit, memory_order_acq_rel) & bit) != 0)
        return -EALREADY;
    if (rte_rcu_qsbr_thread_register(router->qsbr, reader) != 0) {
        atomic_fetch_and_explicit(&router->readers, ~bit, memory_order_release);
        return -EINVAL;
    }
    rte_rcu_qsbr_thread_online(router->qsbr, reader);
    return 0;
}

void dppd_ipv4_router_reader_quiescent(struct dppd_ipv4_router *router, unsigned int reader)
{
    if (router != NULL && reader < DPPD_MAX_WORKERS)
        rte_rcu_qsbr_quiescent(router->qsbr, reader);
}

void dppd_ipv4_router_reader_unregister(struct dppd_ipv4_router *router, unsigned int reader)
{
    if (router == NULL || reader >= DPPD_MAX_WORKERS)
        return;
    rte_rcu_qsbr_thread_offline(router->qsbr, reader);
    rte_rcu_qsbr_thread_unregister(router->qsbr, reader);
    atomic_fetch_and_explicit(&router->readers, ~(UINT64_C(1) << reader), memory_order_release);
}

int dppd_ipv4_router_lookup(struct dppd_ipv4_router *router, uint32_t destination_be, struct dppd_ipv4_forward *forward)
{
    if (router == NULL || forward == NULL)
        return -EINVAL;
    memset(forward, 0, sizeof(*forward));
    if (!unicast_address(destination_be))
        return -EAFNOSUPPORT;
    const struct routing_snapshot *snapshot = atomic_load_explicit(&router->active, memory_order_acquire);
    const struct dppd_ipv4_configuration *configuration = &snapshot->configuration;
    uint32_t address = ntohl(destination_be), node = 0, selected = snapshot->nodes[0].route;
    for (unsigned int bit = 0; bit < 32; ++bit) {
        node = snapshot->nodes[node].child[(address >> (31U - bit)) & 1U];
        if (node == 0)
            break;
        if (snapshot->nodes[node].route != 0)
            selected = snapshot->nodes[node].route;
    }
    if (selected == 0)
        return -ENOENT;
    const struct dppd_ipv4_route *route = &configuration->routes[selected - 1];
    struct dppd_ipv4_neighbor key = {.port_id = route->port_id,
        .address_be = route->gateway_be != 0 ? route->gateway_be : destination_be};
    const struct dppd_ipv4_neighbor *neighbor =
        bsearch(&key, configuration->neighbors, configuration->neighbor_count, sizeof(key), compare_neighbor);
    if (neighbor == NULL)
        return -EHOSTUNREACH;
    forward->generation = configuration->generation;
    forward->port_id = route->port_id;
    forward->next_hop_be = key.address_be;
    memcpy(forward->destination_mac, neighbor->mac, sizeof(neighbor->mac));
    return 0;
}
