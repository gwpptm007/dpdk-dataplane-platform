#define _GNU_SOURCE
#include "dppd/persistence.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DPPD_SNAPSHOT_VERSION 2U
#define DPPD_SNAPSHOT_HEADER_SIZE 48U
#define DPPD_SNAPSHOT_RULE_HEADER_SIZE 40U
#define DPPD_SNAPSHOT_MATCH_SIZE 20U
#define DPPD_SNAPSHOT_ACTION_SIZE 8U
#define DPPD_SNAPSHOT_RECORD_SIZE \
    (DPPD_SNAPSHOT_RULE_HEADER_SIZE + \
     DPPD_RULE_MAX_ITEMS * DPPD_SNAPSHOT_MATCH_SIZE + \
     DPPD_RULE_MAX_ACTIONS * DPPD_SNAPSHOT_ACTION_SIZE)
#define DPPD_SNAPSHOT_MAX_RULES 1000000U
#define DPPD_SNAPSHOT_CHECKSUM_OFFSET 40U

static const uint8_t snapshot_magic[8] = {
    'D', 'P', 'P', 'R', 'U', 'L', 'E', '\0',
};

static void put_u16_le(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
}

static void put_u32_le(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
    output[2] = (uint8_t)(value >> 16);
    output[3] = (uint8_t)(value >> 24);
}

static void put_u64_le(uint8_t *output, uint64_t value)
{
    uint8_t i;

    for (i = 0; i < 8; ++i)
        output[i] = (uint8_t)(value >> (8U * i));
}

static uint16_t get_u16_le(const uint8_t *input)
{
    return (uint16_t)input[0] | (uint16_t)((uint16_t)input[1] << 8);
}

static uint32_t get_u32_le(const uint8_t *input)
{
    return (uint32_t)input[0] |
           ((uint32_t)input[1] << 8) |
           ((uint32_t)input[2] << 16) |
           ((uint32_t)input[3] << 24);
}

static uint64_t get_u64_le(const uint8_t *input)
{
    uint64_t value = 0;
    uint8_t i;

    for (i = 0; i < 8; ++i)
        value |= (uint64_t)input[i] << (8U * i);
    return value;
}

/*
 * 使用 CRC32/IEEE 检测截断、部分写入和静默损坏。CRC 不是认证机制；
 * snapshot 文件仍依赖目录权限，不能把它当作不可信输入的安全签名。
 */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length)
{
    size_t i;

    for (i = 0; i < length; ++i) {
        uint8_t bit;

        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return crc;
}

static int encode_rule(const struct dppd_rule *rule, uint8_t *output)
{
    char validation_error[128];
    uint16_t i;

    if (dppd_rule_validate(rule, validation_error,
                           sizeof(validation_error)) != 0 ||
        rule->id == 0 || rule->generation == 0)
        return -EINVAL;
    memset(output, 0, DPPD_SNAPSHOT_RECORD_SIZE);
    put_u64_le(output, rule->id);
    put_u64_le(output + 8, rule->generation);
    put_u32_le(output + 16, (uint32_t)rule->domain);
    put_u32_le(output + 20, (uint32_t)rule->fallback);
    put_u32_le(output + 24, rule->group);
    put_u32_le(output + 28, rule->priority);
    put_u16_le(output + 32, rule->nb_matches);
    put_u16_le(output + 34, rule->nb_actions);
    put_u16_le(output + 36, rule->install_port_id);
    /* 38..39 为 v2 record 保留字段，memset 已保证写出为 0。 */

    for (i = 0; i < rule->nb_matches; ++i) {
        const struct dppd_match *match = &rule->matches[i];
        uint8_t *encoded = output + DPPD_SNAPSHOT_RULE_HEADER_SIZE +
            i * DPPD_SNAPSHOT_MATCH_SIZE;

        put_u32_le(encoded, (uint32_t)match->type);
        /*
         * IP/L4 字段在 IR 中已是网络字节序。这里复制协议字节而不是把它们当作
         * host integer 编码，保证大小端主机读取后仍得到同一网络地址和端口。
         */
        switch (match->type) {
        case DPPD_MATCH_ETH:
            break;
        case DPPD_MATCH_IPV4:
            memcpy(encoded + 4, &match->spec.ipv4.src_be, 4);
            memcpy(encoded + 8, &match->spec.ipv4.src_mask_be, 4);
            memcpy(encoded + 12, &match->spec.ipv4.dst_be, 4);
            memcpy(encoded + 16, &match->spec.ipv4.dst_mask_be, 4);
            break;
        case DPPD_MATCH_UDP:
        case DPPD_MATCH_TCP:
            memcpy(encoded + 4, &match->spec.l4.src_be, 2);
            memcpy(encoded + 6, &match->spec.l4.src_mask_be, 2);
            memcpy(encoded + 8, &match->spec.l4.dst_be, 2);
            memcpy(encoded + 10, &match->spec.l4.dst_mask_be, 2);
            break;
        case DPPD_MATCH_REPRESENTED_PORT:
            put_u16_le(encoded + 4, match->spec.ethdev_port_id);
            break;
        default:
            return -EINVAL;
        }
    }

    for (i = 0; i < rule->nb_actions; ++i) {
        const struct dppd_action *action = &rule->actions[i];
        uint8_t *encoded = output + DPPD_SNAPSHOT_RULE_HEADER_SIZE +
            DPPD_RULE_MAX_ITEMS * DPPD_SNAPSHOT_MATCH_SIZE +
            i * DPPD_SNAPSHOT_ACTION_SIZE;

        put_u32_le(encoded, (uint32_t)action->type);
        switch (action->type) {
        case DPPD_ACTION_DROP:
        case DPPD_ACTION_COUNT:
            break;
        case DPPD_ACTION_QUEUE:
            put_u16_le(encoded + 4, action->conf.queue_id);
            break;
        case DPPD_ACTION_MARK:
            put_u32_le(encoded + 4, action->conf.mark_id);
            break;
        case DPPD_ACTION_REPRESENTED_PORT:
            put_u16_le(encoded + 4, action->conf.ethdev_port_id);
            break;
        default:
            return -EINVAL;
        }
    }
    return 0;
}

static int decode_rule(const uint8_t *input, struct dppd_rule *rule)
{
    char validation_error[128];
    uint16_t i;

    memset(rule, 0, sizeof(*rule));
    rule->id = get_u64_le(input);
    rule->generation = get_u64_le(input + 8);
    rule->domain = (enum dppd_rule_domain)get_u32_le(input + 16);
    rule->fallback = (enum dppd_fallback_policy)get_u32_le(input + 20);
    rule->group = get_u32_le(input + 24);
    rule->priority = get_u32_le(input + 28);
    rule->nb_matches = get_u16_le(input + 32);
    rule->nb_actions = get_u16_le(input + 34);
    rule->install_port_id = get_u16_le(input + 36);
    if (rule->id == 0 || rule->generation == 0 ||
        get_u16_le(input + 38) != 0 ||
        rule->nb_matches > DPPD_RULE_MAX_ITEMS ||
        rule->nb_actions > DPPD_RULE_MAX_ACTIONS)
        return -EBADMSG;

    for (i = 0; i < rule->nb_matches; ++i) {
        struct dppd_match *match = &rule->matches[i];
        const uint8_t *encoded =
            input + DPPD_SNAPSHOT_RULE_HEADER_SIZE +
            i * DPPD_SNAPSHOT_MATCH_SIZE;

        match->type = (enum dppd_match_type)get_u32_le(encoded);
        switch (match->type) {
        case DPPD_MATCH_ETH:
            break;
        case DPPD_MATCH_IPV4:
            memcpy(&match->spec.ipv4.src_be, encoded + 4, 4);
            memcpy(&match->spec.ipv4.src_mask_be, encoded + 8, 4);
            memcpy(&match->spec.ipv4.dst_be, encoded + 12, 4);
            memcpy(&match->spec.ipv4.dst_mask_be, encoded + 16, 4);
            break;
        case DPPD_MATCH_UDP:
        case DPPD_MATCH_TCP:
            memcpy(&match->spec.l4.src_be, encoded + 4, 2);
            memcpy(&match->spec.l4.src_mask_be, encoded + 6, 2);
            memcpy(&match->spec.l4.dst_be, encoded + 8, 2);
            memcpy(&match->spec.l4.dst_mask_be, encoded + 10, 2);
            break;
        case DPPD_MATCH_REPRESENTED_PORT:
            match->spec.ethdev_port_id = get_u16_le(encoded + 4);
            break;
        default:
            return -EBADMSG;
        }
    }

    for (i = 0; i < rule->nb_actions; ++i) {
        struct dppd_action *action = &rule->actions[i];
        const uint8_t *encoded = input + DPPD_SNAPSHOT_RULE_HEADER_SIZE +
            DPPD_RULE_MAX_ITEMS * DPPD_SNAPSHOT_MATCH_SIZE +
            i * DPPD_SNAPSHOT_ACTION_SIZE;

        action->type = (enum dppd_action_type)get_u32_le(encoded);
        switch (action->type) {
        case DPPD_ACTION_DROP:
        case DPPD_ACTION_COUNT:
            break;
        case DPPD_ACTION_QUEUE:
            action->conf.queue_id = get_u16_le(encoded + 4);
            break;
        case DPPD_ACTION_MARK:
            action->conf.mark_id = get_u32_le(encoded + 4);
            break;
        case DPPD_ACTION_REPRESENTED_PORT:
            action->conf.ethdev_port_id = get_u16_le(encoded + 4);
            break;
        default:
            return -EBADMSG;
        }
    }
    if (dppd_rule_validate(rule, validation_error,
                           sizeof(validation_error)) != 0)
        return -EBADMSG;
    return 0;
}

static int write_all(int fd, const uint8_t *data, size_t length)
{
    while (length != 0) {
        ssize_t written = write(fd, data, length);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (written == 0)
            return -EIO;
        data += written;
        length -= (size_t)written;
    }
    return 0;
}

static int read_all(int fd, uint8_t *data, size_t length)
{
    while (length != 0) {
        ssize_t received = read(fd, data, length);

        if (received < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (received == 0)
            return -EBADMSG;
        data += received;
        length -= (size_t)received;
    }
    return 0;
}

static int fsync_parent_directory(const char *path)
{
    char directory[PATH_MAX];
    char *slash;
    int fd;
    int rc;

    if (strlen(path) >= sizeof(directory))
        return -ENAMETOOLONG;
    memcpy(directory, path, strlen(path) + 1U);
    slash = strrchr(directory, '/');
    if (slash == NULL) {
        memcpy(directory, ".", 2);
    } else if (slash == directory) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    rc = fsync(fd) == 0 ? 0 : -errno;
    close(fd);
    return rc;
}

int dppd_persistence_save(const char *path,
                          const struct dppd_rule_repository *repository)
{
    struct dppd_rule *rules = NULL;
    uint8_t header[DPPD_SNAPSHOT_HEADER_SIZE];
    uint8_t *payload = NULL;
    char temporary[PATH_MAX];
    uint64_t generation;
    uint64_t payload_size;
    uint32_t count;
    uint32_t listed = 0;
    uint32_t checksum;
    uint32_t i;
    bool has_more = false;
    int fd = -1;
    int rc;

    if (path == NULL || path[0] == '\0' || repository == NULL ||
        repository->records == NULL)
        return -EINVAL;
    count = dppd_rule_repository_count(repository);
    generation = dppd_rule_repository_generation(repository);
    if (count > DPPD_SNAPSHOT_MAX_RULES)
        return -E2BIG;
    payload_size = (uint64_t)count * DPPD_SNAPSHOT_RECORD_SIZE;
    if (payload_size > SIZE_MAX)
        return -EOVERFLOW;

    if (count != 0) {
        rules = calloc(count, sizeof(*rules));
        payload = calloc(1, (size_t)payload_size);
        if (rules == NULL || payload == NULL) {
            rc = -ENOMEM;
            goto cleanup;
        }
        rc = dppd_rule_repository_list(
            repository, 0, generation, rules, count, &listed, &has_more,
            &generation);
        if (rc != 0)
            goto cleanup;
        if (listed != count || has_more) {
            rc = -EUCLEAN;
            goto cleanup;
        }
        for (i = 0; i < count; ++i) {
            rc = encode_rule(&rules[i],
                             payload + i * DPPD_SNAPSHOT_RECORD_SIZE);
            if (rc != 0)
                goto cleanup;
        }
    }

    memset(header, 0, sizeof(header));
    memcpy(header, snapshot_magic, sizeof(snapshot_magic));
    put_u32_le(header + 8, DPPD_SNAPSHOT_VERSION);
    put_u32_le(header + 12, DPPD_SNAPSHOT_HEADER_SIZE);
    put_u32_le(header + 16, DPPD_SNAPSHOT_RECORD_SIZE);
    put_u32_le(header + 20, count);
    put_u64_le(header + 24, generation);
    put_u64_le(header + 32, payload_size);
    checksum = crc32_update(UINT32_MAX, header, sizeof(header));
    checksum = crc32_update(checksum, payload, (size_t)payload_size) ^ UINT32_MAX;
    put_u32_le(header + DPPD_SNAPSHOT_CHECKSUM_OFFSET, checksum);

    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld",
                 path, (long)getpid()) >= (int)sizeof(temporary)) {
        rc = -ENAMETOOLONG;
        goto cleanup;
    }
    /*
     * O_EXCL 防止覆盖其他进程的临时文件；0600 避免规则内容在 rename 前短暂
     * 暴露。临时文件位于目标同目录，保证 rename 在同一文件系统内原子完成。
     */
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        rc = -errno;
        goto cleanup;
    }
    rc = write_all(fd, header, sizeof(header));
    if (rc == 0 && payload_size != 0)
        rc = write_all(fd, payload, (size_t)payload_size);
    if (rc == 0 && fsync(fd) != 0)
        rc = -errno;
    if (close(fd) != 0 && rc == 0)
        rc = -errno;
    fd = -1;
    if (rc != 0)
        goto unlink_temporary;
    if (rename(temporary, path) != 0) {
        rc = -errno;
        goto unlink_temporary;
    }
    rc = fsync_parent_directory(path);
    goto cleanup;

unlink_temporary:
    unlink(temporary);
cleanup:
    if (fd >= 0)
        close(fd);
    free(payload);
    free(rules);
    return rc;
}

int dppd_persistence_load(const char *path,
                          struct dppd_persisted_snapshot *snapshot)
{
    uint8_t header[DPPD_SNAPSHOT_HEADER_SIZE];
    uint8_t checksum_header[DPPD_SNAPSHOT_HEADER_SIZE];
    uint8_t *payload = NULL;
    struct stat metadata;
    uint64_t generation;
    uint64_t payload_size;
    uint64_t expected_size;
    uint32_t stored_checksum;
    uint32_t calculated_checksum;
    uint32_t count;
    uint32_t i;
    int fd = -1;
    int rc;

    if (path == NULL || path[0] == '\0' || snapshot == NULL)
        return -EINVAL;
    memset(snapshot, 0, sizeof(*snapshot));
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -errno;
    if (fstat(fd, &metadata) != 0) {
        rc = -errno;
        goto cleanup;
    }
    if (!S_ISREG(metadata.st_mode) ||
        metadata.st_size < (off_t)sizeof(header)) {
        rc = -EBADMSG;
        goto cleanup;
    }
    rc = read_all(fd, header, sizeof(header));
    if (rc != 0)
        goto cleanup;
    if (memcmp(header, snapshot_magic, sizeof(snapshot_magic)) != 0) {
        rc = -EBADMSG;
        goto cleanup;
    }
    /*
     * v1 没有 install_port_id，无法无歧义重放。对已识别但不兼容的版本返回
     * EPROTONOSUPPORT，和随机损坏使用的 EBADMSG 明确区分，便于迁移工具诊断。
     */
    if (get_u32_le(header + 8) != DPPD_SNAPSHOT_VERSION) {
        rc = -EPROTONOSUPPORT;
        goto cleanup;
    }
    if (get_u32_le(header + 12) != DPPD_SNAPSHOT_HEADER_SIZE ||
        get_u32_le(header + 16) != DPPD_SNAPSHOT_RECORD_SIZE ||
        get_u32_le(header + 44) != 0) {
        rc = -EBADMSG;
        goto cleanup;
    }
    count = get_u32_le(header + 20);
    generation = get_u64_le(header + 24);
    payload_size = get_u64_le(header + 32);
    if (count > DPPD_SNAPSHOT_MAX_RULES ||
        payload_size != (uint64_t)count * DPPD_SNAPSHOT_RECORD_SIZE) {
        rc = -EBADMSG;
        goto cleanup;
    }
    expected_size = DPPD_SNAPSHOT_HEADER_SIZE + payload_size;
    if (expected_size > INT64_MAX ||
        metadata.st_size != (off_t)expected_size) {
        rc = -EBADMSG;
        goto cleanup;
    }
    if (count != 0) {
        payload = malloc((size_t)payload_size);
        snapshot->rules = calloc(count, sizeof(*snapshot->rules));
        if (payload == NULL || snapshot->rules == NULL) {
            rc = -ENOMEM;
            goto cleanup;
        }
        rc = read_all(fd, payload, (size_t)payload_size);
        if (rc != 0)
            goto cleanup;
    }

    memcpy(checksum_header, header, sizeof(header));
    stored_checksum = get_u32_le(
        checksum_header + DPPD_SNAPSHOT_CHECKSUM_OFFSET);
    put_u32_le(checksum_header + DPPD_SNAPSHOT_CHECKSUM_OFFSET, 0);
    calculated_checksum =
        crc32_update(UINT32_MAX, checksum_header, sizeof(checksum_header));
    calculated_checksum =
        crc32_update(calculated_checksum, payload, (size_t)payload_size) ^
        UINT32_MAX;
    if (stored_checksum != calculated_checksum) {
        rc = -EBADMSG;
        goto cleanup;
    }
    for (i = 0; i < count; ++i) {
        rc = decode_rule(payload + i * DPPD_SNAPSHOT_RECORD_SIZE,
                         &snapshot->rules[i]);
        if (rc != 0)
            goto cleanup;
        if (snapshot->rules[i].generation > generation ||
            (i != 0 && snapshot->rules[i - 1].id >= snapshot->rules[i].id)) {
            rc = -EBADMSG;
            goto cleanup;
        }
    }
    snapshot->count = count;
    snapshot->repository_generation = generation;
    rc = 0;

cleanup:
    if (fd >= 0)
        close(fd);
    free(payload);
    if (rc != 0)
        dppd_persisted_snapshot_destroy(snapshot);
    return rc;
}

void dppd_persisted_snapshot_destroy(struct dppd_persisted_snapshot *snapshot)
{
    if (snapshot == NULL)
        return;
    free(snapshot->rules);
    memset(snapshot, 0, sizeof(*snapshot));
}
