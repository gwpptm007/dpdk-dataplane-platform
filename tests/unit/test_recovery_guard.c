#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "dppd/recovery_guard.h"
#include "dppd/rte_flow_backend.h"

static unsigned int syncs, creates, removes;
static unsigned int fail_sync;
static bool partial_create, fail_remove;
static struct dppd_recovery_guard *active_guard;

int __real_fsync(int fd);
int __wrap_fsync(int fd);

/** 只在指定一次同步处模拟磁盘故障，其余调用真实落盘，验证先记录后驱动的顺序 */
int __wrap_fsync(int fd)
{
    if (++syncs == fail_sync) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

/** 测试固定设备身份，把真实恢复保护接到正式 backend 的安装入口 */
static int prepare(void *context, uint16_t port, const struct dppd_rule *rule, uint64_t *attempt)
{
    *attempt = 0;
    return dppd_recovery_guard_prepare(context, port, "0000:01:00.0", "test-driver",
        rule->id, rule->generation);
}

/** 允许事务进入安装阶段，不代表任何真实 PMD 能力 */
static int validate(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_error *error)
{
    (void)port; (void)rule; (void)error;
    return 0;
}

/** 驱动边界必须看见已同步的待核对标记，部分失败可留下 handle 供退出清理测试 */
static int create(uint16_t port, const struct dppd_rule *rule,
    struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    assert(active_guard->record.count == 1 && !active_guard->faulted);
    assert(syncs >= 3);
    creates++;
    handle->flow = (struct rte_flow *)(uintptr_t)1;
    handle->port_id = port;
    handle->rule_id = rule->id;
    handle->rule_generation = rule->generation;
    return partial_create ? -EIO : 0;
}

/** 删除失败必须保留 handle，成功才允许恢复文件标记为干净 */
static int remove_flow(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    removes++;
    if (fail_remove)
        return -EFAULT;
    memset(handle, 0, sizeof(*handle));
    return 0;
}

/** 真实事务失败也要回滚本地预留项，写盘失败时驱动 create 的调用次数必须为零 */
static void backend_contract(const char *path, const char *state)
{
    const struct dppd_flow_api api = {.validate = validate, .create = create, .remove = remove_flow};
    struct dppd_recovery_guard guard;
    struct dppd_rte_flow_backend backend;
    struct dppd_transaction_backends backends = {0};
    struct dppd_transaction_item item = {0};
    struct dppd_transaction transaction;

    item.rule.id = 77;
    item.rule.generation = 9;
    item.plan.rule_id = 77;
    item.plan.rule_generation = 9;
    item.plan.install_port_id = 5;
    item.plan.backend = DPPD_PLAN_BACKEND_RTE_FLOW;
    assert(dppd_recovery_guard_open(&guard, path, state) == 0);
    active_guard = &guard;
    assert(dppd_rte_flow_backend_init(&backend, 2, &api) == 0);
    backend.before_create = prepare;
    backend.before_create_context = &guard;
    backends.rte_flow = dppd_rte_flow_transaction_backend(&backend);
    fail_sync = syncs + 1;
    assert(dppd_transaction_init(&transaction, 1, &item, 1) == 0);
    assert(dppd_transaction_run(&transaction, &backends) == -EIO);
    assert(creates == 0 && backend.count == 0 && guard.faulted);
    assert(dppd_recovery_guard_clean(&guard) == -EUCLEAN);
    assert(dppd_rte_flow_backend_fini(&backend) == 0);
    dppd_recovery_guard_close(&guard);
    assert(dppd_recovery_guard_open(&guard, path, NULL) == 0);
    assert(guard.record.count == 1);
    assert(dppd_recovery_guard_acknowledge(&guard, guard.record.revision) == 0);
    dppd_recovery_guard_close(&guard);

    assert(dppd_recovery_guard_open(&guard, path, state) == 0);
    assert(dppd_rte_flow_backend_init(&backend, 2, &api) == 0);
    backend.before_create = prepare;
    backend.before_create_context = &guard;
    backends.rte_flow = dppd_rte_flow_transaction_backend(&backend);
    partial_create = fail_remove = true;
    assert(dppd_transaction_init(&transaction, 2, &item, 1) == 0);
    /** 底层事务保留创建的原始错误，控制服务才把未完成补偿包装为 EUCLEAN */
    assert(dppd_transaction_run(&transaction, &backends) == -EIO && transaction.rollback_code == -EFAULT);
    assert(creates == 1 && backend.count == 1 && guard.record.count == 1);
    assert(dppd_rte_flow_backend_fini(&backend) == -EFAULT);
    assert(backend.count == 1 && backend.objects != NULL);
    fail_remove = false;
    assert(dppd_rte_flow_backend_fini(&backend) == 0 && removes == 3);
    assert(dppd_recovery_guard_clean(&guard) == 0);
    dppd_recovery_guard_close(&guard);
    assert(dppd_recovery_guard_open(&guard, path, NULL) == 0 && guard.record.count == 0);
    dppd_recovery_guard_close(&guard);
}

/** 锁、精确确认版本、快照绑定和关闭不清除，都是跨进程边界而非内存状态约定 */
static void lifecycle(const char *path, const char *state)
{
    struct dppd_recovery_guard guard, second;
    struct stat info;
    unsigned int prior_syncs;
    uint64_t revision;
    int status;
    pid_t child;

    assert(dppd_recovery_guard_open(&guard, path, state) == 0);
    assert(stat(path, &info) == 0 && (info.st_mode & 0777) == 0600);
    assert(guard.record.count == 0 && guard.record.revision == 0);
    assert(dppd_recovery_guard_open(&second, path, NULL) == -EBUSY);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        dppd_recovery_guard_close(&guard);
        _exit(dppd_recovery_guard_open(&second, path, state) == -EBUSY ? 0 : 1);
    }
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(dppd_recovery_guard_prepare(&guard, 5, "device-a", "driver", UINT64_MAX, 1) == 0);
    prior_syncs = syncs;
    assert(dppd_recovery_guard_prepare(&guard, 5, "device-a", "driver", 2, 2) == 0);
    assert(syncs == prior_syncs && guard.record.revision == 1 && guard.record.count == 1);
    assert(dppd_recovery_guard_prepare(&guard, 5, "different", "driver", 2, 2) == -EXDEV);
    assert(dppd_recovery_guard_prepare(&guard, 9, "device-b", "driver", 3, 3) == 0);
    revision = guard.record.revision;
    assert(revision == 2 && guard.record.count == 2);
    dppd_recovery_guard_close(&guard);
    assert(dppd_recovery_guard_open(&guard, path, state) == 0 && !guard.writable);
    assert(guard.record.ports[0].first_rule == UINT64_MAX && guard.record.ports[1].port_id == 9);
    assert(dppd_recovery_guard_clean(&guard) == -EUCLEAN);
    assert(dppd_recovery_guard_prepare(&guard, 5, "device-a", "driver", 4, 4) == -EUCLEAN);
    assert(dppd_recovery_guard_acknowledge(&guard, revision - 1) == -ESTALE);
    assert(dppd_recovery_guard_acknowledge(&guard, revision) == 0);
    assert(guard.record.revision == revision + 1 && guard.record.count == 0);
    assert(dppd_recovery_guard_acknowledge(&guard, revision) == -ESTALE);
    assert(dppd_recovery_guard_acknowledge(&guard, revision + 1) == -EALREADY);
    dppd_recovery_guard_close(&guard);
    assert(dppd_recovery_guard_open(&guard, path, "/tmp/a-different-snapshot") == -EXDEV);
}

/** 子进程写入后直接结束，模拟没有清理流程的崩溃，新进程必须仍能读出端口线索 */
static void crash_and_capacity(const char *path, const char *state)
{
    struct dppd_recovery_guard guard;
    int status;
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(dppd_recovery_guard_open(&guard, path, state) == 0);
        for (uint16_t index = 0; index < DPPD_MAX_PORTS; ++index)
            assert(dppd_recovery_guard_prepare(&guard, index, "device", "driver", index + 1, 1) == 0);
        assert(dppd_recovery_guard_prepare(&guard, DPPD_MAX_PORTS, "device", "driver", 99, 1) == -EOVERFLOW);
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(dppd_recovery_guard_open(&guard, path, state) == 0);
    assert(guard.record.count == DPPD_MAX_PORTS && !guard.writable);
    assert(dppd_recovery_guard_acknowledge(&guard, guard.record.revision) == 0);
    dppd_recovery_guard_close(&guard);
}

/** 路径替换、符号链接、截断和位翻转都不能绕过保护，也不能覆盖坏记录重新初始化 */
static void damaged_files(const char *path, const char *state, const char *backup)
{
    struct dppd_recovery_guard guard;
    unsigned char byte;
    int fd;

    assert(dppd_recovery_guard_open(&guard, path, state) == 0);
    assert(dppd_recovery_guard_prepare(&guard, 5, "device", "driver", 1, 1) == 0);
    assert(rename(path, backup) == 0);
    assert(dppd_recovery_guard_prepare(&guard, 5, "device", "driver", 2, 2) == -ENOENT);
    assert(dppd_recovery_guard_clean(&guard) == -EUCLEAN);
    dppd_recovery_guard_close(&guard);
    assert(symlink(backup, path) == 0);
    assert(dppd_recovery_guard_open(&guard, path, state) == -ELOOP);
    assert(unlink(path) == 0 && rename(backup, path) == 0);
    fd = open(path, O_RDWR);
    assert(fd >= 0 && pread(fd, &byte, 1, 80) == 1);
    byte ^= 1;
    assert(pwrite(fd, &byte, 1, 80) == 1);
    assert(dppd_recovery_guard_open(&guard, path, state) == -EBADMSG);
    byte ^= 1;
    assert(pwrite(fd, &byte, 1, 80) == 1);
    assert(chmod(path, 0644) == 0);
    assert(dppd_recovery_guard_open(&guard, path, state) == -ESTALE);
    assert(chmod(path, 0600) == 0);
    assert(link(path, backup) == 0);
    assert(dppd_recovery_guard_open(&guard, path, state) == -ESTALE);
    assert(unlink(backup) == 0);
    assert(dppd_recovery_guard_open(&guard, path, path) == -EINVAL);
    assert(ftruncate(fd, 0) == 0 && close(fd) == 0);
    assert(dppd_recovery_guard_open(&guard, path, state) == -EBADMSG);
}

/** 测试将正式 v2 编码收窄为既有 v1 布局并重算 CRC，避免用内存结构冒充磁盘兼容性 */
static void legacy_file(const char *path, unsigned int format)
{
    unsigned char current[33856], legacy[9248] = {0};
    size_t size = format == 1 ? 7712 : 9248;
    uint32_t crc = UINT32_MAX;
    int fd = open(path, O_RDWR);
    assert(fd >= 0 && pread(fd, current, sizeof(current), 0) == sizeof(current));
    memcpy(legacy, current, 4128);
    legacy[8] = (unsigned char)format;
    legacy[12] = (unsigned char)size;
    legacy[13] = (unsigned char)(size >> 8);
    memset(legacy + 28, 0, 4);
    for (unsigned int index = 0; index < DPPD_MAX_PORTS; ++index)
        memcpy(legacy + 4128 + index * (format == 1 ? 224 : 320), current + 4128 + index * 320,
            format == 1 ? 216 : 320);
    for (size_t index = 0; index < size; ++index) {
        crc ^= legacy[index];
        for (unsigned int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    crc = ~crc;
    for (unsigned int index = 0; index < 4; ++index)
        legacy[28 + index] = (unsigned char)(crc >> (index * 8));
    assert(pwrite(fd, legacy, size, 0) == (ssize_t)size);
    assert(ftruncate(fd, (off_t)size) == 0 && close(fd) == 0);
}

/** 定位信息必须完整保存，同端口更换身份不能复用旧标记，v1 待确认文件禁止自动升级 */
static void identity_and_compatibility(const char *path, const char *state)
{
    struct dppd_recovery_guard guard;
    struct dppd_recovery_identity identity = {.ifindex = 123, .ifname = "test-tap",
        .boot_id = "01234567-89ab-cdef-0123-456789abcdef", .netns_device = 4, .netns_inode = 567};
    struct stat info;
    assert(dppd_recovery_guard_open(&guard, path, state) == 0 && guard.record.format == 3);
    identity.boot_id[0] = 'z';
    assert(dppd_recovery_guard_prepare_identity(&guard, 0, "net_tap0", "net_tap", 1, 1, &identity) == -EINVAL);
    identity.boot_id[0] = '0';
    assert(dppd_recovery_guard_prepare_identity(&guard, 0, "net_tap0", "net_ring", 1, 1, &identity) == -EINVAL);
    assert(dppd_recovery_guard_prepare_identity(&guard, 0, "net_tap0", "net_tap", 1, 1, &identity) == 0);
    uint64_t revision = guard.record.revision;
    identity.netns_inode++;
    assert(dppd_recovery_guard_prepare_identity(&guard, 0, "net_tap0", "net_tap", 2, 2, &identity) == -EXDEV);
    identity.netns_inode--;
    assert(dppd_recovery_guard_prepare(&guard, 0, "net_tap0", "net_tap", 2, 2) == -EXDEV);
    assert(dppd_recovery_guard_prepare_identity(&guard, 0, "net_tap0", "net_tap", 2, 2, &identity) == 0);
    dppd_recovery_guard_close(&guard);
    assert(dppd_recovery_guard_open(&guard, path, NULL) == 0);
    assert(guard.record.revision == revision && guard.record.ports[0].identity.ifindex == 123);
    assert(guard.record.ports[0].identity.netns_inode == 567);
    assert(strcmp(guard.record.ports[0].identity.boot_id, identity.boot_id) == 0);
    dppd_recovery_guard_close(&guard);
    legacy_file(path, 1);
    assert(dppd_recovery_guard_open(&guard, path, state) == 0 && !guard.writable);
    assert(guard.record.format == 1 && guard.record.revision == revision);
    assert(guard.record.ports[0].identity.ifindex == 0);
    assert(dppd_recovery_guard_acknowledge(&guard, revision) == 0 && guard.record.format == 1);
    dppd_recovery_guard_close(&guard);
    assert(dppd_recovery_guard_open(&guard, path, NULL) == 0 && guard.record.format == 1);
    assert(stat(path, &info) == 0 && info.st_size == 7712);
    dppd_recovery_guard_close(&guard);
    assert(dppd_recovery_guard_open(&guard, path, state) == 0 && guard.writable);
    assert(guard.record.format == 3 && guard.record.revision == revision + 2);
    assert(stat(path, &info) == 0 && info.st_size == 33856);
    dppd_recovery_guard_close(&guard);
    legacy_file(path, 2);
    assert(dppd_recovery_guard_open(&guard, path, NULL) == 0 && guard.record.format == 2);
    assert(guard.record.revision == revision + 2);
    dppd_recovery_guard_close(&guard);
    assert(dppd_recovery_guard_open(&guard, path, state) == 0 && guard.writable);
    assert(guard.record.format == 3 && guard.record.revision == revision + 3);
    dppd_recovery_guard_close(&guard);
}

/** 所有路径都在测试私有目录内，测试结束后只删除自己创建的文件 */
int main(void)
{
    char directory[] = "/tmp/dppd-guard-unit-XXXXXX";
    char path[256], state[256], backup[256];
    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/recovery", directory);
    snprintf(state, sizeof(state), "%s/snapshot", directory);
    snprintf(backup, sizeof(backup), "%s/backup", directory);
    lifecycle(path, state);
    crash_and_capacity(path, state);
    backend_contract(path, state);
    identity_and_compatibility(path, state);
    damaged_files(path, state, backup);
    assert(unlink(path) == 0 && rmdir(directory) == 0);
    return 0;
}
