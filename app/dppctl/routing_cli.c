#define _GNU_SOURCE
#include "routing_cli.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int integer(const char *text, uint64_t maximum, uint64_t *result)
{
    if (text == NULL || *text < '0' || *text > '9')
        return -EINVAL;
    char *end;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || value > maximum)
        return -EINVAL;
    *result = value;
    return 0;
}

static int prefix(const char *text, struct dppd_ipv4_route *route)
{
    char copy[INET_ADDRSTRLEN + 4];
    if (strlen(text) >= sizeof(copy))
        return -EINVAL;
    strcpy(copy, text);
    char *slash = strchr(copy, '/');
    if (slash == NULL)
        return -EINVAL;
    *slash++ = '\0';
    uint64_t depth;
    if (integer(slash, 32, &depth) != 0 || inet_pton(AF_INET, copy, &route->prefix_be) != 1)
        return -EINVAL;
    route->depth = (uint8_t)depth;
    uint32_t mask = depth == 0 ? 0 : UINT32_MAX << (32U - depth);
    route->prefix_be = htonl(ntohl(route->prefix_be) & mask);
    return 0;
}

static int route_entry(char **arguments, struct dppd_ipv4_route *route)
{
    uint64_t port;
    if (prefix(arguments[0], route) != 0 || integer(arguments[1], UINT16_MAX - 1U, &port) != 0)
        return -EINVAL;
    route->port_id = (uint16_t)port;
    if (strcmp(arguments[2], "direct") == 0)
        route->gateway_be = 0;
    else if (inet_pton(AF_INET, arguments[2], &route->gateway_be) != 1)
        return -EINVAL;
    return 0;
}

static int hex_digit(char digit)
{
    if (digit >= '0' && digit <= '9')
        return digit - '0';
    if (digit >= 'a' && digit <= 'f')
        return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F')
        return digit - 'A' + 10;
    return -1;
}

static int neighbor_key(char **arguments, struct dppd_ipv4_neighbor *neighbor)
{
    uint64_t port;
    if (integer(arguments[0], UINT16_MAX - 1U, &port) != 0 ||
        inet_pton(AF_INET, arguments[1], &neighbor->address_be) != 1)
        return -EINVAL;
    neighbor->port_id = (uint16_t)port;
    return 0;
}

static int mac_address(const char *mac, uint8_t bytes[6])
{
    if (strlen(mac) != 17)
        return -EINVAL;
    for (unsigned int i = 0; i < 6; ++i) {
        int first = hex_digit(mac[i * 3]), second = hex_digit(mac[i * 3 + 1]);
        if (first < 0 || second < 0 || (i < 5 && mac[i * 3 + 2] != ':'))
            return -EINVAL;
        bytes[i] = (uint8_t)((unsigned int)first * 16 + (unsigned int)second);
    }
    return 0;
}

static int neighbor_entry(char **arguments, struct dppd_ipv4_neighbor *neighbor)
{
    return neighbor_key(arguments, neighbor) == 0 ? mac_address(arguments[2], neighbor->mac) : -EINVAL;
}

/** IPv6 前缀仍做规范化，逐位清除主机位，处理 /65 等非整字节边界 */
static int prefix6(const char *text, struct dppd_ipv6_route *route)
{
    char copy[INET6_ADDRSTRLEN + 5];
    if (strlen(text) >= sizeof(copy))
        return -EINVAL;
    strcpy(copy, text);
    char *slash = strchr(copy, '/');
    if (slash == NULL)
        return -EINVAL;
    *slash++ = '\0';
    uint64_t depth;
    if (integer(slash, 128, &depth) != 0 || inet_pton(AF_INET6, copy, route->prefix) != 1)
        return -EINVAL;
    route->depth = (uint8_t)depth;
    for (unsigned int bit = (unsigned int)depth; bit < 128; ++bit)
        route->prefix[bit / 8] &= (uint8_t) ~(0x80U >> (bit % 8));
    return 0;
}

static int route6_entry(char **arguments, struct dppd_ipv6_route *route)
{
    uint64_t port;
    if (prefix6(arguments[0], route) != 0 || integer(arguments[1], UINT16_MAX - 1U, &port) != 0)
        return -EINVAL;
    route->port_id = (uint16_t)port;
    if (strcmp(arguments[2], "direct") == 0)
        memset(route->gateway, 0, 16);
    else if (inet_pton(AF_INET6, arguments[2], route->gateway) != 1)
        return -EINVAL;
    return 0;
}

static int neighbor6_key(char **arguments, struct dppd_ipv6_neighbor *neighbor)
{
    uint64_t port;
    if (integer(arguments[0], UINT16_MAX - 1U, &port) != 0 || inet_pton(AF_INET6, arguments[1], neighbor->address) != 1)
        return -EINVAL;
    neighbor->port_id = (uint16_t)port;
    return 0;
}

static int neighbor6_entry(char **arguments, struct dppd_ipv6_neighbor *neighbor)
{
    return neighbor6_key(arguments, neighbor) == 0 ? mac_address(arguments[2], neighbor->mac) : -EINVAL;
}

/** 整表文件只接受有界普通文件，每行一条 route 或 neighbor，任何一行错误都不发请求 */
static int read_configuration(const char *path, struct dppd_ipv4_configuration *configuration)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -errno;
    struct stat info;
    if (fstat(fd, &info) != 0) {
        int rc = -errno;
        close(fd);
        return rc;
    }
    if (!S_ISREG(info.st_mode) || info.st_size > 65536) {
        close(fd);
        return -EINVAL;
    }
    char *text = malloc(65537);
    if (text == NULL) {
        close(fd);
        return -ENOMEM;
    }
    size_t used = 0;
    int read_error = 0;
    for (;;) {
        ssize_t size = read(fd, text + used, 65537 - used);
        if (size < 0 && errno == EINTR)
            continue;
        if (size <= 0) {
            read_error = size < 0 ? -errno : 0;
            break;
        }
        used += (size_t)size;
        if (used > 65536) {
            read_error = -E2BIG;
            break;
        }
    }
    close(fd);
    if (read_error != 0 || memchr(text, '\0', used) != NULL) {
        free(text);
        return read_error != 0 ? read_error : -EINVAL;
    }
    text[used] = '\0';
    char *lines = NULL;
    int rc = 0;
    for (char *line = strtok_r(text, "\n", &lines); line != NULL; line = strtok_r(NULL, "\n", &lines)) {
        if (strlen(line) > 512) {
            rc = -EINVAL;
            break;
        }
        char *comment = strchr(line, '#');
        if (comment != NULL)
            *comment = '\0';
        char *arguments[5], *save = NULL;
        unsigned int count = 0;
        char *word = strtok_r(line, " \t\r\n", &save);
        while (word != NULL && count < 5) {
            arguments[count++] = word;
            word = strtok_r(NULL, " \t\r\n", &save);
        }
        if (count == 0)
            continue;
        if (count != 4 || word != NULL) {
            rc = -EINVAL;
            break;
        }
        if (strcmp(arguments[0], "route") == 0 && configuration->route_count < DPPD_IPV4_ROUTE_LIMIT) {
            rc = route_entry(arguments + 1, &configuration->routes[configuration->route_count++]);
        } else if (strcmp(arguments[0], "neighbor") == 0 && configuration->neighbor_count < DPPD_IPV4_NEIGHBOR_LIMIT) {
            rc = neighbor_entry(arguments + 1, &configuration->neighbors[configuration->neighbor_count++]);
        } else if (strcmp(arguments[0], "route6") == 0 && configuration->route6_count < DPPD_IPV4_ROUTE_LIMIT) {
            rc = route6_entry(arguments + 1, &configuration->routes6[configuration->route6_count++]);
        } else if (strcmp(arguments[0], "neighbor6") == 0 &&
            configuration->neighbor6_count < DPPD_IPV4_NEIGHBOR_LIMIT) {
            rc = neighbor6_entry(arguments + 1, &configuration->neighbors6[configuration->neighbor6_count++]);
        } else
            rc = -EINVAL;
        if (rc != 0)
            break;
    }
    free(text);
    return rc;
}

int dppctl_routing_build(int argc, char **argv, struct dppd_management_request *request,
    void (*initialize)(struct dppd_management_request *, enum dppd_management_operation))
{
    if (argc < 1)
        return -ENOSYS;
    enum dppd_management_operation operation;
    if (strcmp(argv[0], "routing-status") == 0)
        operation = DPPD_MANAGEMENT_ROUTING_STATUS;
    else if (strcmp(argv[0], "routing-show") == 0)
        operation = DPPD_MANAGEMENT_ROUTING_GET;
    else if (strcmp(argv[0], "routing-flush") == 0)
        operation = DPPD_MANAGEMENT_ROUTING_FLUSH;
    else if (strcmp(argv[0], "routing-replace") == 0)
        operation = DPPD_MANAGEMENT_ROUTING_REPLACE;
    else if (strcmp(argv[0], "route-set") == 0)
        operation = DPPD_MANAGEMENT_ROUTE_SET;
    else if (strcmp(argv[0], "route-delete") == 0)
        operation = DPPD_MANAGEMENT_ROUTE_DELETE;
    else if (strcmp(argv[0], "neighbor-set") == 0)
        operation = DPPD_MANAGEMENT_NEIGHBOR_SET;
    else if (strcmp(argv[0], "neighbor-delete") == 0)
        operation = DPPD_MANAGEMENT_NEIGHBOR_DELETE;
    else if (strcmp(argv[0], "route6-set") == 0)
        operation = DPPD_MANAGEMENT_ROUTE6_SET;
    else if (strcmp(argv[0], "route6-delete") == 0)
        operation = DPPD_MANAGEMENT_ROUTE6_DELETE;
    else if (strcmp(argv[0], "neighbor6-set") == 0)
        operation = DPPD_MANAGEMENT_NEIGHBOR6_SET;
    else if (strcmp(argv[0], "neighbor6-delete") == 0)
        operation = DPPD_MANAGEMENT_NEIGHBOR6_DELETE;
    else
        return -ENOSYS;
    initialize(request, operation);
    if (operation == DPPD_MANAGEMENT_ROUTING_STATUS || operation == DPPD_MANAGEMENT_ROUTING_GET ||
        operation == DPPD_MANAGEMENT_ROUTING_FLUSH)
        return argc == 1 ? 0 : -EINVAL;
    uint64_t generation;
    if (argc < 2 || integer(argv[argc - 1], UINT64_MAX, &generation) != 0 || generation == 0)
        return -EINVAL;
    if (operation == DPPD_MANAGEMENT_ROUTING_REPLACE) {
        if (argc != 3)
            return -EINVAL;
        request->payload.routing_replace.expected_generation = generation;
        return read_configuration(argv[1], &request->payload.routing_replace.configuration);
    }
    if (operation == DPPD_MANAGEMENT_ROUTE6_SET || operation == DPPD_MANAGEMENT_ROUTE6_DELETE) {
        request->payload.route6.expected_generation = generation;
        return operation == DPPD_MANAGEMENT_ROUTE6_SET
            ? (argc == 5 ? route6_entry(argv + 1, &request->payload.route6.entry) : -EINVAL)
            : (argc == 3 ? prefix6(argv[1], &request->payload.route6.entry) : -EINVAL);
    }
    if (operation == DPPD_MANAGEMENT_NEIGHBOR6_SET || operation == DPPD_MANAGEMENT_NEIGHBOR6_DELETE) {
        request->payload.neighbor6.expected_generation = generation;
        return operation == DPPD_MANAGEMENT_NEIGHBOR6_SET
            ? (argc == 5 ? neighbor6_entry(argv + 1, &request->payload.neighbor6.entry) : -EINVAL)
            : (argc == 4 ? neighbor6_key(argv + 1, &request->payload.neighbor6.entry) : -EINVAL);
    }
    if (operation == DPPD_MANAGEMENT_ROUTE_SET || operation == DPPD_MANAGEMENT_ROUTE_DELETE) {
        request->payload.route.expected_generation = generation;
        if (operation == DPPD_MANAGEMENT_ROUTE_SET)
            return argc == 5 ? route_entry(argv + 1, &request->payload.route.entry) : -EINVAL;
        return argc == 3 ? prefix(argv[1], &request->payload.route.entry) : -EINVAL;
    }
    request->payload.neighbor.expected_generation = generation;
    if (operation == DPPD_MANAGEMENT_NEIGHBOR_SET)
        return argc == 5 ? neighbor_entry(argv + 1, &request->payload.neighbor.entry) : -EINVAL;
    return argc == 4 ? neighbor_key(argv + 1, &request->payload.neighbor.entry) : -EINVAL;
}

bool dppctl_routing_print(const struct dppd_management_response *response)
{
    if ((response->operation < DPPD_MANAGEMENT_ROUTING_STATUS || response->operation > DPPD_MANAGEMENT_ROUTING_FLUSH) &&
        (response->operation < DPPD_MANAGEMENT_ROUTE6_SET || response->operation > DPPD_MANAGEMENT_NEIGHBOR6_DELETE))
        return false;
    if (response->operation != DPPD_MANAGEMENT_ROUTING_GET) {
        const struct dppd_routing_status *status = &response->payload.routing_status;
        printf("routing enabled=%s dirty=%s generation=%" PRIu64 " persisted-generation=%" PRIu64
               " routes=%u neighbors=%u routes6=%u neighbors6=%u last-error=%d\n",
            status->enabled ? "yes" : "no", status->dirty ? "yes" : "no", status->generation,
            status->persisted_generation, status->route_count, status->neighbor_count, status->route6_count,
            status->neighbor6_count, status->last_error);
        return true;
    }
    const struct dppd_ipv4_configuration *configuration = &response->payload.routing;
    printf("routing generation=%" PRIu64 " routes=%u neighbors=%u routes6=%u neighbors6=%u\n",
        configuration->generation, configuration->route_count, configuration->neighbor_count,
        configuration->route6_count, configuration->neighbor6_count);
    char address[INET_ADDRSTRLEN], next_hop[INET_ADDRSTRLEN];
    for (uint32_t i = 0; i < configuration->route_count; ++i) {
        const struct dppd_ipv4_route *route = &configuration->routes[i];
        inet_ntop(AF_INET, &route->prefix_be, address, sizeof(address));
        inet_ntop(AF_INET, &route->gateway_be, next_hop, sizeof(next_hop));
        printf("route prefix=%s/%u port=%u next-hop=%s\n", address, route->depth, route->port_id,
            route->gateway_be != 0 ? next_hop : "direct");
    }
    for (uint32_t i = 0; i < configuration->neighbor_count; ++i) {
        const struct dppd_ipv4_neighbor *neighbor = &configuration->neighbors[i];
        inet_ntop(AF_INET, &neighbor->address_be, address, sizeof(address));
        printf("neighbor port=%u address=%s mac=%02x:%02x:%02x:%02x:%02x:%02x\n", neighbor->port_id, address,
            neighbor->mac[0], neighbor->mac[1], neighbor->mac[2], neighbor->mac[3], neighbor->mac[4], neighbor->mac[5]);
    }
    const uint8_t zero[16] = {0};
    char address6[INET6_ADDRSTRLEN], next6[INET6_ADDRSTRLEN];
    for (uint32_t i = 0; i < configuration->route6_count; ++i) {
        const struct dppd_ipv6_route *route = &configuration->routes6[i];
        inet_ntop(AF_INET6, route->prefix, address6, sizeof(address6));
        inet_ntop(AF_INET6, route->gateway, next6, sizeof(next6));
        printf("route6 prefix=%s/%u port=%u next-hop=%s\n", address6, route->depth, route->port_id,
            memcmp(route->gateway, zero, 16) != 0 ? next6 : "direct");
    }
    for (uint32_t i = 0; i < configuration->neighbor6_count; ++i) {
        const struct dppd_ipv6_neighbor *neighbor = &configuration->neighbors6[i];
        inet_ntop(AF_INET6, neighbor->address, address6, sizeof(address6));
        printf("neighbor6 port=%u address=%s mac=%02x:%02x:%02x:%02x:%02x:%02x\n", neighbor->port_id, address6,
            neighbor->mac[0], neighbor->mac[1], neighbor->mac[2], neighbor->mac[3], neighbor->mac[4], neighbor->mac[5]);
    }
    return true;
}

void dppctl_routing_usage(const char *program)
{
    fprintf(stderr, "  %s [--socket PATH] routing-status|routing-show|routing-flush\n", program);
    fprintf(stderr, "  %s [--socket PATH] route-set PREFIX PORT NEXT_HOP|direct EXPECTED_GENERATION\n", program);
    fprintf(stderr, "  %s [--socket PATH] route-delete PREFIX EXPECTED_GENERATION\n", program);
    fprintf(stderr, "  %s [--socket PATH] neighbor-set PORT IP MAC EXPECTED_GENERATION\n", program);
    fprintf(stderr, "  %s [--socket PATH] neighbor-delete PORT IP EXPECTED_GENERATION\n", program);
    fprintf(stderr, "  %s [--socket PATH] routing-replace FILE EXPECTED_GENERATION\n", program);
    fprintf(stderr, "  IPv6 uses route6-set/delete and neighbor6-set/delete with the same arguments\n");
}
