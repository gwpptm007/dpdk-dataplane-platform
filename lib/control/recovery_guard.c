#define _GNU_SOURCE
#include "dppd/recovery_guard.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

/** 固定长度、小端编码、CRC32，坏文件必须阻止启动，不能被解释成空白或已清理 */
#define RECORD_HEADER 32U
#define RECORD_V1_PORT 224U
#define RECORD_PORT 320U
#define RECORD_V1_SIZE (RECORD_HEADER + DPPD_STATE_PATH_CAPACITY + DPPD_MAX_PORTS * RECORD_V1_PORT)
#define RECORD_V2_SIZE (RECORD_HEADER + DPPD_STATE_PATH_CAPACITY + DPPD_MAX_PORTS * RECORD_PORT)
#define RECORD_V3_ATTEMPT 96U
#define RECORD_V3_SIZE (RECORD_V2_SIZE + 32U + DPPD_RECOVERY_ATTEMPT_LIMIT * RECORD_V3_ATTEMPT)
#define RECORD_ATTEMPT 112U
#define RECORD_SIZE (RECORD_V2_SIZE + 32U + DPPD_RECOVERY_ATTEMPT_LIMIT * RECORD_ATTEMPT)
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
static uint32_t checksum(const unsigned char *bytes, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0; index < size; ++index) {
        crc ^= index >= 28 && index < 32 ? 0 : bytes[index];
        for (unsigned int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

/** 每次完整编码都清零保留字段和字符串尾部，便于严格拒绝未知布局 */
static size_t encode(const struct dppd_recovery_record *record, unsigned char *bytes)
{
    size_t size = record->format == 1 ? RECORD_V1_SIZE : record->format == 2 ? RECORD_V2_SIZE :
        record->format == 3 ? RECORD_V3_SIZE : RECORD_SIZE;
    size_t stride = record->format == 1 ? RECORD_V1_PORT : RECORD_PORT;
    memset(bytes, 0, RECORD_SIZE);
    memcpy(bytes, magic, sizeof(magic));
    put(bytes + 8, record->format, 4);
    put(bytes + 12, size, 4);
    put(bytes + 16, record->revision, 8);
    put(bytes + 24, record->count, 4);
    memcpy(bytes + RECORD_HEADER, record->state_path, strlen(record->state_path));
    for (uint32_t index = 0; index < record->count; ++index) {
        const struct dppd_recovery_port *port = &record->ports[index];
        unsigned char *row = bytes + RECORD_HEADER + DPPD_STATE_PATH_CAPACITY + index * stride;
        put(row, port->port_id, 2);
        put(row + 8, port->first_rule, 8);
        put(row + 16, port->first_generation, 8);
        memcpy(row + 24, port->device, strlen(port->device));
        memcpy(row + 152, port->driver, strlen(port->driver));
        if (record->format >= 2 && port->identity.ifindex != 0) {
            put(row + 216, port->identity.ifindex, 4);
            memcpy(row + 220, port->identity.ifname, strlen(port->identity.ifname));
            memcpy(row + 236, port->identity.boot_id, strlen(port->identity.boot_id));
            put(row + 280, port->identity.netns_device, 8);
            put(row + 288, port->identity.netns_inode, 8);
        }
    }
    if (record->format >= 3) {
        put(bytes + RECORD_V2_SIZE, record->last_attempt, 8);
        put(bytes + RECORD_V2_SIZE + 8, record->attempt_count, 4);
        for (uint32_t index = 0; index < record->attempt_count; ++index) {
            const struct dppd_recovery_attempt *attempt = &record->attempts[index];
            unsigned char *row = bytes + RECORD_V2_SIZE + 32 + index *
                (record->format == 3 ? RECORD_V3_ATTEMPT : RECORD_ATTEMPT);
            put(row, attempt->id, 8);
            put(row + 8, attempt->rule_id, 8);
            put(row + 16, attempt->generation, 8);
            put(row + 24, attempt->port_id, 2);
            if (record->format >= 4)
                memcpy(row + 96, attempt->owner_cookie, DPPD_TAP_COOKIE_SIZE);
            row[26] = (unsigned char)attempt->phase;
            row[27] = (unsigned char)attempt->evidence;
            put(row + 28, (uint32_t)attempt->create_error, 4);
            put(row + 32, (uint32_t)attempt->remove_error, 4);
            put(row + 36, (uint32_t)attempt->observation_error, 4);
            if (attempt->evidence == DPPD_EVIDENCE_SINGLE_ADDITION) {
                put(row + 40, attempt->candidate.parent, 4);
                put(row + 44, attempt->candidate.handle, 4);
                put(row + 48, attempt->candidate.chain, 4);
                put(row + 52, attempt->candidate.priority, 2);
                put(row + 54, attempt->candidate.protocol, 2);
                memcpy(row + 56, attempt->candidate.kind, strlen(attempt->candidate.kind));
            }
        }
    }
    put(bytes + 28, checksum(bytes, size), 4);
    return size;
}

/** 非空且终止的字符串才可被日志和离线工具使用，避免坏文件引发越界读取 */
static bool valid_string(const char *value, size_t capacity)
{
    return value[0] != '\0' && memchr(value, '\0', capacity) != NULL;
}

/** 启动标识必须是内核使用的小写 UUID，拒绝长度相同但格式损坏的定位信息 */
static bool valid_boot_id(const char *value)
{
    if (!valid_string(value, DPPD_RECOVERY_BOOT_SIZE) || strlen(value) != 36)
        return false;
    for (size_t index = 0; index < 36; ++index) {
        bool hyphen = index == 8 || index == 13 || index == 18 || index == 23;
        if (hyphen ? value[index] != '-' : !((value[index] >= '0' && value[index] <= '9') ||
            (value[index] >= 'a' && value[index] <= 'f')))
            return false;
    }
    return true;
}

/** 正式接口统一保存 Linux 负 errno，损坏的正数或超出范围值不能混入恢复结论 */
static bool valid_error(int value)
{
    return value <= 0 && value >= -4095;
}

/** 同时校验状态和字段组合，意图未完成时不得带成功证据，删除成功才允许复用槽位 */
static bool valid_attempt(const struct dppd_recovery_attempt *attempt)
{
    if (attempt->id == 0 || attempt->rule_id == 0 || attempt->generation == 0 ||
        attempt->phase < DPPD_RECOVERY_INTENT || attempt->phase > DPPD_RECOVERY_REMOVED ||
        attempt->evidence < DPPD_EVIDENCE_NONE || attempt->evidence > DPPD_EVIDENCE_UNAVAILABLE ||
        !valid_error(attempt->create_error) || !valid_error(attempt->remove_error) ||
        !valid_error(attempt->observation_error))
        return false;
    if ((attempt->phase == DPPD_RECOVERY_CREATED && attempt->create_error != 0) ||
        (attempt->phase == DPPD_RECOVERY_CREATE_FAILED && attempt->create_error == 0) ||
        (attempt->phase == DPPD_RECOVERY_REMOVED && attempt->remove_error != 0))
        return false;
    if (attempt->phase == DPPD_RECOVERY_INTENT && (attempt->create_error != 0 ||
        attempt->remove_error != 0 || attempt->evidence != DPPD_EVIDENCE_NONE))
        return false;
    if ((attempt->evidence == DPPD_EVIDENCE_UNAVAILABLE) != (attempt->observation_error != 0))
        return false;
    return attempt->evidence != DPPD_EVIDENCE_SINGLE_ADDITION ||
        (attempt->candidate.handle != 0 && valid_string(attempt->candidate.kind, sizeof(attempt->candidate.kind)));
}

/** 验证校验和、唯一端口和完整规范编码，预留位或无效尾部也不能静默接受 */
static int decode(const unsigned char *bytes, size_t size, struct dppd_recovery_record *record)
{
    unsigned char canonical[RECORD_SIZE];
    uint32_t format = (uint32_t)get(bytes + 8, 4);
    size_t stride = format == 1 ? RECORD_V1_PORT : RECORD_PORT;

    if (memcmp(bytes, magic, sizeof(magic)) != 0 || (format < 1 || format > 4) ||
        size != (format == 1 ? RECORD_V1_SIZE : format == 2 ? RECORD_V2_SIZE :
            format == 3 ? RECORD_V3_SIZE : RECORD_SIZE) ||
        get(bytes + 12, 4) != size || get(bytes + 28, 4) != checksum(bytes, size))
        return -EBADMSG;
    memset(record, 0, sizeof(*record));
    record->format = format;
    record->revision = get(bytes + 16, 8);
    record->count = (uint32_t)get(bytes + 24, 4);
    memcpy(record->state_path, bytes + RECORD_HEADER, sizeof(record->state_path));
    if (record->count > DPPD_MAX_PORTS || (record->count != 0 && record->revision == 0) ||
        !valid_string(record->state_path, sizeof(record->state_path)) || record->state_path[0] != '/')
        return -EBADMSG;
    for (uint32_t index = 0; index < record->count; ++index) {
        struct dppd_recovery_port *port = &record->ports[index];
        const unsigned char *row = bytes + RECORD_HEADER + DPPD_STATE_PATH_CAPACITY + index * stride;
        port->port_id = (uint16_t)get(row, 2);
        port->first_rule = get(row + 8, 8);
        port->first_generation = get(row + 16, 8);
        memcpy(port->device, row + 24, sizeof(port->device));
        memcpy(port->driver, row + 152, sizeof(port->driver));
        if (!valid_string(port->device, sizeof(port->device)) ||
            !valid_string(port->driver, sizeof(port->driver)) || !port->first_rule || !port->first_generation)
            return -EBADMSG;
        if (format >= 2) {
            struct dppd_recovery_identity *identity = &port->identity;
            identity->ifindex = (uint32_t)get(row + 216, 4);
            if (identity->ifindex != 0) {
                memcpy(identity->ifname, row + 220, sizeof(identity->ifname));
                memcpy(identity->boot_id, row + 236, sizeof(identity->boot_id));
                identity->netns_device = get(row + 280, 8);
                identity->netns_inode = get(row + 288, 8);
                if (identity->ifindex > INT32_MAX || strcmp(port->driver, "net_tap") != 0 ||
                    !valid_string(identity->ifname, sizeof(identity->ifname)) ||
                    !valid_boot_id(identity->boot_id) || identity->netns_inode == 0)
                    return -EBADMSG;
            }
        }
        for (uint32_t prior = 0; prior < index; ++prior)
            if (record->ports[prior].port_id == port->port_id)
                return -EBADMSG;
    }
    if (format >= 3) {
        record->last_attempt = get(bytes + RECORD_V2_SIZE, 8);
        record->attempt_count = (uint32_t)get(bytes + RECORD_V2_SIZE + 8, 4);
        if (record->attempt_count > DPPD_RECOVERY_ATTEMPT_LIMIT ||
            (record->count == 0 && record->attempt_count != 0))
            return -EBADMSG;
        for (uint32_t index = 0; index < record->attempt_count; ++index) {
            struct dppd_recovery_attempt *attempt = &record->attempts[index];
            const unsigned char *row = bytes + RECORD_V2_SIZE + 32 + index *
                (record->format == 3 ? RECORD_V3_ATTEMPT : RECORD_ATTEMPT);
            attempt->id = get(row, 8);
            attempt->rule_id = get(row + 8, 8);
            attempt->generation = get(row + 16, 8);
            attempt->port_id = (uint16_t)get(row + 24, 2);
            if (format >= 4)
                memcpy(attempt->owner_cookie, row + 96, DPPD_TAP_COOKIE_SIZE);
            attempt->phase = row[26];
            attempt->evidence = row[27];
            attempt->create_error = (int32_t)get(row + 28, 4);
            attempt->remove_error = (int32_t)get(row + 32, 4);
            attempt->observation_error = (int32_t)get(row + 36, 4);
            attempt->candidate.parent = (uint32_t)get(row + 40, 4);
            attempt->candidate.handle = (uint32_t)get(row + 44, 4);
            attempt->candidate.chain = (uint32_t)get(row + 48, 4);
            attempt->candidate.priority = (uint16_t)get(row + 52, 2);
            attempt->candidate.protocol = (uint16_t)get(row + 54, 2);
            memcpy(attempt->candidate.kind, row + 56, sizeof(attempt->candidate.kind));
            if (!valid_attempt(attempt) || attempt->id > record->last_attempt)
                return -EBADMSG;
            bool known_port = false;
            for (uint32_t port = 0; port < record->count; ++port)
                if (record->ports[port].port_id == attempt->port_id) {
                    known_port = true;
                    if ((attempt->evidence == DPPD_EVIDENCE_SINGLE_ADDITION ||
                        dppd_tap_cookie_present(attempt->owner_cookie)) &&
                        record->ports[port].identity.ifindex == 0)
                        return -EBADMSG;
                }
            if (!known_port)
                return -EBADMSG;
            for (uint32_t prior = 0; prior < index; ++prior)
                if (record->attempts[prior].id == attempt->id ||
                    (dppd_tap_cookie_present(attempt->owner_cookie) &&
                     memcmp(record->attempts[prior].owner_cookie, attempt->owner_cookie, DPPD_TAP_COOKIE_SIZE) == 0))
                    return -EBADMSG;
        }
    }
    encode(record, canonical);
    return memcmp(bytes, canonical, size) == 0 ? 0 : -EBADMSG;
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
    size_t done = 0, size;
    int rc = same_file(guard);

    if (rc != 0)
        goto fail;
    size = encode(record, bytes);
    while (done < size) {
        ssize_t written = pwrite(guard->fd, bytes + done, size - done, (off_t)done);
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
        guard->record.format = 4;
        strcpy(guard->record.state_path, state);
        rc = save(guard, &guard->record);
        if (rc != 0)
            goto fail;
    } else {
        if (fstat(guard->fd, &info) != 0) {
            rc = -errno;
            goto fail;
        }
        if (info.st_size != RECORD_SIZE && info.st_size != RECORD_V3_SIZE && info.st_size != RECORD_V1_SIZE && info.st_size != RECORD_V2_SIZE) {
            rc = -EBADMSG;
            goto fail;
        }
        while (done < (size_t)info.st_size) {
            ssize_t received = pread(guard->fd, bytes + done, (size_t)info.st_size - done, (off_t)done);
            if (received < 0 && errno == EINTR)
                continue;
            if (received <= 0) {
                rc = received < 0 ? -errno : -EBADMSG;
                goto fail;
            }
            done += (size_t)received;
        }
        rc = decode(bytes, (size_t)info.st_size, &guard->record);
        if (rc != 0)
            goto fail;
        if (state_path != NULL && strcmp(state, guard->record.state_path) != 0) {
            rc = -EXDEV;
            goto fail;
        }
    }
    /** 旧待核对记录保持原格式，只有确认干净的旧文件才允许在新 daemon 启动时升级 */
    if (state_path != NULL && guard->record.format < 4 && guard->record.count == 0) {
        struct dppd_recovery_record upgraded = guard->record;
        if (upgraded.revision == UINT64_MAX) {
            rc = -EOVERFLOW;
            goto fail;
        }
        upgraded.format = 4;
        upgraded.revision++;
        rc = save(guard, &upgraded);
        if (rc != 0)
            goto fail;
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
int dppd_recovery_guard_prepare_identity(struct dppd_recovery_guard *guard, uint16_t port_id,
    const char *device, const char *driver, uint64_t rule_id, uint64_t generation,
    const struct dppd_recovery_identity *identity)
{
    struct dppd_recovery_record next;
    struct dppd_recovery_port *port;
    int rc;

    if (guard == NULL || device == NULL || driver == NULL || device[0] == '\0' || driver[0] == '\0' ||
        strlen(device) >= DPPD_RECOVERY_DEVICE_SIZE || strlen(driver) >= DPPD_RECOVERY_DRIVER_SIZE ||
        rule_id == 0 || generation == 0)
        return -EINVAL;
    if (identity != NULL && (guard->record.format < 2 || strcmp(driver, "net_tap") != 0 ||
        identity->ifindex == 0 || identity->ifindex > INT32_MAX || identity->netns_inode == 0 ||
        !valid_string(identity->ifname, sizeof(identity->ifname)) ||
        !valid_boot_id(identity->boot_id)))
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
        if (port->port_id == port_id) {
            if (strcmp(port->device, device) != 0 || strcmp(port->driver, driver) != 0)
                return -EXDEV;
            if (identity == NULL)
                return port->identity.ifindex == 0 ? 0 : -EXDEV;
            return port->identity.ifindex == identity->ifindex &&
                port->identity.netns_device == identity->netns_device &&
                port->identity.netns_inode == identity->netns_inode &&
                strcmp(port->identity.ifname, identity->ifname) == 0 &&
                strcmp(port->identity.boot_id, identity->boot_id) == 0 ? 0 : -EXDEV;
        }
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
    if (identity != NULL)
        port->identity = *identity;
    return save(guard, &next);
}

/** 不支持内核定位的驱动沿用原保护，空身份必须明确保留为未知 */
int dppd_recovery_guard_prepare(struct dppd_recovery_guard *guard, uint16_t port_id,
    const char *device, const char *driver, uint64_t rule_id, uint64_t generation)
{
    return dppd_recovery_guard_prepare_identity(guard, port_id, device, driver, rule_id, generation, NULL);
}

/**
 * 只使用内核随机源，不以时间、地址或可复用编号代替随机标识
 * 拒绝全零和本记录中的重复值，随机源故障时直接停止创建
 */
static int new_cookie(const struct dppd_recovery_record *record, uint8_t cookie[DPPD_TAP_COOKIE_SIZE])
{
    for (unsigned int retry = 0; retry < 8; ++retry) {
        size_t done = 0;
        while (done < DPPD_TAP_COOKIE_SIZE) {
            ssize_t received = getrandom(cookie + done, DPPD_TAP_COOKIE_SIZE - done, 0);
            if (received < 0 && errno == EINTR)
                continue;
            if (received <= 0)
                return received < 0 ? -errno : -EIO;
            done += (size_t)received;
        }
        bool duplicate = !dppd_tap_cookie_present(cookie);
        for (uint32_t index = 0; index < record->attempt_count; ++index)
            duplicate |= memcmp(cookie, record->attempts[index].owner_cookie, DPPD_TAP_COOKIE_SIZE) == 0;
        if (!duplicate)
            return 0;
    }
    return -EAGAIN;
}

/** 单调编号与意图一起落盘，删除过的槽位可复用，未清理线索满时显式拒绝新安装 */
static int begin_attempt(struct dppd_recovery_guard *guard, uint16_t port_id,
    uint64_t rule_id, uint64_t generation, uint64_t *attempt, uint8_t *cookie)
{
    struct dppd_recovery_record next;
    bool known_port = false, known_tap = false;
    uint32_t slot;
    if (guard == NULL || attempt == NULL || rule_id == 0 || generation == 0)
        return -EINVAL;
    *attempt = 0;
    if (cookie != NULL)
        memset(cookie, 0, DPPD_TAP_COOKIE_SIZE);
    if (!guard->writable || guard->faulted || guard->record.format != 4)
        return -EUCLEAN;
    if (guard->record.last_attempt == UINT64_MAX || guard->record.revision == UINT64_MAX)
        return -EOVERFLOW;
    for (uint32_t index = 0; index < guard->record.count; ++index) {
        const struct dppd_recovery_port *port = &guard->record.ports[index];
        if (port->port_id == port_id) {
            known_port = true;
            known_tap = port->identity.ifindex != 0 && strcmp(port->driver, "net_tap") == 0;
        }
    }
    if (!known_port)
        return -ENOENT;
    if (cookie != NULL && !known_tap)
        return -ENOTSUP;
    for (slot = 0; slot < guard->record.attempt_count; ++slot)
        if (guard->record.attempts[slot].phase == DPPD_RECOVERY_REMOVED)
            break;
    if (slot == DPPD_RECOVERY_ATTEMPT_LIMIT)
        return -ENOSPC;
    next = guard->record;
    if (slot == next.attempt_count)
        next.attempt_count++;
    next.attempts[slot] = (struct dppd_recovery_attempt){.id = ++next.last_attempt,
        .rule_id = rule_id, .generation = generation, .port_id = port_id, .phase = DPPD_RECOVERY_INTENT};
    if (cookie != NULL) {
        int rc = new_cookie(&guard->record, next.attempts[slot].owner_cookie);
        if (rc != 0)
            return rc;
    }
    next.revision++;
    int rc = save(guard, &next);
    if (rc == 0) {
        *attempt = next.last_attempt;
        if (cookie != NULL)
            memcpy(cookie, next.attempts[slot].owner_cookie, DPPD_TAP_COOKIE_SIZE);
    }
    return rc;
}

/** 普通模式保留相同的逐次记录语义，不向驱动传递原生标识 */
int dppd_recovery_guard_begin(struct dppd_recovery_guard *guard, uint16_t port_id,
    uint64_t rule_id, uint64_t generation, uint64_t *attempt)
{
    return begin_attempt(guard, port_id, rule_id, generation, attempt, NULL);
}

/** 成功返回表示随机标识已经与意图一起 fsync，调用方此后才可执行创建 */
int dppd_recovery_guard_begin_owned(struct dppd_recovery_guard *guard, uint16_t port_id,
    uint64_t rule_id, uint64_t generation, uint64_t *attempt, uint8_t cookie[DPPD_TAP_COOKIE_SIZE])
{
    if (cookie == NULL)
        return -EINVAL;
    return begin_attempt(guard, port_id, rule_id, generation, attempt, cookie);
}

/** 更新必须引用本文件仍保留的精确尝试编号，不能凭可能重复的业务 ID 修改另一轮记录 */
static int attempt_slot(const struct dppd_recovery_guard *guard, uint64_t attempt)
{
    if (guard == NULL || attempt == 0)
        return -EINVAL;
    if (!guard->writable || guard->faulted || guard->record.format != 4)
        return -EUCLEAN;
    if (guard->record.revision == UINT64_MAX)
        return -EOVERFLOW;
    for (uint32_t index = 0; index < guard->record.attempt_count; ++index)
        if (guard->record.attempts[index].id == attempt)
            return (int)index;
    return -ENOENT;
}

/** 驱动结果独立于观察结果保存，查询失败不能伪造没有新增规则，也不能改写驱动错误 */
int dppd_recovery_guard_created(struct dppd_recovery_guard *guard, uint64_t attempt, int create_error,
    enum dppd_recovery_evidence evidence, int observation_error, const struct dppd_recovery_filter *candidate)
{
    struct dppd_recovery_record next;
    int slot = attempt_slot(guard, attempt);
    if (slot < 0)
        return slot;
    if (guard->record.attempts[slot].phase != DPPD_RECOVERY_INTENT)
        return -EALREADY;
    next = guard->record;
    struct dppd_recovery_attempt *entry = &next.attempts[slot];
    entry->phase = create_error == 0 ? DPPD_RECOVERY_CREATED : DPPD_RECOVERY_CREATE_FAILED;
    entry->create_error = create_error;
    entry->evidence = evidence;
    entry->observation_error = observation_error;
    if ((candidate != NULL) != (evidence == DPPD_EVIDENCE_SINGLE_ADDITION))
        return -EINVAL;
    if (candidate != NULL) {
        bool known_identity = false;
        for (uint32_t index = 0; index < next.count; ++index)
            known_identity |= next.ports[index].port_id == entry->port_id && next.ports[index].identity.ifindex != 0;
        if (!known_identity)
            return -EINVAL;
        entry->candidate = *candidate;
    }
    if (!valid_attempt(entry))
        return -EINVAL;
    next.revision++;
    return save(guard, &next);
}

/** 驱动删除成功后才标记 removed，失败保留原阶段和最新删除错误，绝不提前复用 */
int dppd_recovery_guard_removed(struct dppd_recovery_guard *guard, uint64_t attempt, int remove_error)
{
    struct dppd_recovery_record next;
    int slot = attempt_slot(guard, attempt);
    if (slot < 0)
        return slot;
    if (!valid_error(remove_error))
        return -EINVAL;
    next = guard->record;
    struct dppd_recovery_attempt *entry = &next.attempts[slot];
    if (entry->phase == DPPD_RECOVERY_REMOVED)
        return -EALREADY;
    /** 创建结果同步失败时磁盘上可能仍为 intent，faulted 已阻止此处覆盖不确定状态 */
    if (entry->phase == DPPD_RECOVERY_INTENT)
        return -EINVAL;
    entry->remove_error = remove_error;
    if (remove_error == 0)
        entry->phase = DPPD_RECOVERY_REMOVED;
    next.revision++;
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
    next.attempt_count = 0;
    memset(next.attempts, 0, sizeof(next.attempts));
    return save(guard, &next);
}

/** 上一进程留下的标记不能由本次空 backend 的退出清理冒充已删除 */
int dppd_recovery_guard_clean(struct dppd_recovery_guard *guard)
{
    if (guard == NULL || guard->fd < 0)
        return -EINVAL;
    /** 没有取得可删除 handle 的失败创建也可能留有对象，只有显式外部确认能解除 */
    for (uint32_t index = 0; index < guard->record.attempt_count; ++index)
        if (guard->record.attempts[index].phase != DPPD_RECOVERY_REMOVED)
            return -EUCLEAN;
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
