#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dppd/routing.h"

static int failure;
static bool short_write;
ssize_t __real_pwrite64(int fd, const void *bytes, size_t size, off_t offset);
int __real_fsync(int fd);
int __real_rename(const char *from, const char *to);
int __real_renameat2(int from_fd, const char *from, int to_fd, const char *to, unsigned int flags);

ssize_t __wrap_pwrite64(int fd, const void *bytes, size_t size, off_t offset)
{
    if (failure == 3 && offset != 0) {
        errno = ENOSPC;
        return -1;
    }
    if ((failure == 3 || short_write) && size > 17)
        size = 17;
    return __real_pwrite64(fd, bytes, size, offset);
}

int __wrap_fsync(int fd)
{
    struct stat info;
    assert(fstat(fd, &info) == 0);
    if ((failure == 1 && S_ISREG(info.st_mode)) || (failure == 2 && S_ISDIR(info.st_mode))) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

int __wrap_rename(const char *from, const char *to)
{
    if (failure == 4) {
        errno = EIO;
        return -1;
    }
    return __real_rename(from, to);
}

int __wrap_renameat2(int from_fd, const char *from, int to_fd, const char *to, unsigned int flags)
{
    if (failure == 5) {
        errno = EIO;
        return -1;
    }
    return __real_renameat2(from_fd, from, to_fd, to, flags);
}

static const struct dppd_routing_binding bindings[] = {{.port_id = 3, .device = "test-3", .driver = "net_ring"},
    {.port_id = 7, .device = "test-7", .driver = "net_tap", .ifname = "testtap7"}};

static void configuration(struct dppd_ipv4_configuration *value)
{
    memset(value, 0, sizeof(*value));
    value->route_count = value->neighbor_count = 1;
    value->routes[0].port_id = value->neighbors[0].port_id = 7;
    assert(inet_pton(AF_INET, "192.0.2.1", &value->routes[0].gateway_be) == 1);
    value->neighbors[0].address_be = value->routes[0].gateway_be;
    value->neighbors[0].mac[0] = 2;
    value->neighbors[0].mac[5] = 7;
}

static uint64_t disk_generation(const char *path)
{
    uint8_t bytes[8];
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    assert(fd >= 0 && pread(fd, bytes, sizeof(bytes), 16) == sizeof(bytes));
    close(fd);
    uint64_t value = 0;
    for (unsigned int i = 0; i < 8; ++i)
        value |= (uint64_t)bytes[i] << (i * 8);
    return value;
}

/** 保存成功再返回新版本，关闭后从磁盘恢复相同版本、路由和邻居，另一个实例始终拿不到锁 */
static void test_restart(const char *directory)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/restart", directory);
    struct dppd_routing_service *service = NULL, *second = NULL;
    short_write = true;
    assert(dppd_routing_open(&service, path, bindings, 2) == 0);
    short_write = false;
    assert(dppd_routing_open(&second, path, bindings, 2) == -EBUSY && second == NULL);
    struct dppd_ipv4_configuration value, restored;
    configuration(&value);
    assert(dppd_routing_replace(service, 1, &value) == 0 && disk_generation(path) == 2);
    assert(dppd_routing_get(service, &value) == 0 && value.generation == 2);
    assert(dppd_routing_replace(service, 1, &value) == -ESTALE);
    assert(dppd_routing_close(service) == 0);
    service = NULL;
    assert(dppd_routing_open(&service, path, bindings, 2) == 0);
    assert(dppd_routing_get(service, &restored) == 0 && memcmp(&value, &restored, sizeof(value)) == 0);
    struct dppd_routing_status status;
    dppd_routing_status(service, &status);
    assert(status.enabled && !status.dirty && status.generation == 2 && status.persisted_generation == 2);
    assert(status.route_count == 1 && status.neighbor_count == 1);
    assert(dppd_routing_close(service) == 0);
    struct dppd_routing_binding changed[2];
    memcpy(changed, bindings, sizeof(changed));
    changed[1].device[0] = 'X';
    service = NULL;
    assert(dppd_routing_open(&service, path, changed, 2) == -EXDEV && service == NULL);
    memcpy(changed, bindings, sizeof(changed));
    strcpy(changed[1].ifname, "different");
    assert(dppd_routing_open(&service, path, changed, 2) == -EXDEV);
}

/** 目录同步失败时磁盘可能已替换，但活跃表保持旧版，后续写入必须先显式修复 */
static void test_failures(const char *directory)
{
    for (int mode = 1; mode <= 4; ++mode) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/fault-%d", directory, mode);
        struct dppd_routing_service *service = NULL;
        assert(dppd_routing_open(&service, path, bindings, 2) == 0);
        struct dppd_ipv4_configuration value, active;
        configuration(&value);
        failure = mode;
        assert(dppd_routing_replace(service, 1, &value) == -EUCLEAN);
        failure = 0;
        struct dppd_routing_status status;
        dppd_routing_status(service, &status);
        assert(status.dirty && status.generation == 1 && status.last_error < 0);
        assert(dppd_routing_get(service, &active) == 0 && active.route_count == 0);
        assert(disk_generation(path) == (mode == 2 ? 2U : 1U));
        assert(dppd_routing_replace(service, 1, &value) == -EUCLEAN);
        assert(dppd_routing_flush(service) == 0 && disk_generation(path) == 1);
        dppd_routing_status(service, &status);
        assert(!status.dirty && status.last_error == 0);
        assert(dppd_routing_replace(service, 1, &value) == 0);
        assert(dppd_routing_close(service) == 0);
        service = NULL;
        assert(dppd_routing_open(&service, path, bindings, 2) == 0);
        assert(dppd_routing_get(service, &active) == 0 && active.generation == 2 && active.route_count == 1);
        assert(dppd_routing_close(service) == 0);
    }
    char path[4096];
    snprintf(path, sizeof(path), "%s/first-rename-fault", directory);
    struct dppd_routing_service *service = NULL;
    failure = 5;
    assert(dppd_routing_open(&service, path, bindings, 2) == -EIO && service == NULL);
    failure = 0;
    assert(access(path, F_OK) != 0);
    assert(dppd_routing_open(&service, path, bindings, 2) == 0);
    assert(dppd_routing_close(service) == 0);
}

static void test_tampering(const char *directory)
{
    for (unsigned int mode = 0; mode < 6; ++mode) {
        char path[4096], backup[4096];
        snprintf(path, sizeof(path), "%s/tamper-%u", directory, mode);
        snprintf(backup, sizeof(backup), "%s/backup-%u", directory, mode);
        struct dppd_routing_service *service = NULL;
        assert(dppd_routing_open(&service, path, bindings, 2) == 0);
        if (mode == 0 || mode == 1) {
            int fd = open(path, O_WRONLY);
            uint8_t byte = 1;
            assert(fd >= 0 && __real_pwrite64(fd, &byte, 1, mode == 0 ? 40 : DPPD_ROUTING_FILE_SIZE) == 1);
            close(fd);
        } else if (mode == 2)
            assert(chmod(path, 0644) == 0);
        else if (mode == 3)
            assert(link(path, backup) == 0);
        else {
            assert(__real_rename(path, backup) == 0);
            if (mode == 4)
                assert(symlink(backup, path) == 0);
            else {
                int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
                assert(fd >= 0 && ftruncate(fd, DPPD_ROUTING_FILE_SIZE) == 0);
                close(fd);
            }
        }
        struct dppd_ipv4_configuration value;
        configuration(&value);
        assert(dppd_routing_replace(service, 1, &value) == -EUCLEAN);
        assert(dppd_routing_flush(service) < 0);
        assert(dppd_routing_close(service) == 0);
        service = NULL;
        assert(dppd_routing_open(&service, path, bindings, 2) < 0 && service == NULL);
    }
}

static void cleanup(const char *directory)
{
    DIR *stream = opendir(directory);
    assert(stream != NULL);
    struct dirent *entry;
    while ((entry = readdir(stream)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        assert(unlink(path) == 0);
    }
    closedir(stream);
    assert(rmdir(directory) == 0);
}

/** 相对路径和点路径应指向同一目标，启动检查不得先创建文件再发现与规则存储冲突 */
static void test_paths(const char *directory)
{
    char path[4096], alias[4096], lock[4096], other[4096];
    snprintf(path, sizeof(path), "%s/routes", directory);
    snprintf(alias, sizeof(alias), "%s/./routes", directory);
    snprintf(lock, sizeof(lock), "%s/routes.lock", directory);
    snprintf(other, sizeof(other), "%s/rules", directory);
    const char *paths[] = {alias, NULL};
    assert(dppd_routing_check_paths(path, paths, 2) == -EINVAL);
    paths[0] = lock;
    assert(dppd_routing_check_paths(path, paths, 2) == -EINVAL);
    paths[0] = other;
    assert(dppd_routing_check_paths(path, paths, 2) == 0);
    assert(access(path, F_OK) != 0 && access(lock, F_OK) != 0);
    assert(dppd_routing_check_paths(NULL, paths, 2) == 0);
    assert(dppd_routing_check_paths(path, NULL, 1) == -EINVAL);
}

int main(void)
{
    char directory[] = "/tmp/dppd-routing-store-XXXXXX";
    assert(mkdtemp(directory) != NULL);
    test_paths(directory);
    test_restart(directory);
    test_failures(directory);
    test_tampering(directory);
    cleanup(directory);
    puts("routing persistence, binding, failure and restart checks passed");
    return 0;
}
