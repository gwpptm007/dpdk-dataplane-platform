#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dppd/cleanup_journal.h"

static int failure;
static bool short_write;
ssize_t __real_pwrite64(int fd, const void *buffer, size_t size, off_t offset);
int __real_fsync(int fd);
int __real_rename(const char *from, const char *to);
int __real_renameat2(int from_fd, const char *from, int to_fd, const char *to, unsigned int flags);

/** 只截断日志临时文件的写入，随后一次真实错误模拟磁盘写满 */
ssize_t __wrap_pwrite64(int fd, const void *buffer, size_t size, off_t offset)
{
    if (failure == 1 && offset != 0) {
        errno = ENOSPC;
        return -1;
    }
    if ((failure == 1 || short_write) && size > 17)
        size = 17;
    return __real_pwrite64(fd, buffer, size, offset);
}

int __wrap_fsync(int fd)
{
    struct stat info;
    assert(fstat(fd, &info) == 0);
    if ((failure == 2 && S_ISREG(info.st_mode)) || (failure == 4 && S_ISDIR(info.st_mode))) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

int __wrap_rename(const char *from, const char *to)
{
    if (failure == 3) {
        errno = EIO;
        return -1;
    }
    return __real_rename(from, to);
}

int __wrap_renameat2(int from_fd, const char *from, int to_fd, const char *to, unsigned int flags)
{
    if (failure == 3) {
        errno = EIO;
        return -1;
    }
    return __real_renameat2(from_fd, from, to_fd, to, flags);
}

/** 日志层使用最小恢复文件样本，不伪造任何删除授权或内核观察结果 */
static void prepare(struct dppd_recovery_guard *guard, struct dppd_recovery_scope *scope, const char *directory)
{
    *guard = (struct dppd_recovery_guard){.fd = -1};
    snprintf(guard->path, sizeof(guard->path), "%s/recovery", directory);
    guard->fd = open(guard->path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    assert(guard->fd >= 0);
    unsigned char bytes[32] = {[28] = 0x78, [29] = 0x56, [30] = 0x34, [31] = 0x12};
    assert(write(guard->fd, bytes, sizeof(bytes)) == sizeof(bytes));
    guard->record.format = 6;
    guard->record.revision = 29;
    guard->record.attempt_count = 2;
    guard->record.attempts[0].id = 43;
    guard->record.attempts[1].id = 44;
    guard->record.monitor_session[0] = 3;
    *scope = (struct dppd_recovery_scope){.fd = -1, .enabled = true, .role = DPPD_SCOPE_RECOVERY, .id = {9}};
}

static void reopen(struct dppd_cleanup_journal *journal, struct dppd_recovery_guard *guard,
    struct dppd_recovery_scope *scope)
{
    dppd_cleanup_journal_close(journal);
    assert(dppd_cleanup_journal_open(journal, guard, scope) == 0);
}

static void test_progress(struct dppd_recovery_guard *guard, struct dppd_recovery_scope *scope)
{
    struct dppd_cleanup_journal journal = {.fd = -1};
    short_write = true;
    assert(dppd_cleanup_journal_open(&journal, guard, scope) == 0);
    short_write = false;
    assert(journal.sequence == 1 && journal.recovery_crc == 0x12345678U);
    assert(dppd_cleanup_journal_set(&journal, 0, DPPD_CLEANUP_FAILED, -EIO) == -EINVAL);
    assert(dppd_cleanup_journal_set(&journal, 0, DPPD_CLEANUP_DELETING, 0) == 0);
    reopen(&journal, guard, scope);
    assert(journal.entries[0].state == DPPD_CLEANUP_DELETING && journal.sequence == 2);
    assert(dppd_cleanup_journal_set(&journal, 0, DPPD_CLEANUP_FAILED, -ENOBUFS) == 0);
    reopen(&journal, guard, scope);
    assert(journal.entries[0].error == -ENOBUFS);
    assert(dppd_cleanup_journal_set(&journal, 0, DPPD_CLEANUP_DELETING, 0) == 0);
    assert(dppd_cleanup_journal_set(&journal, 0, DPPD_CLEANUP_REMOVED, 0) == 0);
    assert(dppd_cleanup_journal_set(&journal, 0, DPPD_CLEANUP_DELETING, 0) == -EINVAL);
    uint64_t sequence = journal.sequence;
    assert(dppd_cleanup_journal_set(&journal, 0, DPPD_CLEANUP_REMOVED, 0) == 0 && journal.sequence == sequence);
    /* 创建结果没落盘的槽位也能由执行层核验后保存已删除，原恢复文件不被改写 */
    assert(dppd_cleanup_journal_set(&journal, 1, DPPD_CLEANUP_REMOVED, 0) == 0);
    reopen(&journal, guard, scope);
    assert(journal.entries[0].state == DPPD_CLEANUP_REMOVED && journal.entries[1].state == DPPD_CLEANUP_REMOVED);
    unsigned char crc[4];
    assert(pread(guard->fd, crc, sizeof(crc), 28) == sizeof(crc) && crc[0] == 0x78);
    dppd_cleanup_journal_close(&journal);
}

static void test_failures(struct dppd_recovery_guard *guard, struct dppd_recovery_scope *scope)
{
    for (int mode = 1; mode <= 4; ++mode) {
        guard->record.revision++;
        struct dppd_cleanup_journal journal = {.fd = -1};
        assert(dppd_cleanup_journal_open(&journal, guard, scope) == 0);
        failure = mode;
        assert(dppd_cleanup_journal_set(&journal, 0, DPPD_CLEANUP_DELETING, 0) < 0);
        assert(journal.faulted);
        failure = 0;
        assert(dppd_cleanup_journal_set(&journal, 0, DPPD_CLEANUP_REMOVED, 0) == -EUCLEAN);
        dppd_cleanup_journal_close(&journal);
        journal.faulted = false;
        assert(dppd_cleanup_journal_open(&journal, guard, scope) == 0);
        /* rename 前失败保留旧版本，目录同步失败则保留完整的新版本供重新核验 */
        assert(journal.entries[0].state == (mode == 4 ? DPPD_CLEANUP_DELETING : DPPD_CLEANUP_PENDING));
        dppd_cleanup_journal_close(&journal);
    }
    for (int mode = 1; mode <= 4; ++mode) {
        guard->record.revision++;
        struct dppd_cleanup_journal journal = {.fd = -1};
        failure = mode;
        assert(dppd_cleanup_journal_open(&journal, guard, scope) < 0);
        assert(journal.faulted);
        failure = 0;
        dppd_cleanup_journal_close(&journal);
        assert(dppd_cleanup_journal_open(&journal, guard, scope) == 0);
        dppd_cleanup_journal_close(&journal);
    }
}

static void test_binding(struct dppd_recovery_guard *guard, struct dppd_recovery_scope *scope)
{
    guard->record.revision++;
    struct dppd_cleanup_journal journal = {.fd = -1};
    assert(dppd_cleanup_journal_open(&journal, guard, scope) == 0);
    dppd_cleanup_journal_close(&journal);
    scope->id[0]++;
    assert(dppd_cleanup_journal_open(&journal, guard, scope) < 0);
    scope->id[0]--;
    guard->record.monitor_session[0]++;
    assert(dppd_cleanup_journal_open(&journal, guard, scope) < 0);
    guard->record.monitor_session[0]--;
    guard->record.attempts[0].id++;
    assert(dppd_cleanup_journal_open(&journal, guard, scope) < 0);
    guard->record.attempts[0].id--;
    assert(dppd_cleanup_journal_open(&journal, guard, scope) == 0);
    dppd_cleanup_journal_close(&journal);
    unsigned char changed = 0x79;
    assert(__real_pwrite64(guard->fd, &changed, 1, 28) == 1);
    assert(dppd_cleanup_journal_open(&journal, guard, scope) < 0);
    changed = 0x78;
    assert(__real_pwrite64(guard->fd, &changed, 1, 28) == 1);
}

static void test_tampering(struct dppd_recovery_guard *guard, struct dppd_recovery_scope *scope)
{
    for (int mode = 0; mode < 7; ++mode) {
        guard->record.revision++;
        struct dppd_cleanup_journal journal = {.fd = -1};
        assert(dppd_cleanup_journal_open(&journal, guard, scope) == 0);
        char alternate[DPPD_STATE_PATH_CAPACITY + 8];
        snprintf(alternate, sizeof(alternate), "%s.other", journal.path);
        if (mode == 0)
            assert(link(journal.path, alternate) == 0);
        else if (mode == 1)
            assert(chmod(journal.path, 0644) == 0);
        else if (mode == 2) {
            assert(__real_rename(journal.path, alternate) == 0);
            assert(symlink(alternate, journal.path) == 0);
        } else if (mode == 3) {
            assert(__real_rename(journal.path, alternate) == 0);
            int fd = open(journal.path, O_CREAT | O_EXCL | O_WRONLY, 0600);
            assert(fd >= 0 && ftruncate(fd, DPPD_CLEANUP_JOURNAL_SIZE) == 0);
            close(fd);
        } else {
            int fd = open(journal.path, O_WRONLY);
            assert(fd >= 0);
            unsigned char byte = 1;
            off_t offset = mode == 4 ? 100 : mode == 5 ? DPPD_CLEANUP_JOURNAL_SIZE : 140;
            assert(__real_pwrite64(fd, &byte, 1, offset) == 1);
            close(fd);
        }
        assert(dppd_cleanup_journal_check(&journal) < 0 && journal.faulted);
        assert(dppd_cleanup_journal_set(&journal, 0, DPPD_CLEANUP_REMOVED, 0) == -EUCLEAN);
        dppd_cleanup_journal_close(&journal);
        assert(dppd_cleanup_journal_open(&journal, guard, scope) < 0);
    }
}

int main(void)
{
    char directory[] = "/tmp/dppd-cleanup-journal-XXXXXX";
    assert(mkdtemp(directory) != NULL);
    struct dppd_recovery_guard *guard = calloc(1, sizeof(*guard));
    struct dppd_recovery_scope scope;
    assert(guard != NULL);
    prepare(guard, &scope, directory);
    test_progress(guard, &scope);
    test_failures(guard, &scope);
    test_binding(guard, &scope);
    test_tampering(guard, &scope);
    close(guard->fd);
    free(guard);
    printf("cleanup journal checks passed; private evidence directory=%s\n", directory);
    return 0;
}
