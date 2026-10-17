#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dppd/ipv6.h"
#include "dppd/routing.h"
#include "dppd/persistence.h"

static bool directory_failure;
int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    struct stat info;
    assert(fstat(fd, &info) == 0);
    if (directory_failure && S_ISDIR(info.st_mode)) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

static void address(const char *text, uint8_t bytes[16])
{
    assert(inet_pton(AF_INET6, text, bytes) == 1);
}

static uint32_t random_state = 0x6abcd123;
static uint32_t random_value(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void configuration(struct dppd_ipv4_configuration *value)
{
    memset(value, 0, sizeof(*value));
    value->generation = 1;
    value->route6_count = 129;
    value->neighbor6_count = 2;
    for (unsigned int i = 0; i < 129; ++i) {
        struct dppd_ipv6_route *route = &value->routes6[i];
        route->depth = (uint8_t)i;
        for (unsigned int byte = 0; byte < 16; ++byte)
            route->prefix[byte] = (uint8_t)random_value();
        for (unsigned int bit = i; bit < 128; ++bit)
            route->prefix[bit / 8] &= (uint8_t) ~(0x80U >> (bit % 8));
        route->port_id = i % 2 == 0 ? 3 : 7;
        address(i % 2 == 0 ? "fe80::1" : "fe80::2", route->gateway);
    }
    for (unsigned int i = 0; i < 2; ++i) {
        struct dppd_ipv6_neighbor *neighbor = &value->neighbors6[i];
        neighbor->port_id = i == 0 ? 3 : 7;
        address(i == 0 ? "fe80::1" : "fe80::2", neighbor->address);
        neighbor->mac[0] = 2;
        neighbor->mac[5] = (uint8_t)neighbor->port_id;
    }
    value->route_count = value->neighbor_count = 1;
    value->routes[0].port_id = value->neighbors[0].port_id = 3;
    value->routes[0].gateway_be = value->neighbors[0].address_be = htonl(0xc0000201);
    value->neighbors[0].mac[0] = 2;
}

/** 线性参考按完整字节和剩余位比较，不复用生产树查找，覆盖全部一百二十九种前缀长度 */
static void lookup(void)
{
    struct dppd_ipv4_configuration input;
    configuration(&input);
    struct dppd_ipv4_router *router = NULL;
    assert(dppd_ipv4_router_create(&router, (uint16_t[]){3, 7}, 2, &input) == 0);
    assert(dppd_ipv4_router_reader_register(router, 0) == 0);
    for (unsigned int sample = 0; sample < 30000; ++sample) {
        uint8_t destination[16];
        for (unsigned int byte = 0; byte < 16; ++byte)
            destination[byte] = (uint8_t)random_value();
        if (sample < 129)
            memcpy(destination, input.routes6[sample].prefix, 16);
        if (!dppd_ipv6_unicast(destination, false))
            continue;
        const struct dppd_ipv6_route *selected = NULL;
        for (unsigned int i = 0; i < input.route6_count; ++i) {
            const struct dppd_ipv6_route *route = &input.routes6[i];
            unsigned int bytes = route->depth / 8, bits = route->depth % 8;
            if (memcmp(destination, route->prefix, bytes) != 0 ||
                (bits != 0 && ((destination[bytes] ^ route->prefix[bytes]) & (255U << (8 - bits))) != 0))
                continue;
            if (selected == NULL || route->depth > selected->depth)
                selected = route;
        }
        struct dppd_ipv4_forward forward;
        assert(selected != NULL && dppd_ipv6_router_lookup(router, destination, &forward) == 0);
        assert(forward.port_id == selected->port_id && forward.destination_mac[5] == selected->port_id);
    }
    struct dppd_ipv4_forward forward;
    uint8_t link_local[16];
    address("fe80::3", link_local);
    assert(dppd_ipv6_router_lookup(router, link_local, &forward) == -EAFNOSUPPORT);
    assert(dppd_ipv4_router_lookup(router, htonl(0xc6336401), &forward) == 0 && forward.port_id == 3);
    input.routes6[128].reserved = 1;
    assert(dppd_ipv4_router_replace(router, 1, &input, NULL, NULL) == -EINVAL);
    input.routes6[128].reserved = 0;
    input.route6_count = 0;
    assert(dppd_ipv4_router_replace(router, 1, &input, NULL, NULL) == 0);
    address("2001:db8::7", link_local);
    assert(dppd_ipv6_router_lookup(router, link_local, &forward) == -ENOENT);
    assert(dppd_ipv4_router_lookup(router, htonl(0xc6336401), &forward) == 0);
    dppd_ipv4_router_reader_quiescent(router, 0);
    dppd_ipv4_router_reader_unregister(router, 0);
    assert(dppd_ipv4_router_destroy(router) == 0);
}

/** IPv4 旧文件不强制改版，首次双栈保存才扩展；目录同步失败保持旧活跃表并可显式修复 */
static void durable(const char *directory)
{
    char path[256], lock[272];
    snprintf(path, sizeof(path), "%s/routes", directory);
    snprintf(lock, sizeof(lock), "%s.lock", path);
    const struct dppd_routing_binding bindings[] = {{.port_id = 3, .device = "test3", .driver = "net_ring"},
        {.port_id = 7, .device = "test7", .driver = "net_ring"}};
    struct dppd_routing_service *service = NULL;
    assert(dppd_routing_open(&service, path, bindings, 2) == 0);
    struct stat info;
    assert(stat(path, &info) == 0 && info.st_size == DPPD_ROUTING_FILE_SIZE);
    struct dppd_ipv4_configuration input;
    configuration(&input);
    /** 首次从旧版升级时目录同步失败，修复仍保留旧活跃空表，且不会错误降回旧文件长度 */
    directory_failure = true;
    assert(dppd_routing_replace(service, 1, &input) == -EUCLEAN);
    directory_failure = false;
    assert(dppd_routing_flush(service) == 0);
    struct dppd_routing_status repaired;
    dppd_routing_status(service, &repaired);
    assert(!repaired.dirty && repaired.generation == 1 && repaired.route6_count == 0);
    assert(stat(path, &info) == 0 && info.st_size == DPPD_ROUTING_DUAL_FILE_SIZE);
    assert(dppd_routing_replace(service, 1, &input) == 0);
    assert(stat(path, &info) == 0 && info.st_size == DPPD_ROUTING_DUAL_FILE_SIZE);
    assert(dppd_routing_close(service) == 0);
    service = NULL;
    assert(dppd_routing_open(&service, path, bindings, 2) == 0);
    assert(dppd_routing_get(service, &input) == 0 && input.generation == 2 && input.route6_count == 129);
    input.route6_count = input.neighbor6_count = 0;
    directory_failure = true;
    assert(dppd_routing_replace(service, 2, &input) == -EUCLEAN);
    directory_failure = false;
    struct dppd_routing_status status;
    dppd_routing_status(service, &status);
    assert(status.dirty && status.generation == 2 && status.route6_count == 129);
    assert(dppd_routing_flush(service) == 0);
    assert(dppd_routing_close(service) == 0);
    service = NULL;
    assert(dppd_routing_open(&service, path, bindings, 2) == 0);
    dppd_routing_status(service, &status);
    assert(!status.dirty && status.generation == 2 && status.route6_count == 129 && status.route_count == 1);
    assert(dppd_routing_close(service) == 0);
    assert(unlink(path) == 0 && unlink(lock) == 0);
}

/** v3 记录完整保存四组 IPv6 字节数组，混合规则的版本、安装端口和原动作不能丢失 */
static void persistence(const char *directory)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/rules", directory);
    struct dppd_rule_repository repository;
    assert(dppd_rule_repository_init(&repository, 4) == 0);
    struct dppd_rule value = {.id = 9,
        .install_port_id = 7,
        .nb_matches = 2,
        .nb_actions = 1,
        .fallback = DPPD_FALLBACK_SOFTWARE_ONLY};
    value.matches[0].type = DPPD_MATCH_ETH;
    value.matches[1].type = DPPD_MATCH_IPV6;
    value.actions[0].type = DPPD_ACTION_ALLOW;
    for (unsigned int i = 0; i < 16; ++i) {
        value.matches[1].spec.ipv6.src[i] = (uint8_t)i;
        value.matches[1].spec.ipv6.src_mask[i] = (uint8_t)(0xf0U | i);
        value.matches[1].spec.ipv6.dst[i] = (uint8_t)(i * 7);
        value.matches[1].spec.ipv6.dst_mask[i] = (uint8_t)(255 - i);
    }
    struct dppd_rule_apply_result result;
    assert(dppd_rule_repository_apply(&repository, &value, 0, &result) == 0);
    value.id = 10;
    memset(value.matches + 1, 0, sizeof(value.matches[1]));
    value.matches[1].type = DPPD_MATCH_IPV4;
    assert(dppd_rule_repository_apply(&repository, &value, 0, &result) == 0);
    assert(dppd_persistence_save(path, &repository) == 0);
    struct dppd_persisted_snapshot snapshot;
    assert(dppd_persistence_load(path, &snapshot) == 0 && snapshot.count == 2);
    for (unsigned int i = 0; i < 2; ++i) {
        struct dppd_rule original;
        assert(dppd_rule_repository_get(&repository, 9 + i, &original) == 0);
        assert(dppd_rule_equal(&snapshot.rules[i], &original) && snapshot.rules[i].generation == original.generation);
    }
    dppd_persisted_snapshot_destroy(&snapshot);
    int fd = open(path, O_RDWR);
    uint8_t version;
    assert(fd >= 0 && pread(fd, &version, 1, 8) == 1 && version == 3);
    uint8_t byte;
    assert(pread(fd, &byte, 1, 100) == 1);
    byte ^= 1;
    assert(pwrite(fd, &byte, 1, 100) == 1 && close(fd) == 0);
    assert(dppd_persistence_load(path, &snapshot) == -EBADMSG);
    dppd_rule_repository_destroy(&repository);
    assert(unlink(path) == 0);
}

int main(void)
{
    lookup();
    char directory[] = "/tmp/dppd-ipv6-unit-XXXXXX";
    assert(mkdtemp(directory) != NULL);
    durable(directory);
    persistence(directory);
    assert(rmdir(directory) == 0);
    return 0;
}
