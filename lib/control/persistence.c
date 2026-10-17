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

#define DPPD_SNAPSHOT_V1_VERSION 1U
#define DPPD_SNAPSHOT_V2_VERSION 2U
#define DPPD_SNAPSHOT_V3_VERSION 3U
#define DPPD_SNAPSHOT_V4_VERSION 4U
#define DPPD_SNAPSHOT_V5_VERSION 5U
#define DPPD_SNAPSHOT_V3_MATCH_SIZE 68U
#define DPPD_SNAPSHOT_V3_RECORD_SIZE \
    (40U + DPPD_RULE_MAX_ITEMS * DPPD_SNAPSHOT_V3_MATCH_SIZE + DPPD_RULE_MAX_ACTIONS * 8U)
/** v4 保留 v3 的记录宽度，新增 AGE 的动作类型和完整秒数，不保存运行时计时状态 */
#define DPPD_SNAPSHOT_V4_RECORD_SIZE DPPD_SNAPSHOT_V3_RECORD_SIZE
#define DPPD_SNAPSHOT_HEADER_SIZE 48U
/** v5 扩展头保存共享表修订号，动作同时保留完整的 64 位 ID 和代次 */
#define DPPD_SNAPSHOT_V5_HEADER_SIZE 64U
#define DPPD_SNAPSHOT_V5_ACTION_SIZE 24U
#define DPPD_SNAPSHOT_V5_SHARED_SIZE 32U
#define DPPD_SNAPSHOT_V5_RECORD_SIZE \
    (40U + DPPD_RULE_MAX_ITEMS * DPPD_SNAPSHOT_V3_MATCH_SIZE + DPPD_RULE_MAX_ACTIONS * DPPD_SNAPSHOT_V5_ACTION_SIZE)
#define DPPD_SNAPSHOT_V1_RULE_HEADER_SIZE 36U
#define DPPD_SNAPSHOT_V2_RULE_HEADER_SIZE 40U
#define DPPD_SNAPSHOT_RULE_HEADER_SIZE DPPD_SNAPSHOT_V2_RULE_HEADER_SIZE
#define DPPD_SNAPSHOT_MATCH_SIZE 20U
#define DPPD_SNAPSHOT_ACTION_SIZE 8U
#define DPPD_SNAPSHOT_V1_RECORD_SIZE                                                      \
    (DPPD_SNAPSHOT_V1_RULE_HEADER_SIZE + DPPD_RULE_MAX_ITEMS * DPPD_SNAPSHOT_MATCH_SIZE + \
        DPPD_RULE_MAX_ACTIONS * DPPD_SNAPSHOT_ACTION_SIZE)
#define DPPD_SNAPSHOT_V2_RECORD_SIZE                                                   \
    (DPPD_SNAPSHOT_RULE_HEADER_SIZE + DPPD_RULE_MAX_ITEMS * DPPD_SNAPSHOT_MATCH_SIZE + \
        DPPD_RULE_MAX_ACTIONS * DPPD_SNAPSHOT_ACTION_SIZE)
#define DPPD_SNAPSHOT_RECORD_SIZE DPPD_SNAPSHOT_V2_RECORD_SIZE
#define DPPD_SNAPSHOT_MAX_RULES 1000000U
#define DPPD_SNAPSHOT_MAX_SHARED_COUNTS DPPD_SHARED_COUNT_MAX
#define DPPD_SNAPSHOT_CHECKSUM_OFFSET 40U

static const uint8_t snapshot_magic[8] = {
    'D',
    'P',
    'P',
    'R',
    'U',
    'L',
    'E',
    '\0',
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
    return (uint32_t)input[0] | ((uint32_t)input[1] << 8) | ((uint32_t)input[2] << 16) | ((uint32_t)input[3] << 24);
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
 * snapshot 文件仍依赖目录权限，不能把它当作不可信输入的安全签名
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

static int encode_rule(const struct dppd_rule *rule, uint8_t *output, uint32_t match_size, uint32_t action_size)
{
    char validation_error[128];
    uint16_t i;

    if (dppd_rule_validate(rule, validation_error, sizeof(validation_error)) != 0 || rule->id == 0 ||
        rule->generation == 0)
        return -EINVAL;
    memset(output, 0,
        DPPD_SNAPSHOT_RULE_HEADER_SIZE + DPPD_RULE_MAX_ITEMS * match_size + DPPD_RULE_MAX_ACTIONS * action_size);
    put_u64_le(output, rule->id);
    put_u64_le(output + 8, rule->generation);
    put_u32_le(output + 16, (uint32_t)rule->domain);
    put_u32_le(output + 20, (uint32_t)rule->fallback);
    put_u32_le(output + 24, rule->group);
    put_u32_le(output + 28, rule->priority);
    put_u16_le(output + 32, rule->nb_matches);
    put_u16_le(output + 34, rule->nb_actions);
    put_u16_le(output + 36, rule->install_port_id);
    /* 38..39 为 v2 record 保留字段，memset 已保证写出为 0 */

    for (i = 0; i < rule->nb_matches; ++i) {
        const struct dppd_match *match = &rule->matches[i];
        uint8_t *encoded = output + DPPD_SNAPSHOT_RULE_HEADER_SIZE + i * match_size;

        put_u32_le(encoded, (uint32_t)match->type);
        /*
         * IP/L4 字段在 IR 中已是网络字节序。这里复制协议字节而不是把它们当作
         * host integer 编码，保证大小端主机读取后仍得到同一网络地址和端口
         */
        switch (match->type) {
        case DPPD_MATCH_ETH:
            break;
        case DPPD_MATCH_IPV6:
            if (match_size != DPPD_SNAPSHOT_V3_MATCH_SIZE)
                return -EINVAL;
            memcpy(encoded + 4, &match->spec.ipv6, sizeof(match->spec.ipv6));
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
        case DPPD_MATCH_UDP_RANGE:
        case DPPD_MATCH_TCP_RANGE:
            memcpy(encoded + 4, &match->spec.range.src_first_be, 2);
            memcpy(encoded + 6, &match->spec.range.src_last_be, 2);
            memcpy(encoded + 8, &match->spec.range.dst_first_be, 2);
            memcpy(encoded + 10, &match->spec.range.dst_last_be, 2);
            break;
        case DPPD_MATCH_IP_PROTOCOL:
            encoded[4] = match->spec.protocol.value;
            encoded[5] = match->spec.protocol.mask;
            break;
        case DPPD_MATCH_CONNECTION_STATE:
            put_u32_le(encoded + 4, match->spec.connection_states);
            break;
        default:
            return -EINVAL;
        }
    }

    for (i = 0; i < rule->nb_actions; ++i) {
        const struct dppd_action *action = &rule->actions[i];
        uint8_t *encoded = output + DPPD_SNAPSHOT_RULE_HEADER_SIZE + DPPD_RULE_MAX_ITEMS * match_size + i * action_size;
        uint8_t *configuration = encoded + (action_size == DPPD_SNAPSHOT_V5_ACTION_SIZE ? 8U : 4U);

        put_u32_le(encoded, (uint32_t)action->type);
        switch (action->type) {
        case DPPD_ACTION_DROP:
        case DPPD_ACTION_COUNT:
        case DPPD_ACTION_ALLOW:
            break;
        case DPPD_ACTION_QUEUE:
            put_u16_le(configuration, action->conf.queue_id);
            break;
        case DPPD_ACTION_MARK:
            put_u32_le(configuration, action->conf.mark_id);
            break;
        case DPPD_ACTION_AGE:
            put_u32_le(configuration, action->conf.age_timeout_seconds);
            break;
        case DPPD_ACTION_SHARED_COUNT:
            if (action_size != DPPD_SNAPSHOT_V5_ACTION_SIZE)
                return -EINVAL;
            put_u64_le(configuration, action->conf.shared_count.id);
            put_u64_le(configuration + 8, action->conf.shared_count.generation);
            break;
        case DPPD_ACTION_REPRESENTED_PORT:
            put_u16_le(configuration, action->conf.ethdev_port_id);
            break;
        default:
            return -EINVAL;
        }
    }
    return 0;
}

/** v5 未使用的字节必须为零，避免损坏数据藏在保留位或被忽略的数组槽里 */
static bool bytes_zero(const uint8_t *data, uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i)
        if (data[i] != 0)
            return false;
    return true;
}

static uint32_t encoded_match_size(enum dppd_match_type type)
{
    switch (type) {
    case DPPD_MATCH_ETH:
        return 4U;
    case DPPD_MATCH_IPV4:
        return 20U;
    case DPPD_MATCH_IPV6:
        return 68U;
    case DPPD_MATCH_TCP:
    case DPPD_MATCH_UDP:
    case DPPD_MATCH_TCP_RANGE:
    case DPPD_MATCH_UDP_RANGE:
        return 12U;
    case DPPD_MATCH_REPRESENTED_PORT:
    case DPPD_MATCH_IP_PROTOCOL:
        return 6U;
    case DPPD_MATCH_CONNECTION_STATE:
        return 8U;
    }
    return 0;
}

/*
 * v1 与 v2 的 match/action 编码完全相同，只是 v1 缺少 install_port_id 和
 * 两字节保留位；v3 另将匹配槽扩展为完整 IPv6 地址与掩码
 * v4 沿用 v3 布局但才允许 AGE，避免把伪造降版的动作当成旧格式已有能力
 * v5 将动作槽扩展为 24 字节，保留位和未使用的字节必须保持为零
 * 将布局参数化可保证迁移读取与正常读取共享同一套 IR 校验
 */
static int decode_rule_layout(const uint8_t *input, struct dppd_rule *rule, uint32_t rule_header_size,
    bool contains_install_port, uint32_t match_size, bool supports_age, uint32_t action_size)
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
    if (contains_install_port)
        rule->install_port_id = get_u16_le(input + 36);
    if (rule->id == 0 || rule->generation == 0 || (contains_install_port && get_u16_le(input + 38) != 0) ||
        rule->nb_matches > DPPD_RULE_MAX_ITEMS || rule->nb_actions > DPPD_RULE_MAX_ACTIONS)
        return -EBADMSG;

    for (i = 0; i < rule->nb_matches; ++i) {
        struct dppd_match *match = &rule->matches[i];
        const uint8_t *encoded = input + rule_header_size + i * match_size;

        match->type = (enum dppd_match_type)get_u32_le(encoded);
        switch (match->type) {
        case DPPD_MATCH_ETH:
            break;
        case DPPD_MATCH_IPV6:
            if (match_size != DPPD_SNAPSHOT_V3_MATCH_SIZE)
                return -EBADMSG;
            memcpy(&match->spec.ipv6, encoded + 4, sizeof(match->spec.ipv6));
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
        case DPPD_MATCH_UDP_RANGE:
        case DPPD_MATCH_TCP_RANGE:
            memcpy(&match->spec.range.src_first_be, encoded + 4, 2);
            memcpy(&match->spec.range.src_last_be, encoded + 6, 2);
            memcpy(&match->spec.range.dst_first_be, encoded + 8, 2);
            memcpy(&match->spec.range.dst_last_be, encoded + 10, 2);
            break;
        case DPPD_MATCH_IP_PROTOCOL:
            match->spec.protocol.value = encoded[4];
            match->spec.protocol.mask = encoded[5];
            break;
        case DPPD_MATCH_CONNECTION_STATE:
            match->spec.connection_states = get_u32_le(encoded + 4);
            break;
        default:
            return -EBADMSG;
        }
        if (action_size == DPPD_SNAPSHOT_V5_ACTION_SIZE) {
            uint32_t used = encoded_match_size(match->type);
            if (used == 0 || used > match_size || !bytes_zero(encoded + used, match_size - used))
                return -EBADMSG;
        }
    }

    for (i = 0; i < rule->nb_actions; ++i) {
        struct dppd_action *action = &rule->actions[i];
        const uint8_t *encoded = input + rule_header_size + DPPD_RULE_MAX_ITEMS * match_size + i * action_size;
        bool extended = action_size == DPPD_SNAPSHOT_V5_ACTION_SIZE;
        const uint8_t *configuration = encoded + (extended ? 8U : 4U);
        uint32_t used = 0;

        if (extended && get_u32_le(encoded + 4) != 0)
            return -EBADMSG;

        action->type = (enum dppd_action_type)get_u32_le(encoded);
        switch (action->type) {
        case DPPD_ACTION_DROP:
        case DPPD_ACTION_COUNT:
        case DPPD_ACTION_ALLOW:
            break;
        case DPPD_ACTION_QUEUE:
            action->conf.queue_id = get_u16_le(configuration);
            used = 2U;
            break;
        case DPPD_ACTION_MARK:
            action->conf.mark_id = get_u32_le(configuration);
            used = 4U;
            break;
        case DPPD_ACTION_AGE:
            if (!supports_age)
                return -EBADMSG;
            action->conf.age_timeout_seconds = get_u32_le(configuration);
            used = 4U;
            break;
        case DPPD_ACTION_SHARED_COUNT:
            if (!extended)
                return -EBADMSG;
            action->conf.shared_count.id = get_u64_le(configuration);
            action->conf.shared_count.generation = get_u64_le(configuration + 8);
            used = 16U;
            break;
        case DPPD_ACTION_REPRESENTED_PORT:
            action->conf.ethdev_port_id = get_u16_le(configuration);
            used = 2U;
            break;
        default:
            return -EBADMSG;
        }
        if (extended && !bytes_zero(configuration + used, 16U - used))
            return -EBADMSG;
    }
    if (action_size == DPPD_SNAPSHOT_V5_ACTION_SIZE &&
        (!bytes_zero(input + rule_header_size + rule->nb_matches * match_size,
             (DPPD_RULE_MAX_ITEMS - rule->nb_matches) * match_size) ||
            !bytes_zero(input + rule_header_size + DPPD_RULE_MAX_ITEMS * match_size + rule->nb_actions * action_size,
                (DPPD_RULE_MAX_ACTIONS - rule->nb_actions) * action_size)))
        return -EBADMSG;
    if (dppd_rule_validate(rule, validation_error, sizeof(validation_error)) != 0)
        return -EBADMSG;
    return 0;
}

/** 一次 write 可能只写出部分数据，持续推进指针直到写完，信号中断则重试 */
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

/** 必须读满指定长度才接受记录，提前遇到文件末尾说明快照被截断 */
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

/** 文件内容落盘之后还要同步父目录，确保重命名产生的新文件名也能在重启后保留 */
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

static int compare_shared_ids(const void *left, const void *right)
{
    const struct dppd_shared_count_spec *a = left, *b = right;
    return a->id < b->id ? -1 : a->id > b->id;
}

/** 全局修订号不会因对象删空而归零，旧对象的代次也不能超过这份表的修订号 */
static bool shared_specs_valid(const struct dppd_shared_count_spec *specs, uint32_t count, uint64_t revision)
{
    if (revision == UINT64_MAX || (count != 0 && specs == NULL))
        return false;
    for (uint32_t i = 0; i < count; ++i)
        if (specs[i].id == 0 || specs[i].id == UINT64_MAX || specs[i].generation == 0 ||
            specs[i].generation == UINT64_MAX || specs[i].generation > revision ||
            specs[i].install_port_id == UINT16_MAX || specs[i].domain < DPPD_RULE_DOMAIN_INGRESS ||
            specs[i].domain > DPPD_RULE_DOMAIN_TRANSFER || (i != 0 && specs[i - 1U].id >= specs[i].id))
            return false;
    return true;
}

/** 按稳定 ID 二分查找，不能让大量规则与对象形成逐项笛卡尔扫描 */
static const struct dppd_shared_count_spec *find_shared_spec(const struct dppd_shared_count_spec *specs, uint32_t count,
    uint64_t id)
{
    uint32_t first = 0, last = count;
    while (first < last) {
        uint32_t middle = first + (last - first) / 2U;
        if (specs[middle].id < id)
            first = middle + 1U;
        else
            last = middle;
    }
    return first < count && specs[first].id == id ? &specs[first] : NULL;
}

/** 共享引用必须指向精确代次和相同作用域，不能把重建后的同 ID 对象认成原对象 */
static bool shared_references_valid(const struct dppd_rule *rules, uint32_t count,
    const struct dppd_shared_count_spec *specs, uint32_t shared_count)
{
    for (uint32_t i = 0; i < count; ++i) {
        bool private_count = false, shared_count_seen = false;
        if (rules[i].nb_actions > DPPD_RULE_MAX_ACTIONS)
            return false;
        for (uint16_t action = 0; action < rules[i].nb_actions; ++action) {
            const struct dppd_action *item = &rules[i].actions[action];
            private_count |= item->type == DPPD_ACTION_COUNT;
            if (item->type != DPPD_ACTION_SHARED_COUNT)
                continue;
            const struct dppd_shared_count_spec *spec =
                find_shared_spec(specs, shared_count, item->conf.shared_count.id);
            if (shared_count_seen || spec == NULL || spec->generation != item->conf.shared_count.generation ||
                spec->install_port_id != rules[i].install_port_id || spec->domain != rules[i].domain)
                return false;
            shared_count_seen = true;
        }
        if (private_count && shared_count_seen)
            return false;
    }
    return true;
}

static void encode_shared_spec(const struct dppd_shared_count_spec *spec, uint8_t *output)
{
    memset(output, 0, DPPD_SNAPSHOT_V5_SHARED_SIZE);
    put_u64_le(output, spec->id);
    put_u64_le(output + 8, spec->generation);
    put_u16_le(output + 16, spec->install_port_id);
    put_u32_le(output + 20, (uint32_t)spec->domain);
}

static int decode_shared_spec(const uint8_t *input, struct dppd_shared_count_spec *spec)
{
    if (get_u16_le(input + 18) != 0 || get_u64_le(input + 24) != 0)
        return -EBADMSG;
    *spec = (struct dppd_shared_count_spec){.id = get_u64_le(input),
        .generation = get_u64_le(input + 8),
        .install_port_id = get_u16_le(input + 16),
        .domain = (enum dppd_rule_domain)get_u32_le(input + 20)};
    return 0;
}

/**
 * 先把完整快照写入同目录临时文件，再用重命名替换旧快照
 * 替换之前失败可以保留旧文件，替换之后同步失败仍要报告错误，不能宣称已经可靠保存
 */
int dppd_persistence_save_with_shared(const char *path, const struct dppd_rule_repository *repository,
    const struct dppd_shared_count_spec *specs, uint32_t shared_count, uint64_t shared_revision)
{
    struct dppd_rule *rules = NULL;
    struct dppd_shared_count_spec *shared = NULL;
    uint8_t header[DPPD_SNAPSHOT_V5_HEADER_SIZE];
    uint8_t *payload = NULL;
    char temporary[PATH_MAX];
    uint64_t generation;
    uint64_t payload_size;
    uint32_t count;
    uint32_t match_size = DPPD_SNAPSHOT_MATCH_SIZE, record_size = DPPD_SNAPSHOT_RECORD_SIZE;
    uint32_t action_size = DPPD_SNAPSHOT_ACTION_SIZE, header_size = DPPD_SNAPSHOT_HEADER_SIZE;
    uint32_t version = DPPD_SNAPSHOT_V2_VERSION;
    uint32_t listed = 0;
    uint32_t checksum;
    uint32_t i;
    bool has_more = false;
    int fd = -1;
    int rc;

    if (path == NULL || path[0] == '\0' || repository == NULL || repository->records == NULL ||
        (shared_count != 0 && specs == NULL) || shared_revision == UINT64_MAX)
        return -EINVAL;
    count = dppd_rule_repository_count(repository);
    generation = dppd_rule_repository_generation(repository);
    if (count > DPPD_SNAPSHOT_MAX_RULES || shared_count > DPPD_SNAPSHOT_MAX_SHARED_COUNTS)
        return -E2BIG;
    payload_size = (uint64_t)count * DPPD_SNAPSHOT_RECORD_SIZE;
    if (payload_size > SIZE_MAX)
        return -EOVERFLOW;

    if (shared_count != 0) {
        shared = calloc(shared_count, sizeof(*shared));
        if (shared == NULL) {
            rc = -ENOMEM;
            goto cleanup;
        }
        memcpy(shared, specs, shared_count * sizeof(*shared));
        qsort(shared, shared_count, sizeof(*shared), compare_shared_ids);
    }
    if (!shared_specs_valid(shared, shared_count, shared_revision)) {
        rc = -EINVAL;
        goto cleanup;
    }
    if (shared_revision != 0 || shared_count != 0)
        version = DPPD_SNAPSHOT_V5_VERSION;

    if (count != 0) {
        rules = calloc(count, sizeof(*rules));
        if (rules == NULL) {
            rc = -ENOMEM;
            goto cleanup;
        }
        rc = dppd_rule_repository_list(repository, 0, generation, rules, count, &listed, &has_more, &generation);
        if (rc != 0)
            goto cleanup;
        if (listed != count || has_more) {
            rc = -EUCLEAN;
            goto cleanup;
        }
        /** 仅在需要 AGE 或共享身份时升级格式，普通 IPv4 和 IPv6 保留原版本 */
        for (i = 0; i < count; ++i) {
            for (uint16_t item = 0; item < rules[i].nb_matches; ++item)
                if (rules[i].matches[item].type == DPPD_MATCH_IPV6 && version < DPPD_SNAPSHOT_V3_VERSION)
                    version = DPPD_SNAPSHOT_V3_VERSION;
            for (uint16_t action = 0; action < rules[i].nb_actions; ++action)
                if (rules[i].actions[action].type == DPPD_ACTION_SHARED_COUNT)
                    version = DPPD_SNAPSHOT_V5_VERSION;
                else if (rules[i].actions[action].type == DPPD_ACTION_AGE && version < DPPD_SNAPSHOT_V4_VERSION)
                    version = DPPD_SNAPSHOT_V4_VERSION;
        }
    }
    if (!shared_references_valid(rules, count, shared, shared_count)) {
        rc = -EINVAL;
        goto cleanup;
    }
    if (version >= DPPD_SNAPSHOT_V3_VERSION) {
        match_size = DPPD_SNAPSHOT_V3_MATCH_SIZE;
        record_size = DPPD_SNAPSHOT_V3_RECORD_SIZE;
    }
    if (version == DPPD_SNAPSHOT_V5_VERSION) {
        header_size = DPPD_SNAPSHOT_V5_HEADER_SIZE;
        action_size = DPPD_SNAPSHOT_V5_ACTION_SIZE;
        record_size = DPPD_SNAPSHOT_V5_RECORD_SIZE;
    }
    payload_size = (uint64_t)count * record_size + (uint64_t)shared_count * DPPD_SNAPSHOT_V5_SHARED_SIZE;
    if (payload_size > SIZE_MAX || payload_size > (uint64_t)INT64_MAX - header_size) {
        rc = -EOVERFLOW;
        goto cleanup;
    }
    if (payload_size != 0) {
        payload = calloc(1, (size_t)payload_size);
        if (payload == NULL) {
            rc = -ENOMEM;
            goto cleanup;
        }
        for (i = 0; i < count; ++i) {
            rc = encode_rule(&rules[i], payload + (size_t)i * record_size, match_size, action_size);
            if (rc != 0)
                goto cleanup;
        }
        for (i = 0; i < shared_count; ++i)
            encode_shared_spec(&shared[i],
                payload + (size_t)count * record_size + (size_t)i * DPPD_SNAPSHOT_V5_SHARED_SIZE);
    }

    memset(header, 0, sizeof(header));
    memcpy(header, snapshot_magic, sizeof(snapshot_magic));
    put_u32_le(header + 8, version);
    put_u32_le(header + 12, header_size);
    put_u32_le(header + 16, record_size);
    put_u32_le(header + 20, count);
    put_u64_le(header + 24, generation);
    put_u64_le(header + 32, payload_size);
    if (version == DPPD_SNAPSHOT_V5_VERSION) {
        put_u64_le(header + 48, shared_revision);
        put_u32_le(header + 56, shared_count);
        put_u32_le(header + 60, DPPD_SNAPSHOT_V5_SHARED_SIZE);
    }
    checksum = crc32_update(UINT32_MAX, header, header_size);
    checksum = crc32_update(checksum, payload, (size_t)payload_size) ^ UINT32_MAX;
    put_u32_le(header + DPPD_SNAPSHOT_CHECKSUM_OFFSET, checksum);

    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(temporary)) {
        rc = -ENAMETOOLONG;
        goto cleanup;
    }
    /*
     * O_EXCL 防止覆盖其他进程的临时文件；0600 避免规则内容在 rename 前短暂
     * 暴露。临时文件位于目标同目录，保证 rename 在同一文件系统内原子完成
     */
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        rc = -errno;
        goto cleanup;
    }
    rc = write_all(fd, header, header_size);
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
    free(shared);
    return rc;
}

/** 旧入口没有共享对象表，遇到共享引用时明确失败，不生成丢失引用的半份快照 */
int dppd_persistence_save(const char *path, const struct dppd_rule_repository *repository)
{
    return dppd_persistence_save_with_shared(path, repository, NULL, 0, 0);
}

typedef int (*dppd_snapshot_rule_decoder)(const uint8_t *input, struct dppd_rule *rule);

static int decode_rule_v2(const uint8_t *input, struct dppd_rule *rule)
{
    return decode_rule_layout(input, rule, DPPD_SNAPSHOT_V2_RULE_HEADER_SIZE, true, DPPD_SNAPSHOT_MATCH_SIZE, false,
        DPPD_SNAPSHOT_ACTION_SIZE);
}

static int decode_rule_v3(const uint8_t *input, struct dppd_rule *rule)
{
    return decode_rule_layout(input, rule, DPPD_SNAPSHOT_V2_RULE_HEADER_SIZE, true, DPPD_SNAPSHOT_V3_MATCH_SIZE, false,
        DPPD_SNAPSHOT_ACTION_SIZE);
}

static int decode_rule_v4(const uint8_t *input, struct dppd_rule *rule)
{
    return decode_rule_layout(input, rule, DPPD_SNAPSHOT_V2_RULE_HEADER_SIZE, true, DPPD_SNAPSHOT_V3_MATCH_SIZE, true,
        DPPD_SNAPSHOT_ACTION_SIZE);
}

static int decode_rule_v5(const uint8_t *input, struct dppd_rule *rule)
{
    return decode_rule_layout(input, rule, DPPD_SNAPSHOT_V2_RULE_HEADER_SIZE, true, DPPD_SNAPSHOT_V3_MATCH_SIZE, true,
        DPPD_SNAPSHOT_V5_ACTION_SIZE);
}

static int decode_rule_v1(const uint8_t *input, struct dppd_rule *rule)
{
    return decode_rule_layout(input, rule, DPPD_SNAPSHOT_V1_RULE_HEADER_SIZE, false, DPPD_SNAPSHOT_MATCH_SIZE, false,
        DPPD_SNAPSHOT_ACTION_SIZE);
}

/*
 * 版本选择在这里集中完成：正常启动接受带安装端口的 v2 到 v5，迁移工具才传入 v1。这样不会
 * 因为新增迁移能力而让守护进程误把缺少安装端口的旧快照当成可恢复状态
 */
static int load_snapshot_version(const char *path, struct dppd_persisted_snapshot *snapshot, uint32_t expected_version,
    uint32_t expected_record_size, dppd_snapshot_rule_decoder decode_rule)
{
    uint8_t header[DPPD_SNAPSHOT_V5_HEADER_SIZE] = {0};
    uint8_t checksum_header[DPPD_SNAPSHOT_V5_HEADER_SIZE];
    uint8_t *payload = NULL;
    struct stat metadata;
    uint64_t generation;
    uint64_t payload_size;
    uint64_t expected_size;
    uint64_t shared_revision = 0;
    uint32_t stored_checksum;
    uint32_t calculated_checksum;
    uint32_t count;
    uint32_t shared_count = 0;
    uint32_t header_size = DPPD_SNAPSHOT_HEADER_SIZE;
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
    if (!S_ISREG(metadata.st_mode) || metadata.st_size < (off_t)DPPD_SNAPSHOT_HEADER_SIZE) {
        rc = -EBADMSG;
        goto cleanup;
    }
    rc = read_all(fd, header, DPPD_SNAPSHOT_HEADER_SIZE);
    if (rc != 0)
        goto cleanup;
    if (memcmp(header, snapshot_magic, sizeof(snapshot_magic)) != 0) {
        rc = -EBADMSG;
        goto cleanup;
    }
    /*
     * v1 没有 install_port_id，无法无歧义重放。对已识别但不兼容的版本返回
     * EPROTONOSUPPORT，和随机损坏使用的 EBADMSG 明确区分，便于迁移工具诊断
     */
    if (expected_version == 0) {
        expected_version = get_u32_le(header + 8);
        if (expected_version == DPPD_SNAPSHOT_V2_VERSION) {
            expected_record_size = DPPD_SNAPSHOT_V2_RECORD_SIZE;
            decode_rule = decode_rule_v2;
        } else if (expected_version == DPPD_SNAPSHOT_V3_VERSION) {
            expected_record_size = DPPD_SNAPSHOT_V3_RECORD_SIZE;
            decode_rule = decode_rule_v3;
        } else if (expected_version == DPPD_SNAPSHOT_V4_VERSION) {
            expected_record_size = DPPD_SNAPSHOT_V4_RECORD_SIZE;
            decode_rule = decode_rule_v4;
        } else if (expected_version == DPPD_SNAPSHOT_V5_VERSION) {
            expected_record_size = DPPD_SNAPSHOT_V5_RECORD_SIZE;
            decode_rule = decode_rule_v5;
        } else {
            rc = -EPROTONOSUPPORT;
            goto cleanup;
        }
    }
    if (get_u32_le(header + 8) != expected_version) {
        rc = -EPROTONOSUPPORT;
        goto cleanup;
    }
    if (expected_version == DPPD_SNAPSHOT_V5_VERSION) {
        header_size = DPPD_SNAPSHOT_V5_HEADER_SIZE;
        rc = read_all(fd, header + DPPD_SNAPSHOT_HEADER_SIZE, header_size - DPPD_SNAPSHOT_HEADER_SIZE);
        if (rc != 0)
            goto cleanup;
        shared_revision = get_u64_le(header + 48);
        shared_count = get_u32_le(header + 56);
        if (get_u32_le(header + 60) != DPPD_SNAPSHOT_V5_SHARED_SIZE || shared_count > DPPD_SNAPSHOT_MAX_SHARED_COUNTS ||
            shared_revision == UINT64_MAX) {
            rc = -EBADMSG;
            goto cleanup;
        }
    }
    if (get_u32_le(header + 12) != header_size || get_u32_le(header + 16) != expected_record_size ||
        get_u32_le(header + 44) != 0) {
        rc = -EBADMSG;
        goto cleanup;
    }
    count = get_u32_le(header + 20);
    generation = get_u64_le(header + 24);
    payload_size = get_u64_le(header + 32);
    if (count > DPPD_SNAPSHOT_MAX_RULES ||
        payload_size !=
            (uint64_t)count * expected_record_size + (uint64_t)shared_count * DPPD_SNAPSHOT_V5_SHARED_SIZE ||
        payload_size > SIZE_MAX || payload_size > (uint64_t)INT64_MAX - header_size) {
        rc = -EBADMSG;
        goto cleanup;
    }
    expected_size = header_size + payload_size;
    if (metadata.st_size != (off_t)expected_size) {
        rc = -EBADMSG;
        goto cleanup;
    }
    if (payload_size != 0) {
        payload = malloc((size_t)payload_size);
        if (payload == NULL) {
            rc = -ENOMEM;
            goto cleanup;
        }
        rc = read_all(fd, payload, (size_t)payload_size);
        if (rc != 0)
            goto cleanup;
    }

    if (count != 0) {
        snapshot->rules = calloc(count, sizeof(*snapshot->rules));
        if (snapshot->rules == NULL) {
            rc = -ENOMEM;
            goto cleanup;
        }
    }
    if (shared_count != 0) {
        snapshot->shared_counts = calloc(shared_count, sizeof(*snapshot->shared_counts));
        if (snapshot->shared_counts == NULL) {
            rc = -ENOMEM;
            goto cleanup;
        }
    }

    memcpy(checksum_header, header, header_size);
    stored_checksum = get_u32_le(checksum_header + DPPD_SNAPSHOT_CHECKSUM_OFFSET);
    put_u32_le(checksum_header + DPPD_SNAPSHOT_CHECKSUM_OFFSET, 0);
    calculated_checksum = crc32_update(UINT32_MAX, checksum_header, header_size);
    calculated_checksum = crc32_update(calculated_checksum, payload, (size_t)payload_size) ^ UINT32_MAX;
    if (stored_checksum != calculated_checksum) {
        rc = -EBADMSG;
        goto cleanup;
    }
    for (i = 0; i < count; ++i) {
        rc = decode_rule(payload + (size_t)i * expected_record_size, &snapshot->rules[i]);
        if (rc != 0)
            goto cleanup;
        if (snapshot->rules[i].generation > generation ||
            (i != 0 && snapshot->rules[i - 1].id >= snapshot->rules[i].id)) {
            rc = -EBADMSG;
            goto cleanup;
        }
    }
    for (i = 0; i < shared_count; ++i) {
        rc = decode_shared_spec(payload + (size_t)count * expected_record_size +
                (size_t)i * DPPD_SNAPSHOT_V5_SHARED_SIZE,
            &snapshot->shared_counts[i]);
        if (rc != 0)
            goto cleanup;
    }
    if (!shared_specs_valid(snapshot->shared_counts, shared_count, shared_revision) ||
        !shared_references_valid(snapshot->rules, count, snapshot->shared_counts, shared_count)) {
        rc = -EBADMSG;
        goto cleanup;
    }
    snapshot->count = count;
    snapshot->repository_generation = generation;
    snapshot->shared_count = shared_count;
    snapshot->shared_revision = shared_revision;
    rc = 0;

cleanup:
    if (fd >= 0)
        close(fd);
    free(payload);
    if (rc != 0)
        dppd_persisted_snapshot_destroy(snapshot);
    return rc;
}

int dppd_persistence_load(const char *path, struct dppd_persisted_snapshot *snapshot)
{
    return load_snapshot_version(path, snapshot, 0, 0, NULL);
}

int dppd_persistence_load_v1_for_migration(const char *path, struct dppd_persisted_snapshot *snapshot)
{
    return load_snapshot_version(path, snapshot, DPPD_SNAPSHOT_V1_VERSION, DPPD_SNAPSHOT_V1_RECORD_SIZE,
        decode_rule_v1);
}

void dppd_persisted_snapshot_destroy(struct dppd_persisted_snapshot *snapshot)
{
    if (snapshot == NULL)
        return;
    free(snapshot->rules);
    free(snapshot->shared_counts);
    memset(snapshot, 0, sizeof(*snapshot));
}
