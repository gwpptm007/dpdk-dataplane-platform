#define _GNU_SOURCE
#include "dppd/routing.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define HEADER_SIZE 64U
#define BINDING_SIZE 216U
#define ROUTE_OFFSET (HEADER_SIZE + DPPD_MAX_PORTS * BINDING_SIZE)
#define NEIGHBOR_OFFSET (ROUTE_OFFSET + 16U * DPPD_IPV4_ROUTE_LIMIT)

struct dppd_routing_service {
    struct dppd_ipv4_router *router;
    int lock_fd, snapshot_fd, directory_fd, last_error;
    bool dirty;
    uint16_t count;
    uint64_t persisted_generation;
    char path[DPPD_STATE_PATH_CAPACITY], lock_path[DPPD_STATE_PATH_CAPACITY], parent[DPPD_STATE_PATH_CAPACITY];
    struct dppd_routing_binding bindings[DPPD_MAX_PORTS];
    uint8_t expected[DPPD_ROUTING_FILE_SIZE];
};

static void put(uint8_t *target, uint64_t value, unsigned int width)
{
    for (unsigned int i = 0; i < width; ++i)
        target[i] = (uint8_t)(value >> (8U * i));
}

static uint64_t get(const uint8_t *bytes, unsigned int width)
{
    uint64_t value = 0;
    for (unsigned int i = 0; i < width; ++i)
        value |= (uint64_t)bytes[i] << (8U * i);
    return value;
}

static uint32_t checksum(const uint8_t *bytes)
{
    uint32_t value = UINT32_MAX;
    for (size_t i = 0; i < DPPD_ROUTING_FILE_SIZE; ++i) {
        value ^= i >= 32 && i < 36 ? 0 : bytes[i];
        for (unsigned int bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ (0xedb88320U & (0U - (value & 1U)));
    }
    return ~value;
}

/** 固定头部、端口映射、路由和邻居分别编码，IP 字节直接复制，其他整数明确用小端 */
static void encode(const struct dppd_routing_service *service, const struct dppd_ipv4_configuration *configuration,
    uint8_t bytes[DPPD_ROUTING_FILE_SIZE])
{
    memset(bytes, 0, DPPD_ROUTING_FILE_SIZE);
    memcpy(bytes, "DPPIPV41", 8);
    put(bytes + 8, 1, 4);
    put(bytes + 12, DPPD_ROUTING_FILE_SIZE, 4);
    put(bytes + 16, configuration->generation, 8);
    put(bytes + 24, configuration->route_count, 4);
    put(bytes + 28, configuration->neighbor_count, 4);
    put(bytes + 36, service->count, 4);
    for (uint16_t i = 0; i < service->count; ++i) {
        uint8_t *entry = bytes + HEADER_SIZE + i * BINDING_SIZE;
        const struct dppd_routing_binding *binding = &service->bindings[i];
        put(entry, binding->port_id, 2);
        memcpy(entry + 8, binding->device, strlen(binding->device));
        memcpy(entry + 136, binding->driver, strlen(binding->driver));
        memcpy(entry + 200, binding->ifname, strlen(binding->ifname));
    }
    for (uint32_t i = 0; i < configuration->route_count; ++i) {
        uint8_t *entry = bytes + ROUTE_OFFSET + 16U * i;
        const struct dppd_ipv4_route *route = &configuration->routes[i];
        memcpy(entry, &route->prefix_be, 4);
        memcpy(entry + 4, &route->gateway_be, 4);
        put(entry + 8, route->port_id, 2);
        entry[10] = route->depth;
    }
    for (uint32_t i = 0; i < configuration->neighbor_count; ++i) {
        uint8_t *entry = bytes + NEIGHBOR_OFFSET + 16U * i;
        const struct dppd_ipv4_neighbor *neighbor = &configuration->neighbors[i];
        memcpy(entry, &neighbor->address_be, 4);
        put(entry + 4, neighbor->port_id, 2);
        memcpy(entry + 6, neighbor->mac, 6);
    }
    put(bytes + 32, checksum(bytes), 4);
}

static int decode(struct dppd_routing_service *service, const uint8_t *bytes,
    struct dppd_ipv4_configuration *configuration)
{
    if (memcmp(bytes, "DPPIPV41", 8) != 0 || get(bytes + 8, 4) != 1 || get(bytes + 12, 4) != DPPD_ROUTING_FILE_SIZE ||
        get(bytes + 32, 4) != checksum(bytes) || get(bytes + 24, 4) > DPPD_IPV4_ROUTE_LIMIT ||
        get(bytes + 28, 4) > DPPD_IPV4_NEIGHBOR_LIMIT)
        return -EBADMSG;
    memset(configuration, 0, sizeof(*configuration));
    configuration->generation = get(bytes + 16, 8);
    configuration->route_count = (uint32_t)get(bytes + 24, 4);
    configuration->neighbor_count = (uint32_t)get(bytes + 28, 4);
    for (uint32_t i = 0; i < configuration->route_count; ++i) {
        const uint8_t *entry = bytes + ROUTE_OFFSET + 16U * i;
        struct dppd_ipv4_route *route = &configuration->routes[i];
        memcpy(&route->prefix_be, entry, 4);
        memcpy(&route->gateway_be, entry + 4, 4);
        route->port_id = (uint16_t)get(entry + 8, 2);
        route->depth = entry[10];
    }
    for (uint32_t i = 0; i < configuration->neighbor_count; ++i) {
        const uint8_t *entry = bytes + NEIGHBOR_OFFSET + 16U * i;
        struct dppd_ipv4_neighbor *neighbor = &configuration->neighbors[i];
        memcpy(&neighbor->address_be, entry, 4);
        neighbor->port_id = (uint16_t)get(entry + 4, 2);
        memcpy(neighbor->mac, entry + 6, 6);
    }
    uint8_t canonical[DPPD_ROUTING_FILE_SIZE];
    encode(service, configuration, canonical);
    /* 完整比较同时约束端口身份、保留位、字符串尾部和未使用槽位 */
    return memcmp(bytes, canonical, sizeof(canonical)) == 0 ? 0 : -EXDEV;
}

static int file_identity(int fd, const char *path, off_t size)
{
    struct stat held, named;
    if (fstat(fd, &held) != 0 || lstat(path, &named) != 0)
        return -errno;
    if (!S_ISREG(named.st_mode) || held.st_dev != named.st_dev || held.st_ino != named.st_ino || held.st_nlink != 1 ||
        held.st_uid != geteuid() || (held.st_mode & 07777) != 0600 || held.st_size != size)
        return -ESTALE;
    return 0;
}

static int read_exact(int fd, uint8_t *bytes, size_t size)
{
    size_t done = 0;
    while (done < size) {
        ssize_t read_size = pread(fd, bytes + done, size - done, (off_t)done);
        if (read_size < 0 && errno == EINTR)
            continue;
        if (read_size <= 0)
            return read_size < 0 ? -errno : -EBADMSG;
        done += (size_t)read_size;
    }
    return 0;
}

/** 写入前再次核对同一目录、锁文件及上次完整快照，不能盲目覆盖被替换或损坏的文件 */
static int unchanged(struct dppd_routing_service *service)
{
    struct stat held, named;
    if (fstat(service->directory_fd, &held) != 0 || lstat(service->parent, &named) != 0)
        return -errno;
    if (!S_ISDIR(named.st_mode) || held.st_dev != named.st_dev || held.st_ino != named.st_ino)
        return -ESTALE;
    int rc = file_identity(service->lock_fd, service->lock_path, 0);
    if (rc == 0 && service->snapshot_fd >= 0) {
        rc = file_identity(service->snapshot_fd, service->path, DPPD_ROUTING_FILE_SIZE);
        uint8_t bytes[DPPD_ROUTING_FILE_SIZE];
        if (rc == 0)
            rc = read_exact(service->snapshot_fd, bytes, sizeof(bytes));
        if (rc == 0 && memcmp(bytes, service->expected, sizeof(bytes)) != 0)
            rc = -EBADMSG;
    }
    return rc;
}

/** 文件同步、原子替换和目录同步均完成才成功，任何错误都禁止随后直接发布候选 */
static int save(void *context, const struct dppd_ipv4_configuration *configuration)
{
    struct dppd_routing_service *service = context;
    char temporary[DPPD_STATE_PATH_CAPACITY];
    int fd = -1, rc = unchanged(service);
    if (rc != 0)
        goto fail;
    if (snprintf(temporary, sizeof(temporary), "%s.XXXXXX", service->path) >= (int)sizeof(temporary)) {
        rc = -ENAMETOOLONG;
        goto fail;
    }
    fd = mkostemp(temporary, O_CLOEXEC);
    if (fd < 0) {
        rc = -errno;
        goto fail;
    }
    uint8_t bytes[DPPD_ROUTING_FILE_SIZE];
    encode(service, configuration, bytes);
    size_t done = 0;
    while (done < sizeof(bytes)) {
        ssize_t written = pwrite(fd, bytes + done, sizeof(bytes) - done, (off_t)done);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            rc = written < 0 ? -errno : -EIO;
            goto fail;
        }
        done += (size_t)written;
    }
    if (fsync(fd) != 0) {
        rc = -errno;
        goto fail;
    }
    rc = unchanged(service);
    if (rc != 0)
        goto fail;
    int renamed = service->snapshot_fd < 0 ? renameat2(AT_FDCWD, temporary, AT_FDCWD, service->path, RENAME_NOREPLACE)
                                           : rename(temporary, service->path);
    if (renamed != 0) {
        rc = -errno;
        goto fail;
    }
    if (service->snapshot_fd >= 0)
        close(service->snapshot_fd);
    service->snapshot_fd = fd;
    memcpy(service->expected, bytes, sizeof(bytes));
    fd = -1;
    if (fsync(service->directory_fd) != 0) {
        rc = -errno;
        goto fail;
    }
    service->persisted_generation = configuration->generation;
    service->last_error = 0;
    service->dirty = false;
    return 0;
fail:
    if (fd >= 0) {
        close(fd);
        unlink(temporary);
    }
    service->last_error = rc;
    service->dirty = true;
    return rc;
}

static int compare_binding(const void *left, const void *right)
{
    const struct dppd_routing_binding *a = left, *b = right;
    return (int)a->port_id - b->port_id;
}

static int canonical_target(const char *path, char target[DPPD_STATE_PATH_CAPACITY])
{
    char parent[DPPD_STATE_PATH_CAPACITY], name[DPPD_STATE_PATH_CAPACITY], resolved[DPPD_STATE_PATH_CAPACITY];
    if (path == NULL || path[0] == '\0' || strlen(path) >= sizeof(parent))
        return -EINVAL;
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    const char *base = slash != NULL ? slash + 1 : path;
    if (*base == '\0' || strcmp(base, ".") == 0 || strcmp(base, "..") == 0)
        return -EINVAL;
    strcpy(name, base);
    if (slash != NULL)
        slash[slash == parent ? 1 : 0] = '\0';
    else
        strcpy(parent, ".");
    if (realpath(parent, resolved) == NULL)
        return -errno;
    return snprintf(target, DPPD_STATE_PATH_CAPACITY, "%s%s%s", resolved, strcmp(resolved, "/") == 0 ? "" : "/",
               name) >= (int)DPPD_STATE_PATH_CAPACITY
        ? -ENAMETOOLONG
        : 0;
}

int dppd_routing_check_paths(const char *path, const char *const *others, size_t count)
{
    if (path == NULL || *path == '\0')
        return 0;
    if (count != 0 && others == NULL)
        return -EINVAL;
    char canonical[DPPD_STATE_PATH_CAPACITY], lock[DPPD_STATE_PATH_CAPACITY], other[DPPD_STATE_PATH_CAPACITY];
    int rc = canonical_target(path, canonical);
    if (rc != 0)
        return rc;
    if (snprintf(lock, sizeof(lock), "%s.lock", canonical) >= (int)sizeof(lock))
        return -ENAMETOOLONG;
    for (size_t i = 0; i < count; ++i) {
        if (others[i] == NULL || *others[i] == '\0')
            continue;
        rc = canonical_target(others[i], other);
        if (rc != 0)
            return rc;
        if (strcmp(canonical, other) == 0 || strcmp(lock, other) == 0)
            return -EINVAL;
    }
    return 0;
}

static int prepare_paths(struct dppd_routing_service *service, const char *path)
{
    int rc = canonical_target(path, service->path);
    if (rc != 0)
        return rc;
    if (snprintf(service->lock_path, sizeof(service->lock_path), "%s.lock", service->path) >=
        (int)sizeof(service->lock_path))
        return -ENAMETOOLONG;
    strcpy(service->parent, service->path);
    char *slash = strrchr(service->parent, '/');
    slash[slash == service->parent ? 1 : 0] = '\0';
    service->directory_fd = open(service->parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    return service->directory_fd >= 0 ? 0 : -errno;
}

int dppd_routing_open(struct dppd_routing_service **result, const char *path,
    const struct dppd_routing_binding *bindings, uint16_t count)
{
    if (result == NULL || *result != NULL || bindings == NULL || count == 0 || count > DPPD_MAX_PORTS)
        return -EINVAL;
    struct dppd_routing_service *service = calloc(1, sizeof(*service));
    if (service == NULL)
        return -ENOMEM;
    service->lock_fd = service->snapshot_fd = service->directory_fd = -1;
    int rc = -EINVAL;
    service->count = count;
    uint16_t ports[DPPD_MAX_PORTS];
    for (uint16_t i = 0; i < count; ++i) {
        const struct dppd_routing_binding *binding = &bindings[i];
        if (binding->port_id == UINT16_MAX || binding->device[0] == '\0' || binding->driver[0] == '\0' ||
            memchr(binding->device, 0, sizeof(binding->device)) == NULL ||
            memchr(binding->driver, 0, sizeof(binding->driver)) == NULL ||
            memchr(binding->ifname, 0, sizeof(binding->ifname)) == NULL)
            goto fail;
        service->bindings[i].port_id = binding->port_id;
        strcpy(service->bindings[i].device, binding->device);
        strcpy(service->bindings[i].driver, binding->driver);
        strcpy(service->bindings[i].ifname, binding->ifname);
    }
    qsort(service->bindings, count, sizeof(service->bindings[0]), compare_binding);
    for (uint16_t i = 0; i < count; ++i) {
        if (i != 0 && service->bindings[i].port_id == service->bindings[i - 1].port_id)
            goto fail;
        ports[i] = service->bindings[i].port_id;
    }
    rc = prepare_paths(service, path);
    if (rc != 0)
        goto fail;
    service->lock_fd = open(service->lock_path, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (service->lock_fd < 0) {
        rc = -errno;
        goto fail;
    }
    rc = file_identity(service->lock_fd, service->lock_path, 0);
    if (rc != 0)
        goto fail;
    if (flock(service->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        rc = errno == EWOULDBLOCK ? -EBUSY : -errno;
        goto fail;
    }
    struct dppd_ipv4_configuration initial = {.generation = 1};
    service->snapshot_fd = open(service->path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (service->snapshot_fd >= 0) {
        rc = file_identity(service->snapshot_fd, service->path, DPPD_ROUTING_FILE_SIZE);
        if (rc == 0)
            rc = read_exact(service->snapshot_fd, service->expected, sizeof(service->expected));
        if (rc == 0)
            rc = decode(service, service->expected, &initial);
    } else if (errno != ENOENT)
        rc = -errno;
    if (rc == 0)
        rc = dppd_ipv4_router_create(&service->router, ports, count, &initial);
    if (rc != 0)
        goto fail;
    /* 恢复后再次编码规范顺序，连原来顺序不规范但 CRC 正确的文件也拒绝 */
    struct dppd_ipv4_configuration canonical;
    rc = dppd_ipv4_router_get(service->router, &canonical);
    if (rc == 0 && service->snapshot_fd >= 0) {
        uint8_t bytes[DPPD_ROUTING_FILE_SIZE];
        encode(service, &canonical, bytes);
        if (memcmp(bytes, service->expected, sizeof(bytes)) != 0)
            rc = -EBADMSG;
        if (rc == 0)
            rc = unchanged(service);
        if (rc == 0 && fsync(service->directory_fd) != 0)
            rc = -errno;
    } else if (rc == 0)
        rc = save(service, &canonical);
    if (rc != 0)
        goto fail;
    service->persisted_generation = canonical.generation;
    *result = service;
    return 0;
fail:
    dppd_routing_close(service);
    return rc;
}

int dppd_routing_close(struct dppd_routing_service *service)
{
    if (service == NULL)
        return 0;
    int rc = dppd_ipv4_router_destroy(service->router);
    if (rc != 0)
        return rc;
    if (service->snapshot_fd >= 0)
        close(service->snapshot_fd);
    if (service->lock_fd >= 0)
        close(service->lock_fd);
    if (service->directory_fd >= 0)
        close(service->directory_fd);
    free(service);
    return 0;
}

struct dppd_ipv4_router *dppd_routing_router(struct dppd_routing_service *service)
{
    return service != NULL ? service->router : NULL;
}

int dppd_routing_get(struct dppd_routing_service *service, struct dppd_ipv4_configuration *configuration)
{
    return service != NULL ? dppd_ipv4_router_get(service->router, configuration) : -ENODEV;
}

int dppd_routing_replace(struct dppd_routing_service *service, uint64_t expected,
    const struct dppd_ipv4_configuration *configuration)
{
    if (service == NULL)
        return -ENODEV;
    if (service->dirty)
        return -EUCLEAN;
    int rc = dppd_ipv4_router_replace(service->router, expected, configuration, save, service);
    return service->dirty ? -EUCLEAN : rc;
}

int dppd_routing_flush(struct dppd_routing_service *service)
{
    if (service == NULL)
        return -ENODEV;
    struct dppd_ipv4_configuration configuration;
    int rc = dppd_routing_get(service, &configuration);
    return rc == 0 ? save(service, &configuration) : rc;
}

void dppd_routing_status(struct dppd_routing_service *service, struct dppd_routing_status *status)
{
    if (status == NULL)
        return;
    memset(status, 0, sizeof(*status));
    if (service == NULL)
        return;
    struct dppd_ipv4_configuration configuration;
    if (dppd_routing_get(service, &configuration) != 0)
        return;
    status->enabled = true;
    status->dirty = service->dirty;
    status->last_error = service->last_error;
    status->generation = configuration.generation;
    status->persisted_generation = service->persisted_generation;
    status->route_count = configuration.route_count;
    status->neighbor_count = configuration.neighbor_count;
}
