#define _GNU_SOURCE
#include "dppd/recovery_monitor.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MONITOR_HEADER 128U
#define MONITOR_PORT 96U
#define MONITOR_SIZE (MONITOR_HEADER + DPPD_STATE_PATH_CAPACITY + MONITOR_PORT * DPPD_MAX_PORTS)
static const unsigned char magic[8] = {'D', 'P', 'P', 'M', 'O', 'N', '1', 0};

static void put(unsigned char *bytes, uint64_t value, unsigned int width)
{
    for (unsigned int index = 0; index < width; ++index)
        bytes[index] = (unsigned char)(value >> (8U * index));
}

static uint64_t get(const unsigned char *bytes, unsigned int width)
{
    uint64_t value = 0;
    for (unsigned int index = 0; index < width; ++index)
        value |= (uint64_t)bytes[index] << (8U * index);
    return value;
}

/** 校验和发现部分写入和文件损坏，不承担防伪认证 */
static uint32_t checksum(const unsigned char *bytes)
{
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0; index < MONITOR_SIZE; ++index) {
        crc ^= index >= 28 && index < 32 ? 0 : bytes[index];
        for (unsigned int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

static int timestamp(uint64_t *value)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -errno;
    *value = (uint64_t)now.tv_sec * 1000000000U + (uint64_t)now.tv_nsec;
    return 0;
}

static bool boot_id_valid(const char *value)
{
    if (strnlen(value, DPPD_RECOVERY_BOOT_SIZE) != 36)
        return false;
    for (unsigned int index = 0; index < 36; ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-')
                return false;
        } else if (!((value[index] >= '0' && value[index] <= '9') || (value[index] >= 'a' && value[index] <= 'f')))
            return false;
    }
    return true;
}

/** 字符串长度有界，端口身份不能重复，终态不能重新解释成活动会话 */
static bool valid(const struct dppd_monitor_record *record)
{
    if (!dppd_tap_cookie_present(record->session) || record->count == 0 || record->count > DPPD_MAX_PORTS ||
        record->pid == 0 || record->pid > INT_MAX || record->recovery_inode == 0 || record->started_ns == 0 ||
        record->updated_ns < record->started_ns || record->recovery_path[0] != '/' ||
        strnlen(record->recovery_path, DPPD_STATE_PATH_CAPACITY) == DPPD_STATE_PATH_CAPACITY)
        return false;
    if (record->state == DPPD_MONITOR_ACTIVE) {
        if (record->sequence != 1 || record->reason != DPPD_MONITOR_NONE || record->error != 0 ||
            record->started_ns != record->updated_ns)
            return false;
    } else if ((record->state != DPPD_MONITOR_INVALID && record->state != DPPD_MONITOR_STOPPED) ||
        record->sequence != 2 || record->reason < DPPD_MONITOR_LINK_CHANGED ||
        record->reason > DPPD_MONITOR_STOP_REQUESTED || record->error >= 0 || record->error < -4095 ||
        ((record->state == DPPD_MONITOR_STOPPED) != (record->reason == DPPD_MONITOR_STOP_REQUESTED)))
        return false;
    for (uint32_t index = 0; index < record->count; ++index) {
        const struct dppd_recovery_identity *identity = &record->ports[index].identity;
        if (identity->ifindex == 0 || identity->ifindex > INT_MAX || identity->netns_inode == 0 ||
            identity->ifname[0] == '\0' ||
            strnlen(identity->ifname, sizeof(identity->ifname)) == sizeof(identity->ifname) ||
            !boot_id_valid(identity->boot_id))
            return false;
        for (uint32_t other = 0; other < index; ++other)
            if (record->ports[other].port_id == record->ports[index].port_id ||
                record->ports[other].identity.ifindex == identity->ifindex ||
                strcmp(record->ports[other].identity.ifname, identity->ifname) == 0)
                return false;
        const struct dppd_recovery_identity *first = &record->ports[0].identity;
        if (strcmp(first->boot_id, identity->boot_id) != 0 || first->netns_device != identity->netns_device ||
            first->netns_inode != identity->netns_inode)
            return false;
    }
    return true;
}

/** 固定小端布局并清零保留位，绝不把 C 结构体填充字节直接写入文件 */
static void encode(const struct dppd_monitor_record *record, unsigned char bytes[MONITOR_SIZE])
{
    memset(bytes, 0, MONITOR_SIZE);
    memcpy(bytes, magic, sizeof(magic));
    put(bytes + 8, 1, 4);
    put(bytes + 12, MONITOR_SIZE, 4);
    put(bytes + 16, record->sequence, 8);
    put(bytes + 24, record->count, 4);
    memcpy(bytes + 32, record->session, 16);
    put(bytes + 48, record->state, 4);
    put(bytes + 52, record->reason, 4);
    put(bytes + 56, (uint32_t)record->error, 4);
    put(bytes + 60, record->pid, 4);
    put(bytes + 64, record->recovery_device, 8);
    put(bytes + 72, record->recovery_inode, 8);
    put(bytes + 80, record->baseline_revision, 8);
    put(bytes + 88, record->baseline_attempt, 8);
    put(bytes + 96, record->started_ns, 8);
    put(bytes + 104, record->updated_ns, 8);
    memcpy(bytes + MONITOR_HEADER, record->recovery_path, strlen(record->recovery_path));
    for (uint32_t index = 0; index < record->count; ++index) {
        unsigned char *row = bytes + MONITOR_HEADER + DPPD_STATE_PATH_CAPACITY + index * MONITOR_PORT;
        const struct dppd_recovery_identity *identity = &record->ports[index].identity;
        put(row, record->ports[index].port_id, 2);
        put(row + 4, identity->ifindex, 4);
        memcpy(row + 8, identity->ifname, strlen(identity->ifname));
        memcpy(row + 24, identity->boot_id, strlen(identity->boot_id));
        put(row + 64, identity->netns_device, 8);
        put(row + 72, identity->netns_inode, 8);
    }
    put(bytes + 28, checksum(bytes), 4);
}

static int decode(const unsigned char bytes[MONITOR_SIZE], struct dppd_monitor_record *record)
{
    unsigned char canonical[MONITOR_SIZE];
    if (memcmp(bytes, magic, sizeof(magic)) != 0 || get(bytes + 8, 4) != 1 || get(bytes + 12, 4) != MONITOR_SIZE ||
        get(bytes + 28, 4) != checksum(bytes))
        return -EBADMSG;
    memset(record, 0, sizeof(*record));
    record->sequence = get(bytes + 16, 8);
    record->count = get(bytes + 24, 4);
    if (record->count > DPPD_MAX_PORTS)
        return -EBADMSG;
    memcpy(record->session, bytes + 32, 16);
    record->state = get(bytes + 48, 4);
    record->reason = get(bytes + 52, 4);
    record->error = (int32_t)get(bytes + 56, 4);
    record->pid = get(bytes + 60, 4);
    record->recovery_device = get(bytes + 64, 8);
    record->recovery_inode = get(bytes + 72, 8);
    record->baseline_revision = get(bytes + 80, 8);
    record->baseline_attempt = get(bytes + 88, 8);
    record->started_ns = get(bytes + 96, 8);
    record->updated_ns = get(bytes + 104, 8);
    memcpy(record->recovery_path, bytes + MONITOR_HEADER, sizeof(record->recovery_path));
    for (uint32_t index = 0; index < record->count; ++index) {
        const unsigned char *row = bytes + MONITOR_HEADER + DPPD_STATE_PATH_CAPACITY + index * MONITOR_PORT;
        struct dppd_recovery_identity *identity = &record->ports[index].identity;
        record->ports[index].port_id = get(row, 2);
        identity->ifindex = get(row + 4, 4);
        memcpy(identity->ifname, row + 8, sizeof(identity->ifname));
        memcpy(identity->boot_id, row + 24, sizeof(identity->boot_id));
        identity->netns_device = get(row + 64, 8);
        identity->netns_inode = get(row + 72, 8);
    }
    if (!valid(record))
        return -EBADMSG;
    encode(record, canonical);
    return memcmp(bytes, canonical, MONITOR_SIZE) == 0 ? 0 : -EBADMSG;
}

/** 名称必须仍指向持有的私有普通文件，替换、别名和权限放宽都不能继续写 */
static int same_file(int fd, const char *path)
{
    struct stat held, named;
    if (fstat(fd, &held) != 0 || lstat(path, &named) != 0)
        return -errno;
    if (!S_ISREG(held.st_mode) || !S_ISREG(named.st_mode) || held.st_dev != named.st_dev ||
        held.st_ino != named.st_ino || held.st_nlink != 1 || (held.st_mode & 077) != 0)
        return -ESTALE;
    return 0;
}

static int save(struct dppd_monitor_store *store, const struct dppd_monitor_record *record)
{
    unsigned char bytes[MONITOR_SIZE];
    size_t done = 0;
    int rc = same_file(store->fd, store->path);
    if (rc != 0)
        goto fail;
    encode(record, bytes);
    while (done < sizeof(bytes)) {
        ssize_t written = pwrite(store->fd, bytes + done, sizeof(bytes) - done, (off_t)done);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            rc = written < 0 ? -errno : -EIO;
            goto fail;
        }
        done += (size_t)written;
    }
    if (fsync(store->fd) != 0) {
        rc = -errno;
        goto fail;
    }
    store->record = *record;
    return 0;
fail:
    store->faulted = true;
    return rc;
}

int dppd_monitor_create(struct dppd_monitor_store *store, const char *path, const struct dppd_monitor_record *record)
{
    char parent[DPPD_STATE_PATH_CAPACITY], resolved[DPPD_STATE_PATH_CAPACITY];
    if (store == NULL || path == NULL || record == NULL || path[0] == '\0' || strlen(path) >= sizeof(parent))
        return -EINVAL;
    if (store->fd >= 0)
        return -EALREADY;
    *store = (struct dppd_monitor_store){.fd = -1, .record = *record};
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    const char *name = slash == NULL ? path : strrchr(path, '/') + 1;
    if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return -EINVAL;
    if (slash != NULL)
        *slash = '\0';
    if (realpath(slash == NULL ? "." : slash == parent ? "/" : parent, resolved) == NULL)
        return -errno;
    int length = snprintf(store->path, sizeof(store->path), "%s%s%s", resolved, strcmp(resolved, "/") ? "/" : "", name);
    if (length < 0 || length >= (int)sizeof(store->path))
        return -ENAMETOOLONG;
    struct dppd_monitor_record *initial = &store->record;
    initial->sequence = 1;
    initial->pid = (uint32_t)getpid();
    initial->state = DPPD_MONITOR_ACTIVE;
    initial->reason = DPPD_MONITOR_NONE;
    initial->error = 0;
    int rc = timestamp(&initial->started_ns);
    if (rc != 0)
        return rc;
    initial->updated_ns = initial->started_ns;
    size_t done = 0;
    while (done < sizeof(initial->session)) {
        ssize_t received = getrandom(initial->session + done, sizeof(initial->session) - done, 0);
        if (received < 0 && errno == EINTR)
            continue;
        if (received <= 0)
            return received < 0 ? -errno : -EIO;
        done += (size_t)received;
    }
    if (!valid(initial))
        return -EINVAL;
    store->fd = open(store->path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (store->fd < 0)
        return -errno;
    if (flock(store->fd, LOCK_EX | LOCK_NB) != 0)
        rc = -errno;
    else
        rc = save(store, initial);
    if (rc == 0) {
        int directory = open(resolved, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0)
            rc = -errno;
        else {
            rc = fsync(directory) == 0 ? 0 : -errno;
            close(directory);
        }
    }
    if (rc != 0) {
        store->faulted = true;
        dppd_monitor_close(store);
    }
    return rc;
}

int dppd_monitor_finish(struct dppd_monitor_store *store, enum dppd_monitor_reason reason, int error)
{
    if (store == NULL || store->fd < 0)
        return -EINVAL;
    if (store->faulted)
        return -EUCLEAN;
    if (store->record.state != DPPD_MONITOR_ACTIVE)
        return -EALREADY;
    struct dppd_monitor_record next = store->record;
    next.sequence++;
    next.state = reason == DPPD_MONITOR_STOP_REQUESTED ? DPPD_MONITOR_STOPPED : DPPD_MONITOR_INVALID;
    next.reason = reason;
    next.error = error;
    int rc = timestamp(&next.updated_ns);
    if (rc != 0)
        return rc;
    if (!valid(&next))
        return -EINVAL;
    return save(store, &next);
}

int dppd_monitor_read(const char *path, struct dppd_monitor_record *record, bool *writer_locked)
{
    if (path == NULL || record == NULL || writer_locked == NULL)
        return -EINVAL;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    int rc = same_file(fd, path);
    struct stat info;
    unsigned char bytes[MONITOR_SIZE];
    *writer_locked = false;
    if (rc != 0)
        goto done;
    if (flock(fd, LOCK_SH | LOCK_NB) != 0) {
        if (errno != EWOULDBLOCK) {
            rc = -errno;
            goto done;
        }
        *writer_locked = true;
    }
    if (fstat(fd, &info) != 0) {
        rc = -errno;
        goto done;
    }
    if (info.st_size != MONITOR_SIZE) {
        rc = -EBADMSG;
        goto done;
    }
    size_t received = 0;
    while (received < sizeof(bytes)) {
        ssize_t count = pread(fd, bytes + received, sizeof(bytes) - received, (off_t)received);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            rc = count < 0 ? -errno : -EBADMSG;
            goto done;
        }
        received += (size_t)count;
    }
    rc = decode(bytes, record);
    if (rc == 0)
        rc = same_file(fd, path);
    /* 读取期间写入者可能刚好退出，再试共享锁，多个只读查询不会互相伪装成写入者 */
    if (rc == 0 && *writer_locked) {
        if (flock(fd, LOCK_SH | LOCK_NB) == 0)
            *writer_locked = false;
        else if (errno != EWOULDBLOCK)
            rc = -errno;
    }
done:
    close(fd);
    return rc;
}

void dppd_monitor_close(struct dppd_monitor_store *store)
{
    if (store != NULL && store->fd >= 0) {
        close(store->fd);
        store->fd = -1;
    }
}

const char *dppd_monitor_reason_name(enum dppd_monitor_reason reason)
{
    switch (reason) {
    case DPPD_MONITOR_NONE:
        return "none";
    case DPPD_MONITOR_LINK_CHANGED:
        return "interface-changed";
    case DPPD_MONITOR_OBSERVER_LOST:
        return "observer-lost";
    case DPPD_MONITOR_RECORD_REPLACED:
        return "recovery-record-replaced";
    case DPPD_MONITOR_STOP_REQUESTED:
        return "stop-requested";
    default:
        return "unknown";
    }
}
