#define _GNU_SOURCE
#include "dppd/recovery_guard.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/** 固定长度、小端编码、CRC32，坏文件必须阻止启动，不能被解释成空白或已清理 */
#define RECORD_HEADER 32U
#define RECORD_PORT 224U
#define RECORD_SIZE (RECORD_HEADER + DPPD_STATE_PATH_CAPACITY + DPPD_MAX_PORTS * RECORD_PORT)
static const unsigned char magic[8] = {'D', 'P', 'P', 'R', 'E', 'C', '1', 0};

/** 逐字节编码避免对齐和主机字节序影响，宽度只使用 2、4、8 字节 */
static void put(unsigned char *target, uint64_t value, unsigned int width)
{
    for (unsigned int index = 0; index < width; ++index)
        target[index] = (unsigned char)(value >> (index * 8U));
}

/** 文件字段不借用结构体指针，先解码为整数再验证含义 */
static uint64_t get(const unsigned char *source, unsigned int width)
{
    uint64_t value = 0;
    for (unsigned int index = 0; index < width; ++index)
        value |= (uint64_t)source[index] << (index * 8U);
    return value;
}

/** CRC 用于发现截断或部分写入，不作为身份认证，目录仍须由部署方保护 */
static uint32_t checksum(const unsigned char *bytes)
{
    uint32_t crc = UINT32_MAX;
    for (unsigned int index = 0; index < RECORD_SIZE; ++index) {
        crc ^= index >= 28 && index < 32 ? 0 : bytes[index];
        for (unsigned int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

/** 每次完整编码都清零保留字段和字符串尾部，便于严格拒绝未知布局 */
static void encode(const struct dppd_recovery_record *record, unsigned char *bytes)
{
    memset(bytes, 0, RECORD_SIZE);
    memcpy(bytes, magic, sizeof(magic));
    put(bytes + 8, 1, 4);
    put(bytes + 12, RECORD_SIZE, 4);
    put(bytes + 16, record->revision, 8);
    put(bytes + 24, record->count, 4);
    memcpy(bytes + RECORD_HEADER, record->state_path, strlen(record->state_path));
    for (uint32_t index = 0; index < record->count; ++index) {
        const struct dppd_recovery_port *port = &record->ports[index];
        unsigned char *row = bytes + RECORD_HEADER + DPPD_STATE_PATH_CAPACITY + index * RECORD_PORT;
        put(row, port->port_id, 2);
        put(row + 8, port->first_rule, 8);
        put(row + 16, port->first_generation, 8);
        memcpy(row + 24, port->device, strlen(port->device));
        memcpy(row + 152, port->driver, strlen(port->driver));
    }
    put(bytes + 28, checksum(bytes), 4);
}

/** 非空且终止的字符串才可被日志和离线工具使用，避免坏文件引发越界读取 */
static bool valid_string(const char *value, size_t capacity)
{
    return value[0] != '\0' && memchr(value, '\0', capacity) != NULL;
}

/** 验证校验和、唯一端口和完整规范编码，预留位或无效尾部也不能静默接受 */
static int decode(const unsigned char *bytes, struct dppd_recovery_record *record)
{
    unsigned char canonical[RECORD_SIZE];

    if (memcmp(bytes, magic, sizeof(magic)) != 0 || get(bytes + 8, 4) != 1 ||
        get(bytes + 12, 4) != RECORD_SIZE || get(bytes + 28, 4) != checksum(bytes))
        return -EBADMSG;
    memset(record, 0, sizeof(*record));
    record->revision = get(bytes + 16, 8);
    record->count = (uint32_t)get(bytes + 24, 4);
    memcpy(record->state_path, bytes + RECORD_HEADER, sizeof(record->state_path));
    if (record->count > DPPD_MAX_PORTS || (record->count != 0 && record->revision == 0) ||
        !valid_string(record->state_path, sizeof(record->state_path)) || record->state_path[0] != '/')
        return -EBADMSG;
    for (uint32_t index = 0; index < record->count; ++index) {
        struct dppd_recovery_port *port = &record->ports[index];
        const unsigned char *row = bytes + RECORD_HEADER + DPPD_STATE_PATH_CAPACITY + index * RECORD_PORT;
        port->port_id = (uint16_t)get(row, 2);
        port->first_rule = get(row + 8, 8);
        port->first_generation = get(row + 16, 8);
        memcpy(port->device, row + 24, sizeof(port->device));
        memcpy(port->driver, row + 152, sizeof(port->driver));
        if (!valid_string(port->device, sizeof(port->device)) ||
            !valid_string(port->driver, sizeof(port->driver)) || !port->first_rule || !port->first_generation)
            return -EBADMSG;
        for (uint32_t prior = 0; prior < index; ++prior)
            if (record->ports[prior].port_id == port->port_id)
                return -EBADMSG;
    }
    encode(record, canonical);
    return memcmp(bytes, canonical, RECORD_SIZE) == 0 ? 0 : -EBADMSG;
}

/** 解析父目录后保留文件名，允许快照尚不存在，同时拒绝目录本身和超长路径 */
static int canonical_path(const char *path, char *output)
{
    char copy[DPPD_STATE_PATH_CAPACITY], parent[DPPD_STATE_PATH_CAPACITY];
    char *name, *slash;
    int length;

    if (path == NULL || path[0] == '\0' || strlen(path) >= sizeof(copy))
        return -EINVAL;
    strcpy(copy, path);
    slash = strrchr(copy, '/');
    name = slash == NULL ? copy : slash + 1;
    if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return -EINVAL;
    if (slash != NULL)
        *slash = '\0';
    if (realpath(slash == NULL ? "." : slash == copy ? "/" : copy, parent) == NULL)
        return -errno;
    length = snprintf(output, DPPD_STATE_PATH_CAPACITY, "%s%s%s", parent,
        strcmp(parent, "/") == 0 ? "" : "/", name);
    return length < 0 || length >= (int)DPPD_STATE_PATH_CAPACITY ? -ENAMETOOLONG : 0;
}

/** 文件锁不能跨替换的新 inode 生效，每次安装前核对名称仍指向持锁的普通文件 */
static int same_file(struct dppd_recovery_guard *guard)
{
    struct stat held, named;
    if (guard == NULL || guard->fd < 0)
        return -EINVAL;
    if (fstat(guard->fd, &held) != 0 || lstat(guard->path, &named) != 0)
        return -errno;
    if (!S_ISREG(named.st_mode) || held.st_dev != named.st_dev || held.st_ino != named.st_ino ||
        held.st_nlink != 1 || (held.st_mode & 077) != 0)
        return -ESTALE;
    return 0;
}

/**
 * 在持锁的同一个 inode 上完整写入并同步，避免 rename 后其他进程取得另一个文件锁
 * 中途失败或崩溃可能留下坏文件，下次必须拒绝启动，不把不完整写入当作清理成功
 */
static int save(struct dppd_recovery_guard *guard, const struct dppd_recovery_record *record)
{
    unsigned char bytes[RECORD_SIZE];
    size_t done = 0;
    int rc = same_file(guard);

    if (rc != 0)
        goto fail;
    encode(record, bytes);
    while (done < sizeof(bytes)) {
        ssize_t written = pwrite(guard->fd, bytes + done, sizeof(bytes) - done, (off_t)done);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            rc = written < 0 ? -errno : -EIO;
            goto fail;
        }
        done += (size_t)written;
    }
    if (fsync(guard->fd) != 0) {
        rc = -errno;
        goto fail;
    }
    guard->record = *record;
    return 0;
fail:
    guard->faulted = true;
    return rc;
}

/** 只关闭描述符，所有自动清除都必须由成功删除全部 flow 的调用方显式请求 */
void dppd_recovery_guard_close(struct dppd_recovery_guard *guard)
{
    if (guard != NULL && guard->fd >= 0) {
        close(guard->fd);
        guard->fd = -1;
        guard->writable = false;
    }
}

/** 打开即持有排他锁，已有待核对记录可以读取，但不能被本次 daemon 自动清除 */
int dppd_recovery_guard_open(struct dppd_recovery_guard *guard,
    const char *path, const char *state_path)
{
    char state[DPPD_STATE_PATH_CAPACITY], parent[DPPD_STATE_PATH_CAPACITY];
    unsigned char bytes[RECORD_SIZE];
    struct stat info;
    bool created = false;
    size_t done = 0;
    int rc, directory;

    if (guard == NULL)
        return -EINVAL;
    memset(guard, 0, sizeof(*guard));
    guard->fd = -1;
    rc = canonical_path(path, guard->path);
    if (rc != 0)
        return rc;
    if (state_path != NULL) {
        rc = canonical_path(state_path, state);
        if (rc != 0 || strcmp(state, guard->path) == 0)
            return rc != 0 ? rc : -EINVAL;
        guard->fd = open(guard->path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        created = guard->fd >= 0;
        if (guard->fd < 0 && errno != EEXIST)
            return -errno;
    }
    if (guard->fd < 0)
        guard->fd = open(guard->path, O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (guard->fd < 0)
        return -errno;
    if (flock(guard->fd, LOCK_EX | LOCK_NB) != 0) {
        rc = errno == EWOULDBLOCK ? -EBUSY : -errno;
        goto fail;
    }
    rc = same_file(guard);
    if (rc != 0)
        goto fail;
    if (created) {
        strcpy(guard->record.state_path, state);
        rc = save(guard, &guard->record);
        if (rc != 0)
            goto fail;
    } else {
        if (fstat(guard->fd, &info) != 0) {
            rc = -errno;
            goto fail;
        }
        if (info.st_size != RECORD_SIZE) {
            rc = -EBADMSG;
            goto fail;
        }
        while (done < sizeof(bytes)) {
            ssize_t received = pread(guard->fd, bytes + done, sizeof(bytes) - done, (off_t)done);
            if (received < 0 && errno == EINTR)
                continue;
            if (received <= 0) {
                rc = received < 0 ? -errno : -EBADMSG;
                goto fail;
            }
            done += (size_t)received;
        }
        rc = decode(bytes, &guard->record);
        if (rc != 0)
            goto fail;
        if (state_path != NULL && strcmp(state, guard->record.state_path) != 0) {
            rc = -EXDEV;
            goto fail;
        }
    }
    /** 也同步已存在文件的目录，覆盖首次创建者同步目录前退出、后继进程接手的窗口 */
    strcpy(parent, guard->path);
    char *slash = strrchr(parent, '/');
    if (slash == parent)
        slash[1] = '\0';
    else
        *slash = '\0';
    directory = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        rc = -errno;
        goto fail;
    }
    rc = fsync(directory) == 0 ? 0 : -errno;
    close(directory);
    if (rc != 0)
        goto fail;
    guard->writable = state_path != NULL && guard->record.count == 0;
    return 0;
fail:
    dppd_recovery_guard_close(guard);
    return rc;
}

/** 每个端口首次尝试前先持久化，之后同一身份可复用标记，软件路径完全不调用这里 */
int dppd_recovery_guard_prepare(struct dppd_recovery_guard *guard, uint16_t port_id,
    const char *device, const char *driver, uint64_t rule_id, uint64_t generation)
{
    struct dppd_recovery_record next;
    struct dppd_recovery_port *port;
    int rc;

    if (guard == NULL || device == NULL || driver == NULL || device[0] == '\0' || driver[0] == '\0' ||
        strlen(device) >= DPPD_RECOVERY_DEVICE_SIZE || strlen(driver) >= DPPD_RECOVERY_DRIVER_SIZE ||
        rule_id == 0 || generation == 0)
        return -EINVAL;
    if (!guard->writable || guard->faulted)
        return -EUCLEAN;
    rc = same_file(guard);
    if (rc != 0) {
        guard->faulted = true;
        return rc;
    }
    for (uint32_t index = 0; index < guard->record.count; ++index) {
        port = &guard->record.ports[index];
        if (port->port_id == port_id)
            return strcmp(port->device, device) == 0 && strcmp(port->driver, driver) == 0 ? 0 : -EXDEV;
    }
    if (guard->record.count == DPPD_MAX_PORTS || guard->record.revision == UINT64_MAX)
        return -EOVERFLOW;
    next = guard->record;
    next.revision++;
    port = &next.ports[next.count++];
    port->port_id = port_id;
    port->first_rule = rule_id;
    port->first_generation = generation;
    strcpy(port->device, device);
    strcpy(port->driver, driver);
    return save(guard, &next);
}

/** 清理确认同样先落盘再报告成功，失败时保留不确定状态，不能让后续安装继续 */
static int clear_record(struct dppd_recovery_guard *guard)
{
    struct dppd_recovery_record next = guard->record;
    if (guard->faulted)
        return -EUCLEAN;
    if (next.count == 0)
        return 0;
    if (next.revision == UINT64_MAX)
        return -EOVERFLOW;
    next.revision++;
    next.count = 0;
    memset(next.ports, 0, sizeof(next.ports));
    return save(guard, &next);
}

/** 上一进程留下的标记不能由本次空 backend 的退出清理冒充已删除 */
int dppd_recovery_guard_clean(struct dppd_recovery_guard *guard)
{
    if (guard == NULL || guard->fd < 0)
        return -EINVAL;
    return guard->writable ? clear_record(guard) : -EUCLEAN;
}

/** 离线工具持锁后还要核对精确版本，确认过的记录不能重复确认或覆盖新一轮安装 */
int dppd_recovery_guard_acknowledge(struct dppd_recovery_guard *guard, uint64_t revision)
{
    if (guard == NULL || guard->fd < 0)
        return -EINVAL;
    if (guard->writable)
        return -EBUSY;
    if (revision != guard->record.revision)
        return -ESTALE;
    if (guard->record.count == 0)
        return -EALREADY;
    return clear_record(guard);
}
