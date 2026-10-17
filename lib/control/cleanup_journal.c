#define _GNU_SOURCE
#include "dppd/cleanup_journal.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const unsigned char magic[8] = {'D', 'P', 'P', 'C', 'L', 'N', '1', 0};

static void put(unsigned char *target, uint64_t value, unsigned int width)
{
    for (unsigned int i = 0; i < width; ++i)
        target[i] = (unsigned char)(value >> (8U * i));
}

static uint64_t get(const unsigned char *bytes, unsigned int width)
{
    uint64_t value = 0;
    for (unsigned int i = 0; i < width; ++i)
        value |= (uint64_t)bytes[i] << (8U * i);
    return value;
}

/** CRC 只发现损坏，私有目录和活着的受控作用域才承担权限边界 */
static uint32_t checksum(const unsigned char bytes[DPPD_CLEANUP_JOURNAL_SIZE])
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < DPPD_CLEANUP_JOURNAL_SIZE; ++i) {
        crc ^= i >= 20 && i < 24 ? 0 : bytes[i];
        for (unsigned int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

/** 固定 128 字节头部和 256 个 16 字节槽位，未使用区域也必须规范编码为零 */
static void encode(const struct dppd_cleanup_journal *journal, unsigned char bytes[DPPD_CLEANUP_JOURNAL_SIZE])
{
    memset(bytes, 0, DPPD_CLEANUP_JOURNAL_SIZE);
    memcpy(bytes, magic, sizeof(magic));
    put(bytes + 8, 1, 4);
    put(bytes + 12, DPPD_CLEANUP_JOURNAL_SIZE, 4);
    put(bytes + 16, journal->count, 4);
    put(bytes + 24, journal->sequence, 8);
    put(bytes + 32, journal->recovery_device, 8);
    put(bytes + 40, journal->recovery_inode, 8);
    put(bytes + 48, journal->revision, 8);
    put(bytes + 56, journal->recovery_crc, 4);
    memcpy(bytes + 64, journal->scope, 16);
    memcpy(bytes + 80, journal->session, 16);
    for (uint32_t i = 0; i < journal->count; ++i) {
        unsigned char *entry = bytes + 128 + 16U * i;
        put(entry, journal->entries[i].attempt, 8);
        put(entry + 8, journal->entries[i].state, 4);
        put(entry + 12, (uint32_t)journal->entries[i].error, 4);
    }
    put(bytes + 20, checksum(bytes), 4);
}

static bool valid_entry(enum dppd_cleanup_state state, int error)
{
    return state >= DPPD_CLEANUP_PENDING && state <= DPPD_CLEANUP_FAILED &&
        (state == DPPD_CLEANUP_FAILED ? error < 0 && error >= -4095 : error == 0);
}

static int fault(struct dppd_cleanup_journal *journal, int error)
{
    journal->faulted = true;
    return error;
}

static int unchanged(const struct dppd_cleanup_journal *journal)
{
    struct stat held, named;
    if (fstat(journal->fd, &held) != 0 || lstat(journal->path, &named) != 0)
        return -errno;
    if (!S_ISREG(named.st_mode) || held.st_dev != named.st_dev || held.st_ino != named.st_ino || held.st_nlink != 1 ||
        held.st_uid != geteuid() || (held.st_mode & 07777) != 0600 || held.st_size != DPPD_CLEANUP_JOURNAL_SIZE)
        return -ESTALE;
    return 0;
}

/** 只有恢复锁的持有者写日志，原子替换日志不会改变原恢复文件的 inode 和租约版本 */
static int save(struct dppd_cleanup_journal *journal)
{
    unsigned char bytes[DPPD_CLEANUP_JOURNAL_SIZE];
    char temporary[DPPD_STATE_PATH_CAPACITY], parent[DPPD_STATE_PATH_CAPACITY];
    int rc = journal->fd >= 0 ? unchanged(journal) : 0;
    if (rc != 0)
        return fault(journal, rc);
    if (snprintf(temporary, sizeof(temporary), "%s.XXXXXX", journal->path) >= (int)sizeof(temporary))
        return fault(journal, -ENAMETOOLONG);
    strcpy(parent, journal->path);
    char *slash = strrchr(parent, '/');
    if (slash == NULL)
        return fault(journal, -EINVAL);
    slash[slash == parent ? 1 : 0] = '\0';
    int directory = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0)
        return fault(journal, -errno);
    int fd = mkostemp(temporary, O_CLOEXEC);
    if (fd < 0) {
        rc = -errno;
        close(directory);
        journal->faulted = true;
        return rc;
    }
    encode(journal, bytes);
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
    if (journal->fd >= 0)
        rc = unchanged(journal);
    if (rc != 0)
        goto fail;
    int renamed = journal->fd < 0 ? renameat2(AT_FDCWD, temporary, AT_FDCWD, journal->path, RENAME_NOREPLACE)
                                  : rename(temporary, journal->path);
    if (renamed != 0) {
        rc = -errno;
        goto fail;
    }
    if (journal->fd >= 0)
        close(journal->fd);
    journal->fd = fd;
    rc = fsync(directory) == 0 ? 0 : -errno;
    close(directory);
    if (rc != 0)
        journal->faulted = true;
    return rc;
fail:
    close(fd);
    unlink(temporary);
    close(directory);
    journal->faulted = true;
    return rc;
}

int dppd_cleanup_journal_open(struct dppd_cleanup_journal *journal, const struct dppd_recovery_guard *guard,
    const struct dppd_recovery_scope *scope)
{
    if (journal == NULL || guard == NULL || scope == NULL || journal->fd >= 0 || guard->fd < 0 || !scope->enabled ||
        scope->role != DPPD_SCOPE_RECOVERY || guard->record.format != 6 ||
        guard->record.attempt_count > DPPD_RECOVERY_ATTEMPT_LIMIT || !dppd_tap_cookie_present(scope->id) ||
        !dppd_tap_cookie_present(guard->record.monitor_session))
        return -EINVAL;
    *journal = (struct dppd_cleanup_journal){.fd = -1,
        .sequence = 1,
        .revision = guard->record.revision,
        .count = guard->record.attempt_count};
    if (snprintf(journal->path, sizeof(journal->path), "%s.cleanup.%" PRIu64, guard->path, journal->revision) >=
        (int)sizeof(journal->path))
        return -ENAMETOOLONG;
    struct stat info;
    unsigned char header[32];
    if (fstat(guard->fd, &info) != 0)
        return -errno;
    ssize_t size = pread(guard->fd, header, sizeof(header), 0);
    if (size != sizeof(header))
        return size < 0 ? -errno : -EIO;
    journal->recovery_device = info.st_dev;
    journal->recovery_inode = info.st_ino;
    journal->recovery_crc = (uint32_t)get(header + 28, 4);
    memcpy(journal->scope, scope->id, 16);
    memcpy(journal->session, guard->record.monitor_session, 16);
    for (uint32_t i = 0; i < journal->count; ++i) {
        journal->entries[i].attempt = guard->record.attempts[i].id;
        journal->entries[i].state = DPPD_CLEANUP_PENDING;
    }
    journal->fd = open(journal->path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (journal->fd < 0)
        return errno == ENOENT ? save(journal) : -errno;
    int rc = unchanged(journal);
    if (rc != 0)
        goto fail;
    if (fstat(journal->fd, &info) != 0) {
        rc = -errno;
        goto fail;
    }
    unsigned char bytes[DPPD_CLEANUP_JOURNAL_SIZE], canonical[DPPD_CLEANUP_JOURNAL_SIZE];
    if (info.st_size != sizeof(bytes)) {
        rc = -EBADMSG;
        goto fail;
    }
    size = pread(journal->fd, bytes, sizeof(bytes), 0);
    if (size != sizeof(bytes)) {
        rc = size < 0 ? -errno : -EIO;
        goto fail;
    }
    encode(journal, canonical);
    /* 绑定字段必须与当前恢复事故完全相同，序号和进展槽位单独解码 */
    if (memcmp(bytes, canonical, 20) != 0 || get(bytes + 20, 4) != checksum(bytes) ||
        memcmp(bytes + 32, canonical + 32, 96) != 0 || get(bytes + 24, 8) == 0) {
        rc = -ESTALE;
        goto fail;
    }
    journal->sequence = get(bytes + 24, 8);
    for (uint32_t i = 0; i < journal->count; ++i) {
        const unsigned char *entry = bytes + 128 + 16U * i;
        enum dppd_cleanup_state state = (enum dppd_cleanup_state)get(entry + 8, 4);
        int error = (int32_t)get(entry + 12, 4);
        if (get(entry, 8) != journal->entries[i].attempt || !valid_entry(state, error)) {
            rc = -EBADMSG;
            goto fail;
        }
        journal->entries[i].state = state;
        journal->entries[i].error = error;
    }
    encode(journal, canonical);
    if (memcmp(bytes, canonical, sizeof(bytes)) != 0) {
        rc = -EBADMSG;
        goto fail;
    }
    rc = unchanged(journal);
    if (rc != 0)
        goto fail;
    return 0;
fail:
    dppd_cleanup_journal_close(journal);
    journal->faulted = true;
    return rc;
}

int dppd_cleanup_journal_set(struct dppd_cleanup_journal *journal, uint32_t index, enum dppd_cleanup_state state,
    int error)
{
    if (journal == NULL || journal->fd < 0 || index >= journal->count || !valid_entry(state, error))
        return -EINVAL;
    if (journal->faulted)
        return -EUCLEAN;
    int rc = dppd_cleanup_journal_check(journal);
    if (rc != 0)
        return rc;
    enum dppd_cleanup_state previous = journal->entries[index].state;
    if (previous == state && journal->entries[index].error == error)
        return 0;
    if (previous == DPPD_CLEANUP_REMOVED || state == DPPD_CLEANUP_PENDING ||
        (state == DPPD_CLEANUP_FAILED && previous != DPPD_CLEANUP_DELETING))
        return -EINVAL;
    if (journal->sequence == UINT64_MAX)
        return -EOVERFLOW;
    journal->sequence++;
    journal->entries[index].state = state;
    journal->entries[index].error = error;
    return save(journal);
}

int dppd_cleanup_journal_check(struct dppd_cleanup_journal *journal)
{
    if (journal == NULL || journal->fd < 0)
        return -EINVAL;
    if (journal->faulted)
        return -EUCLEAN;
    int rc = unchanged(journal);
    if (rc != 0)
        return fault(journal, rc);
    unsigned char expected[DPPD_CLEANUP_JOURNAL_SIZE], bytes[DPPD_CLEANUP_JOURNAL_SIZE];
    encode(journal, expected);
    ssize_t size = pread(journal->fd, bytes, sizeof(bytes), 0);
    if (size != sizeof(bytes))
        return fault(journal, size < 0 ? -errno : -EIO);
    if (memcmp(bytes, expected, sizeof(bytes)) != 0)
        return fault(journal, -EBADMSG);
    return 0;
}

void dppd_cleanup_journal_close(struct dppd_cleanup_journal *journal)
{
    if (journal != NULL && journal->fd >= 0) {
        close(journal->fd);
        journal->fd = -1;
    }
}
