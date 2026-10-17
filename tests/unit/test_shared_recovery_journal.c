#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "dppd/recovery_guard.h"

#define PORT_OFFSET (32U + DPPD_STATE_PATH_CAPACITY)
#define META_OFFSET (PORT_OFFSET + DPPD_MAX_PORTS * 320U)
#define ATTEMPT_OFFSET (META_OFFSET + 32U)
#define TYPED_STRIDE 208U

static unsigned int sync_calls, write_calls, fail_sync, fail_write;

int __real_fsync(int fd);
int __wrap_fsync(int fd);
ssize_t __real_pwrite(int fd, const void *buffer, size_t count, off_t offset);
ssize_t __wrap_pwrite(int fd, const void *buffer, size_t count, off_t offset);
ssize_t __real_pwrite64(int fd, const void *buffer, size_t count, off64_t offset);
ssize_t __wrap_pwrite64(int fd, const void *buffer, size_t count, off64_t offset);

/** 故障只影响指定的一次调用，其余仍真实写入私有文件，不能用内存仓库替代持久化验证 */
int __wrap_fsync(int fd)
{
    if (++sync_calls == fail_sync) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

ssize_t __wrap_pwrite(int fd, const void *buffer, size_t count, off_t offset)
{
    if (++write_calls == fail_write) {
        errno = EIO;
        return -1;
    }
    return __real_pwrite(fd, buffer, count, offset);
}

/** 正式构建启用大文件偏移时 libc 会改用 pwrite64，两种符号必须注入同一故障计划 */
ssize_t __wrap_pwrite64(int fd, const void *buffer, size_t count, off64_t offset)
{
    if (++write_calls == fail_write) {
        errno = EIO;
        return -1;
    }
    return __real_pwrite64(fd, buffer, count, offset);
}

static struct dppd_shared_count_spec counter(void)
{
    return (struct dppd_shared_count_spec){.id = 700,
        .generation = 11,
        .install_port_id = 3,
        .domain = DPPD_RULE_DOMAIN_TRANSFER};
}

static void acknowledged(const char *path)
{
    struct dppd_recovery_guard guard;
    assert(dppd_recovery_guard_open(&guard, path, NULL) == 0);
    assert(!guard.writable);
    assert(dppd_recovery_guard_acknowledge(&guard, guard.record.revision) == 0);
    dppd_recovery_guard_close(&guard);
}

static uint64_t create_counter(struct dppd_recovery_guard *guard, const struct dppd_shared_count_spec *spec)
{
    uint64_t attempt = 99;
    assert(dppd_recovery_guard_prepare_shared(guard, spec, "0000:01:00.0", "simulation", NULL) == 0);
    assert(dppd_recovery_guard_begin_shared(guard, spec, &attempt) == 0);
    assert(dppd_recovery_guard_created(guard, attempt, 0, DPPD_EVIDENCE_NONE, 0, NULL) == 0);
    return attempt;
}

/** 同号规则和计数器可以共存，种类、方向和精确引用必须在磁盘上保持可区分 */
static void typed_lifecycle(const char *path, const char *state)
{
    struct dppd_recovery_guard guard;
    struct dppd_shared_count_spec spec = counter();
    assert(dppd_recovery_guard_open(&guard, path, state) == 0 && guard.record.format == 5);
    assert(!dppd_recovery_record_requires_monitor(&guard.record));
    uint64_t shared = create_counter(&guard, &spec), flow = 99;
    assert(guard.record.format == 7 && guard.record.features == 0);
    assert(guard.record.ports[0].first_kind == DPPD_RECOVERY_OBJECT_SHARED_COUNT);
    assert(guard.record.attempts[0].kind == DPPD_RECOVERY_OBJECT_SHARED_COUNT);
    assert(guard.record.attempts[0].domain == DPPD_RULE_DOMAIN_TRANSFER);
    assert(dppd_recovery_guard_begin_flow(&guard, 3, spec.id, spec.generation, DPPD_RULE_DOMAIN_TRANSFER, spec.id,
               spec.generation, &flow) == 0);
    assert(flow > shared);
    assert(dppd_recovery_guard_created(&guard, flow, 0, DPPD_EVIDENCE_NONE, 0, NULL) == 0);
    assert(dppd_recovery_guard_removed(&guard, flow, -EBUSY) == 0);
    assert(guard.record.attempts[1].phase == DPPD_RECOVERY_CREATED && guard.record.attempts[1].remove_error == -EBUSY);
    assert(dppd_recovery_guard_clean(&guard) == -EUCLEAN);
    dppd_recovery_guard_close(&guard);
    assert(dppd_recovery_guard_open(&guard, path, NULL) == 0 && guard.record.format == 7);
    assert(guard.record.attempts[1].kind == DPPD_RECOVERY_OBJECT_FLOW);
    assert(
        guard.record.attempts[1].shared_id == spec.id && guard.record.attempts[1].shared_generation == spec.generation);
    assert(guard.record.attempts[1].domain == DPPD_RULE_DOMAIN_TRANSFER);
    assert(!dppd_recovery_record_requires_monitor(&guard.record));
    dppd_recovery_guard_close(&guard);
    acknowledged(path);
    assert(dppd_recovery_guard_open(&guard, path, state) == 0 && guard.record.format == 7);
    shared = create_counter(&guard, &spec);
    assert(dppd_recovery_guard_begin_flow(&guard, 3, 701, 12, DPPD_RULE_DOMAIN_TRANSFER, spec.id, spec.generation,
               &flow) == 0);
    assert(dppd_recovery_guard_created(&guard, flow, 0, DPPD_EVIDENCE_NONE, 0, NULL) == 0);
    assert(dppd_recovery_guard_removed(&guard, flow, 0) == 0);
    assert(dppd_recovery_guard_removed(&guard, shared, 0) == 0);
    assert(dppd_recovery_guard_clean(&guard) == 0 && guard.record.format == 7 && guard.record.attempt_count == 0);
    dppd_recovery_guard_close(&guard);
}

static void invalid_inputs(const char *path, const char *state)
{
    struct dppd_recovery_guard guard;
    struct dppd_shared_count_spec spec = counter(), invalid;
    assert(dppd_recovery_guard_open(&guard, path, state) == 0);
    uint64_t attempt = 99;
    unsigned int writes = write_calls;
    invalid = spec;
    invalid.id = UINT64_MAX;
    assert(dppd_recovery_guard_prepare_shared(&guard, &invalid, "device", "driver", NULL) == -EINVAL);
    invalid = spec;
    invalid.generation = 0;
    assert(dppd_recovery_guard_begin_shared(&guard, &invalid, &attempt) == -EINVAL);
    invalid = spec;
    invalid.domain = 3;
    assert(dppd_recovery_guard_begin_shared(&guard, &invalid, &attempt) == -EINVAL);
    assert(dppd_recovery_guard_begin_flow(&guard, 3, 1, 1, DPPD_RULE_DOMAIN_TRANSFER, spec.id, 0, &attempt) == -EINVAL);
    assert(dppd_recovery_guard_begin_flow(&guard, 3, 1, 1, DPPD_RULE_DOMAIN_TRANSFER, 0, spec.generation, &attempt) ==
        -EINVAL);
    assert(dppd_recovery_guard_begin_flow(&guard, 3, 1, 1, DPPD_RULE_DOMAIN_TRANSFER, spec.id, spec.generation,
               &attempt) == -ENOENT);
    assert(attempt == 99 && write_calls == writes);
    uint64_t shared = create_counter(&guard, &spec);
    writes = write_calls;
    assert(dppd_recovery_guard_begin_flow(&guard, 4, 1, 1, DPPD_RULE_DOMAIN_TRANSFER, spec.id, spec.generation,
               &attempt) == -ENOENT);
    assert(dppd_recovery_guard_begin_flow(&guard, 3, 1, 1, DPPD_RULE_DOMAIN_INGRESS, spec.id, spec.generation,
               &attempt) == -ENOENT);
    assert(write_calls == writes && attempt == 99);
    assert(dppd_recovery_guard_removed(&guard, shared, 0) == 0);
    writes = write_calls;
    assert(dppd_recovery_guard_begin_flow(&guard, 3, 1, 1, DPPD_RULE_DOMAIN_TRANSFER, spec.id, spec.generation,
               &attempt) == -ENOENT &&
        write_calls == writes);
    assert(dppd_recovery_guard_clean(&guard) == 0);
    assert(dppd_recovery_guard_prepare_shared(&guard, &spec, "0000:01:00.0", "simulation", NULL) == 0);
    assert(dppd_recovery_guard_begin_shared(&guard, &spec, &attempt) == 0);
    writes = write_calls;
    assert(dppd_recovery_guard_created(&guard, attempt, 0, DPPD_EVIDENCE_UNAVAILABLE, -ENOTSUP, NULL) == -EINVAL);
    assert(write_calls == writes && guard.record.attempts[0].phase == DPPD_RECOVERY_INTENT);
    assert(dppd_recovery_guard_begin_flow(&guard, 3, 1, 1, DPPD_RULE_DOMAIN_TRANSFER, spec.id, spec.generation,
               &shared) == -ENOENT &&
        write_calls == writes);
    assert(dppd_recovery_guard_created(&guard, attempt, -ENOTSUP, DPPD_EVIDENCE_NONE, 0, NULL) == 0);
    assert(dppd_recovery_guard_clean(&guard) == -EUCLEAN);
    assert(guard.record.attempts[0].phase == DPPD_RECOVERY_CREATE_FAILED);
    writes = write_calls;
    assert(dppd_recovery_guard_begin_flow(&guard, 3, 1, 1, DPPD_RULE_DOMAIN_TRANSFER, spec.id, spec.generation,
               &attempt) == -ENOENT &&
        write_calls == writes);
    dppd_recovery_guard_close(&guard);
    acknowledged(path);
}

/** 相同身份的失败重试可能复用更低槽位，引用必须检查最新编号且拒绝两个存活的成功记录 */
static void repeated_identity(const char *path, const char *state)
{
    struct dppd_recovery_guard guard;
    struct dppd_shared_count_spec spec = counter(), other = spec;
    other.id++;
    assert(dppd_recovery_guard_open(&guard, path, state) == 0);
    uint64_t free_slot = create_counter(&guard, &other);
    uint64_t earlier = create_counter(&guard, &spec);
    assert(dppd_recovery_guard_removed(&guard, free_slot, 0) == 0);
    uint64_t newer = 0, flow = 99;
    assert(dppd_recovery_guard_begin_shared(&guard, &spec, &newer) == 0 && newer > earlier);
    assert(guard.record.attempts[0].id == newer && guard.record.attempts[1].id == earlier);
    unsigned int writes = write_calls;
    assert(dppd_recovery_guard_begin_flow(&guard, 3, 1, 1, DPPD_RULE_DOMAIN_TRANSFER, spec.id, spec.generation,
               &flow) == -ENOENT);
    assert(flow == 99 && write_calls == writes);
    assert(dppd_recovery_guard_created(&guard, newer, -ENOSPC, DPPD_EVIDENCE_NONE, 0, NULL) == 0);
    assert(dppd_recovery_guard_begin_flow(&guard, 3, 1, 1, DPPD_RULE_DOMAIN_TRANSFER, spec.id, spec.generation,
               &flow) == -ENOENT);
    uint64_t current;
    assert(dppd_recovery_guard_begin_shared(&guard, &spec, &current) == 0 && current > newer);
    assert(dppd_recovery_guard_created(&guard, current, 0, DPPD_EVIDENCE_NONE, 0, NULL) == 0);
    writes = write_calls;
    assert(dppd_recovery_guard_begin_flow(&guard, 3, 1, 1, DPPD_RULE_DOMAIN_TRANSFER, spec.id, spec.generation,
               &flow) == -EUCLEAN);
    assert(flow == 99 && write_calls == writes);
    assert(dppd_recovery_guard_removed(&guard, earlier, 0) == 0);
    assert(dppd_recovery_guard_begin_flow(&guard, 3, 1, 1, DPPD_RULE_DOMAIN_TRANSFER, spec.id, spec.generation,
               &flow) == 0);
    assert(dppd_recovery_guard_created(&guard, flow, 0, DPPD_EVIDENCE_NONE, 0, NULL) == 0);
    assert(dppd_recovery_guard_removed(&guard, flow, 0) == 0);
    assert(dppd_recovery_guard_removed(&guard, current, 0) == 0);
    /** 较早失败且没有清理确认的尝试仍需外部核对，不能因后来成功创建过就覆盖掉 */
    assert(dppd_recovery_guard_clean(&guard) == -EUCLEAN);
    dppd_recovery_guard_close(&guard);
    acknowledged(path);
}

static void put(unsigned char *bytes, uint64_t value, unsigned int width)
{
    for (unsigned int i = 0; i < width; ++i)
        bytes[i] = (unsigned char)(value >> (8U * i));
}

static void seal(unsigned char *bytes, size_t size)
{
    uint32_t crc = UINT32_MAX;
    memset(bytes + 28, 0, 4);
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    put(bytes + 28, ~crc, 4);
}

/** 重算 CRC 后仍须拒绝未知种类、混用字段和保留字节，不能只证明校验和有效 */
static void corrupted_records(const char *path, const char *state)
{
    struct dppd_recovery_guard guard;
    const struct dppd_shared_count_spec spec = counter();
    assert(dppd_recovery_guard_open(&guard, path, state) == 0);
    (void)create_counter(&guard, &spec);
    uint64_t flow;
    assert(dppd_recovery_guard_begin_flow(&guard, 3, 701, 12, DPPD_RULE_DOMAIN_TRANSFER, spec.id, spec.generation,
               &flow) == 0);
    dppd_recovery_guard_close(&guard);
    int fd = open(path, O_RDWR);
    struct stat info;
    assert(fd >= 0 && fstat(fd, &info) == 0);
    assert(info.st_size == ATTEMPT_OFFSET + TYPED_STRIDE * DPPD_RECOVERY_ATTEMPT_LIMIT);
    size_t size = (size_t)info.st_size;
    unsigned char *saved = malloc(size), *changed = malloc(size);
    assert(saved != NULL && changed != NULL && pread(fd, saved, size, 0) == (ssize_t)size);
    const struct {
        size_t offset;
        uint64_t value;
        unsigned int width;
    } changes[] = {
        {8, 6, 4},
        {META_OFFSET + 12, 2, 4},
        {META_OFFSET + 12, DPPD_RECOVERY_FEATURE_MONITORED, 4},
        {META_OFFSET + 16, 1, 1},
        {PORT_OFFSET + 2, 2, 2},
        {ATTEMPT_OFFSET + 176, 2, 4},
        {ATTEMPT_OFFSET + 180, UINT32_MAX, 4},
        {ATTEMPT_OFFSET + 184, 1, 8},
        {ATTEMPT_OFFSET + 96, 1, 1},
        {ATTEMPT_OFFSET + 40, 1, 1},
        {ATTEMPT_OFFSET + 200, 1, 1},
        {ATTEMPT_OFFSET + TYPED_STRIDE + 192, 0, 8},
        {ATTEMPT_OFFSET + TYPED_STRIDE + 184, UINT64_MAX, 8},
        {ATTEMPT_OFFSET + TYPED_STRIDE + 180, UINT32_MAX, 4},
        {ATTEMPT_OFFSET + TYPED_STRIDE + 96, 1, 1},
    };
    for (unsigned int i = 0; i < sizeof(changes) / sizeof(*changes); ++i) {
        memcpy(changed, saved, size);
        put(changed + changes[i].offset, changes[i].value, changes[i].width);
        seal(changed, size);
        assert(pwrite(fd, changed, size, 0) == (ssize_t)size);
        assert(dppd_recovery_guard_open(&guard, path, NULL) == -EBADMSG);
    }
    memcpy(changed, saved, size);
    changed[ATTEMPT_OFFSET + 8] ^= 1;
    assert(pwrite(fd, changed, size, 0) == (ssize_t)size);
    assert(dppd_recovery_guard_open(&guard, path, NULL) == -EBADMSG);
    assert(pwrite(fd, saved, size, 0) == (ssize_t)size && close(fd) == 0);
    free(changed);
    free(saved);
    acknowledged(path);
}

/** 已有普通规则升级后仍是 FLOW，旧意图没有保存方向就必须继续报告未知 */
static void legacy_flow_upgrade(const char *path, const char *state)
{
    assert(unlink(path) == 0);
    struct dppd_recovery_guard guard;
    const struct dppd_shared_count_spec spec = counter();
    uint64_t flow, shared;
    assert(dppd_recovery_guard_open(&guard, path, state) == 0 && guard.record.format == 5);
    assert(dppd_recovery_guard_prepare(&guard, 3, "device", "driver", 50, 9) == 0);
    assert(dppd_recovery_guard_begin(&guard, 3, 50, 9, &flow) == 0);
    assert(dppd_recovery_guard_created(&guard, flow, 0, DPPD_EVIDENCE_NONE, 0, NULL) == 0);
    assert(dppd_recovery_guard_prepare_shared(&guard, &spec, "device", "driver", NULL) == 0);
    assert(dppd_recovery_guard_begin_shared(&guard, &spec, &shared) == 0);
    assert(dppd_recovery_guard_created(&guard, shared, 0, DPPD_EVIDENCE_NONE, 0, NULL) == 0);
    assert(guard.record.ports[0].first_kind == DPPD_RECOVERY_OBJECT_FLOW);
    dppd_recovery_guard_close(&guard);
    assert(dppd_recovery_guard_open(&guard, path, NULL) == 0 && guard.record.format == 7);
    assert(guard.record.attempts[0].kind == DPPD_RECOVERY_OBJECT_FLOW);
    assert(guard.record.attempts[0].domain == DPPD_RECOVERY_DOMAIN_UNKNOWN);
    assert(guard.record.attempts[0].shared_id == 0 && !dppd_recovery_record_requires_monitor(&guard.record));
    assert(guard.record.attempts[1].kind == DPPD_RECOVERY_OBJECT_SHARED_COUNT);
    assert(dppd_recovery_guard_acknowledge(&guard, guard.record.revision) == 0);
    dppd_recovery_guard_close(&guard);
}

/** 未监护 v5 能升级，已有 v6 监护则跨升级和清理一直保留要求 */
static void monitor_policy(const char *path, const char *state)
{
    struct dppd_recovery_guard guard;
    const uint8_t session[DPPD_TAP_COOKIE_SIZE] = {9};
    const struct dppd_recovery_identity identity = {.ifindex = 17,
        .ifname = "owned",
        .boot_id = "6c307404-452c-475d-83b2-e5aaf3ae0102",
        .netns_inode = 5};
    assert(unlink(path) == 0);
    assert(dppd_recovery_guard_open(&guard, path, state) == 0 && guard.record.format == 5);
    assert(dppd_recovery_guard_bind_monitor(&guard, session) == 0 && guard.record.format == 6);
    assert(dppd_recovery_record_requires_monitor(&guard.record));
    assert(dppd_recovery_guard_prepare_identity(&guard, 3, "net_tap0", "net_tap", 50, 9, &identity) == 0);
    const struct dppd_tap_content expected = {.version = 1,
        .action = 1,
        .parent = 0x10000,
        .priority = 2,
        .protocol = 3};
    uint8_t cookie[DPPD_TAP_COOKIE_SIZE];
    uint64_t flow;
    assert(dppd_recovery_guard_begin_owned(&guard, 3, 50, 9, &flow, cookie, &expected) == 0);
    assert(dppd_recovery_guard_created(&guard, flow, 0, DPPD_EVIDENCE_NONE, 0, NULL) == 0);
    struct dppd_shared_count_spec spec = counter();
    spec.domain = DPPD_RULE_DOMAIN_INGRESS;
    assert(dppd_recovery_guard_prepare_shared(&guard, &spec, "net_tap0", "net_tap", &identity) == 0);
    assert(guard.record.format == 7 && guard.record.features == DPPD_RECOVERY_FEATURE_MONITORED);
    assert(memcmp(guard.record.monitor_session, session, sizeof(session)) == 0);
    assert(guard.record.attempts[0].expected.version == 1);
    assert(memcmp(guard.record.attempts[0].owner_cookie, cookie, sizeof(cookie)) == 0);
    uint64_t attempt;
    assert(dppd_recovery_guard_begin_shared(&guard, &spec, &attempt) == 0);
    assert(dppd_recovery_guard_created(&guard, attempt, 0, DPPD_EVIDENCE_NONE, 0, NULL) == 0);
    assert(dppd_recovery_guard_removed(&guard, attempt, 0) == 0);
    assert(dppd_recovery_guard_removed(&guard, flow, 0) == 0);
    assert(dppd_recovery_guard_clean(&guard) == 0);
    assert(guard.record.format == 7 && dppd_recovery_record_requires_monitor(&guard.record));
    assert(!dppd_tap_cookie_present(guard.record.monitor_session) && !guard.monitor_bound);
    dppd_recovery_guard_close(&guard);
    assert(dppd_recovery_guard_open(&guard, path, state) == 0 && guard.record.format == 7);
    assert(dppd_recovery_guard_prepare_shared(&guard, &spec, "net_tap0", "net_tap", &identity) == -EACCES);
    assert(dppd_recovery_guard_bind_monitor(&guard, session) == 0 && guard.record.format == 7);
    assert(dppd_recovery_guard_prepare_shared(&guard, &spec, "net_tap0", "net_tap", &identity) == 0);
    assert(dppd_recovery_guard_begin_shared(&guard, &spec, &attempt) == 0);
    assert(dppd_recovery_guard_created(&guard, attempt, 0, DPPD_EVIDENCE_NONE, 0, NULL) == 0);
    dppd_recovery_guard_close(&guard);
    assert(dppd_recovery_guard_open(&guard, path, NULL) == 0 && dppd_recovery_record_requires_monitor(&guard.record));
    assert(dppd_recovery_guard_acknowledge(&guard, guard.record.revision) == 0);
    assert(dppd_recovery_record_requires_monitor(&guard.record));
    dppd_recovery_guard_close(&guard);
}

/** 突然退出保留真实落盘的意图或结果，后继进程必须阻断重放而非当作空后端 */
static void crash_windows(const char *path, const char *state)
{
    assert(unlink(path) == 0);
    for (unsigned int stage = 0; stage < 2; ++stage) {
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            struct dppd_recovery_guard guard;
            const struct dppd_shared_count_spec spec = counter();
            uint64_t attempt;
            assert(dppd_recovery_guard_open(&guard, path, state) == 0);
            assert(dppd_recovery_guard_prepare_shared(&guard, &spec, "device", "driver", NULL) == 0);
            assert(dppd_recovery_guard_begin_shared(&guard, &spec, &attempt) == 0);
            if (stage != 0)
                assert(dppd_recovery_guard_created(&guard, attempt, 0, DPPD_EVIDENCE_NONE, 0, NULL) == 0);
            _exit(0);
        }
        int status;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        struct dppd_recovery_guard guard;
        assert(dppd_recovery_guard_open(&guard, path, state) == 0 && !guard.writable);
        assert(guard.record.attempt_count == 1 && guard.record.attempts[0].kind == DPPD_RECOVERY_OBJECT_SHARED_COUNT);
        assert(guard.record.attempts[0].phase == (stage == 0 ? DPPD_RECOVERY_INTENT : DPPD_RECOVERY_CREATED));
        assert(dppd_recovery_guard_clean(&guard) == -EUCLEAN);
        assert(dppd_recovery_guard_acknowledge(&guard, guard.record.revision) == 0);
        dppd_recovery_guard_close(&guard);
    }
}

/** 写入或同步失败锁存 faulted，内存不提前发布，退出也不能把不确定记录改成干净 */
static void storage_failures(const char *path, const char *state)
{
    for (unsigned int stage = 0; stage < 4; ++stage) {
        struct dppd_recovery_guard guard;
        const struct dppd_shared_count_spec spec = counter();
        assert(dppd_recovery_guard_open(&guard, path, state) == 0);
        assert(dppd_recovery_guard_prepare_shared(&guard, &spec, "device", "driver", NULL) == 0);
        uint64_t attempt = 99;
        if (stage == 0)
            fail_write = write_calls + 1;
        else if (stage < 3)
            fail_sync = sync_calls + (stage == 1 ? 1 : 2);
        if (stage < 2) {
            assert(dppd_recovery_guard_begin_shared(&guard, &spec, &attempt) == -EIO);
            assert(attempt == 99 && guard.record.attempt_count == 0);
        } else if (stage == 2) {
            assert(dppd_recovery_guard_begin_shared(&guard, &spec, &attempt) == 0);
            assert(dppd_recovery_guard_created(&guard, attempt, 0, DPPD_EVIDENCE_NONE, 0, NULL) == -EIO);
            assert(guard.record.attempts[0].phase == DPPD_RECOVERY_INTENT);
        } else {
            assert(dppd_recovery_guard_begin_shared(&guard, &spec, &attempt) == 0);
            assert(dppd_recovery_guard_created(&guard, attempt, 0, DPPD_EVIDENCE_NONE, 0, NULL) == 0);
            fail_sync = sync_calls + 1;
            assert(dppd_recovery_guard_removed(&guard, attempt, 0) == -EIO);
            assert(guard.record.attempts[0].phase == DPPD_RECOVERY_CREATED);
        }
        assert(guard.faulted && dppd_recovery_guard_clean(&guard) == -EUCLEAN);
        assert(dppd_recovery_guard_removed(&guard, attempt, 0) == -EUCLEAN);
        fail_write = fail_sync = 0;
        dppd_recovery_guard_close(&guard);
        acknowledged(path);
    }
}

int main(void)
{
    char directory[] = "/tmp/dppd-shared-journal-XXXXXX", path[256], state[256];
    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/recovery", directory);
    snprintf(state, sizeof(state), "%s/snapshot", directory);
    typed_lifecycle(path, state);
    invalid_inputs(path, state);
    repeated_identity(path, state);
    corrupted_records(path, state);
    legacy_flow_upgrade(path, state);
    monitor_policy(path, state);
    crash_windows(path, state);
    storage_failures(path, state);
    assert(unlink(path) == 0 && rmdir(directory) == 0);
    return 0;
}
