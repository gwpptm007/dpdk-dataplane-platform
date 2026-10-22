#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "flow_state_fixture.h"

static unsigned int write_fault, sync_fault, rename_fault;
ssize_t __real_write(int fd, const void *data, size_t size);
ssize_t __wrap_write(int fd, const void *data, size_t size);
int __real_fsync(int fd);
int __wrap_fsync(int fd);
int __real_rename(const char *from, const char *to);
int __wrap_rename(const char *from, const char *to);

ssize_t __wrap_write(int fd, const void *data, size_t size)
{
    unsigned int fault = write_fault;
    write_fault = 0;
    if (fault == 1) {
        errno = EIO;
        return -1;
    }
    if (fault == 2)
        return 0;
    if (fault == 3) {
        errno = EINTR;
        return -1;
    }
    return __real_write(fd, data, fault == 4 && size > 17 ? 17 : size);
}

int __wrap_fsync(int fd)
{
    if (sync_fault != 0 && --sync_fault == 0) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

int __wrap_rename(const char *from, const char *to)
{
    if (rename_fault != 0) {
        rename_fault = 0;
        errno = EIO;
        return -1;
    }
    return __real_rename(from, to);
}

static uint8_t *read_file(const char *path, size_t *size)
{
    struct stat st;
    int fd = open(path, O_RDONLY);
    assert(fd >= 0 && fstat(fd, &st) == 0);
    *size = (size_t)st.st_size;
    uint8_t *bytes = malloc(*size);
    assert(bytes != NULL && read(fd, bytes, *size) == (ssize_t)*size);
    close(fd);
    return bytes;
}

static void write_file(const char *path, const uint8_t *data, size_t size)
{
    int fd = open(path, O_WRONLY | O_TRUNC);
    assert(fd >= 0 && write(fd, data, size) == (ssize_t)size);
    close(fd);
}

static void repair_crc(uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    memset(data + 24, 0, 8);
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned int b = 0; b < 8; ++b)
            crc = (crc & 1U) != 0 ? (crc >> 1) ^ 0xedb88320U : crc >> 1;
    }
    crc ^= UINT32_MAX;
    for (unsigned int b = 0; b < 4; ++b)
        data[24 + b] = (uint8_t)(crc >> (8U * b));
}

int main(void)
{
    char directory[] = "/tmp/dppd-flow-state-XXXXXX", path[256], lock[270], link_path[270];
    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/state", directory);
    snprintf(lock, sizeof(lock), "%s.lock", path);
    snprintf(link_path, sizeof(link_path), "%s.alias", path);
    struct dppd_flow_state *state = flow_state_fixture(129), *loaded = NULL;
    assert(dppd_flow_state_validate(state) == 0 && dppd_flow_state_save(path, state) == 0);
    struct stat st;
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    assert(dppd_flow_state_load(path, &loaded) == 0);
    assert(loaded->rule_count == 129 && loaded->revision == 1381 && loaded->shared_next_generation == 50);
    assert(loaded->tables[0].spec.example.id == 0 && loaded->tables[0].spec.example.generation == 0);
    for (uint32_t i = 0; i < state->rule_count; ++i) {
        assert(dppd_rule_equal(&loaded->rules[i].rule, &state->rules[i].rule));
        assert(loaded->rules[i].rule.generation == state->rules[i].rule.generation && loaded->rules[i].queue == i % 2U);
    }
    dppd_flow_state_free(&loaded);
    /** rename 前的故障保留旧版本，目录同步失败则新版本可见但仍返回失败 */
    for (unsigned int fault = 0; fault < 5; ++fault) {
        state->revision = 2000 + fault;
        if (fault < 2)
            write_fault = fault + 1;
        if (fault == 2)
            sync_fault = 1;
        if (fault == 3)
            rename_fault = 1;
        if (fault == 4)
            sync_fault = 2;
        assert(dppd_flow_state_save(path, state) == -EIO);
        assert(dppd_flow_state_load(path, &loaded) == 0);
        assert(loaded->revision == (fault == 4 ? 2004 : 1381));
        dppd_flow_state_free(&loaded);
    }
    for (unsigned int fault = 3; fault <= 4; ++fault) {
        write_fault = fault;
        assert(dppd_flow_state_save(path, state) == 0);
    }
    size_t size;
    uint8_t *original = read_file(path, &size), *mutated = malloc(size);
    assert(mutated != NULL);
    for (unsigned int mutation = 0; mutation < 4; ++mutation) {
        memcpy(mutated, original, size);
        if (mutation == 0)
            mutated[size - 1U] ^= 1;
        if (mutation == 1) {
            mutated[39] = 0x80;
            repair_crc(mutated, size);
        }
        if (mutation == 2)
            mutated[8] = 3;
        write_file(path, mutated, mutation == 3 ? size - 1U : size);
        assert(dppd_flow_state_load(path, &loaded) == (mutation == 2 ? -EPROTONOSUPPORT : -EBADMSG));
        assert(loaded == NULL);
    }
    write_file(path, original, size);
    assert(link(path, link_path) == 0 && dppd_flow_state_load(path, &loaded) == -EBADMSG);
    unlink(link_path);
    assert(symlink(path, link_path) == 0 && dppd_flow_state_load(link_path, &loaded) == -ELOOP);
    unlink(link_path);
    uint64_t saved = state->rules[0].rule.generation;
    state->rules[0].rule.generation = state->revision + 1U;
    assert(dppd_flow_state_validate(state) == -EINVAL);
    state->rules[0].rule.generation = saved;
    state->rules[0].table_key = 999;
    assert(dppd_flow_state_validate(state) == -EINVAL);
    state->rules[0].table_key = 100;
    state->config.admission.limits[DPPD_RESOURCE_RULE] = 128;
    assert(dppd_flow_state_validate(state) == -ENOSPC);
    dppd_flow_state_free(&state);
    free(original);
    free(mutated);
    unlink(path);
    unlink(lock);
    assert(rmdir(directory) == 0);
    return 0;
}
