#include "flow_routing_io.h"

#include <errno.h>
#include <string.h>

static void put(uint8_t *data, uint64_t value, unsigned int width)
{
    for (unsigned int i = 0; i < width; ++i)
        data[i] = (uint8_t)(value >> (i * 8U));
}

static uint64_t get(const uint8_t *data, unsigned int width)
{
    uint64_t value = 0;
    for (unsigned int i = 0; i < width; ++i)
        value |= (uint64_t)data[i] << (i * 8U);
    return value;
}

static bool counts_valid(const struct dppd_ipv4_configuration *c)
{
    return c->route_count <= DPPD_IPV4_ROUTE_LIMIT && c->route6_count <= DPPD_IPV4_ROUTE_LIMIT &&
        c->neighbor_count <= DPPD_IPV4_NEIGHBOR_LIMIT && c->neighbor6_count <= DPPD_IPV4_NEIGHBOR_LIMIT;
}

int dppd_flow_routing_encode(const struct dppd_ipv4_configuration *c, uint8_t *data, size_t capacity, size_t *length)
{
    if (c == NULL || data == NULL || length == NULL || !counts_valid(c))
        return -EINVAL;
    size_t size = 40U + 16U * c->route_count + 16U * c->neighbor_count + 40U * c->route6_count + 24U * c->neighbor6_count;
    if (size > capacity)
        return -E2BIG;
    memset(data, 0, size);
    put(data, c->generation, 8);
    put(data + 8, c->route_count, 8);
    put(data + 16, c->neighbor_count, 8);
    put(data + 24, c->route6_count, 8);
    put(data + 32, c->neighbor6_count, 8);
    size_t offset = 40;
    for (uint32_t i = 0; i < c->route_count; ++i, offset += 16) {
        const struct dppd_ipv4_route *r = &c->routes[i];
        if (r->reserved != 0)
            return -EINVAL;
        memcpy(data + offset, &r->prefix_be, 4);
        memcpy(data + offset + 4, &r->gateway_be, 4);
        put(data + offset + 8, r->port_id, 2);
        data[offset + 10] = r->depth;
    }
    for (uint32_t i = 0; i < c->neighbor_count; ++i, offset += 16) {
        const struct dppd_ipv4_neighbor *n = &c->neighbors[i];
        memcpy(data + offset, &n->address_be, 4);
        put(data + offset + 4, n->port_id, 2);
        memcpy(data + offset + 6, n->mac, 6);
    }
    for (uint32_t i = 0; i < c->route6_count; ++i, offset += 40) {
        const struct dppd_ipv6_route *r = &c->routes6[i];
        if (r->reserved != 0)
            return -EINVAL;
        memcpy(data + offset, r->prefix, 16);
        memcpy(data + offset + 16, r->gateway, 16);
        put(data + offset + 32, r->port_id, 2);
        data[offset + 34] = r->depth;
    }
    for (uint32_t i = 0; i < c->neighbor6_count; ++i, offset += 24) {
        const struct dppd_ipv6_neighbor *n = &c->neighbors6[i];
        memcpy(data + offset, n->address, 16);
        put(data + offset + 16, n->port_id, 2);
        memcpy(data + offset + 18, n->mac, 6);
    }
    *length = size;
    return 0;
}

int dppd_flow_routing_decode(const uint8_t *data, size_t length, struct dppd_ipv4_configuration *c)
{
    if (data == NULL || c == NULL || length < 40 || length > DPPD_FLOW_ROUTING_BYTES)
        return -EBADMSG;
    /** 先用完整宽度检查数量，避免恶意大整数截断后绕过数组边界 */
    for (unsigned int i = 1; i <= 4; ++i)
        if (get(data + i * 8U, 8) > DPPD_IPV4_ROUTE_LIMIT)
            return -EBADMSG;
    memset(c, 0, sizeof(*c));
    c->generation = get(data, 8);
    c->route_count = get(data + 8, 8);
    c->neighbor_count = get(data + 16, 8);
    c->route6_count = get(data + 24, 8);
    c->neighbor6_count = get(data + 32, 8);
    size_t expected = 40U + 16U * c->route_count + 16U * c->neighbor_count + 40U * c->route6_count + 24U * c->neighbor6_count;
    if (length != expected)
        return -EBADMSG;
    size_t offset = 40;
    for (uint32_t i = 0; i < c->route_count; ++i, offset += 16) {
        struct dppd_ipv4_route *r = &c->routes[i];
        memcpy(&r->prefix_be, data + offset, 4);
        memcpy(&r->gateway_be, data + offset + 4, 4);
        r->port_id = get(data + offset + 8, 2);
        r->depth = data[offset + 10];
    }
    for (uint32_t i = 0; i < c->neighbor_count; ++i, offset += 16) {
        struct dppd_ipv4_neighbor *n = &c->neighbors[i];
        memcpy(&n->address_be, data + offset, 4);
        n->port_id = get(data + offset + 4, 2);
        memcpy(n->mac, data + offset + 6, 6);
    }
    for (uint32_t i = 0; i < c->route6_count; ++i, offset += 40) {
        struct dppd_ipv6_route *r = &c->routes6[i];
        memcpy(r->prefix, data + offset, 16);
        memcpy(r->gateway, data + offset + 16, 16);
        r->port_id = get(data + offset + 32, 2);
        r->depth = data[offset + 34];
    }
    for (uint32_t i = 0; i < c->neighbor6_count; ++i, offset += 24) {
        struct dppd_ipv6_neighbor *n = &c->neighbors6[i];
        memcpy(n->address, data + offset, 16);
        n->port_id = get(data + offset + 16, 2);
        memcpy(n->mac, data + offset + 18, 6);
    }
    /** 所有保留字节必须为零，不能只校验 CRC 后接收另一种不明确的文件表达 */
    uint8_t canonical[DPPD_FLOW_ROUTING_BYTES];
    size_t written;
    int rc = dppd_flow_routing_encode(c, canonical, sizeof(canonical), &written);
    return rc == 0 && written == length && memcmp(data, canonical, length) == 0 ? 0 : -EBADMSG;
}

int dppd_flow_routing_validate(const struct dppd_flow_routing_state *r)
{
    if (r == NULL || r->binding_count == 0 || r->binding_count > DPPD_MAX_PORTS || r->configuration.generation == 0)
        return -EINVAL;
    uint16_t ports[DPPD_MAX_PORTS];
    for (uint16_t i = 0; i < r->binding_count; ++i) {
        const struct dppd_routing_binding *b = &r->bindings[i];
        if (b->port_id == UINT16_MAX || b->device[0] == '\0' || b->driver[0] == '\0' ||
            memchr(b->device, 0, sizeof(b->device)) == NULL || memchr(b->driver, 0, sizeof(b->driver)) == NULL ||
            memchr(b->ifname, 0, sizeof(b->ifname)) == NULL)
            return -EINVAL;
        if (strcmp(b->driver, "net_tap") == 0 && b->ifname[0] == '\0')
            return -EINVAL;
        for (uint16_t j = 0; j < i; ++j)
            if (b->port_id == ports[j] || strcmp(b->device, r->bindings[j].device) == 0)
                return -EINVAL;
        ports[i] = b->port_id;
    }
    /** 与真实路由器共用前缀、下一跳、重复项和 MAC 校验，文件检查不会另造一套宽松规则 */
    struct dppd_ipv4_router *probe = NULL;
    int rc = dppd_ipv4_router_create(&probe, ports, r->binding_count, &r->configuration);
    if (rc == 0)
        rc = dppd_ipv4_router_destroy(probe);
    return rc;
}
