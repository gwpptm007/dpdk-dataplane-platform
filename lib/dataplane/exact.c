#include "dppd/exact.h"

#include <errno.h>
#include <string.h>

/** 键逐字段写入固定字节数组，避免结构体填充或主机字节序影响哈希结果 */
#define EXACT_KEY_SIZE 40U

struct exact_row {
    uint8_t key[EXACT_KEY_SIZE];
    uint64_t id;
    uint32_t group, priority, tag, next;
};

struct dppd_exact {
    uint32_t capacity, count;
    struct exact_row rows[];
};

/** 桶数组放在行数组之后，每个桶保存行号加一，零表示没有候选 */
static uint32_t *buckets(const struct dppd_exact *table)
{
    return (uint32_t *)(table->rows + table->capacity);
}

size_t dppd_exact_size(uint32_t capacity)
{
    size_t bytes = sizeof(struct dppd_exact) + (size_t)capacity * (sizeof(struct exact_row) + sizeof(uint32_t));
    return (bytes + 7U) & ~(size_t)7U;
}

struct dppd_exact *dppd_exact_init(void *memory, uint32_t capacity)
{
    struct dppd_exact *table = memory;
    memset(memory, 0, dppd_exact_size(capacity));
    table->capacity = capacity;
    return table;
}

static uint32_t hash_key(const uint8_t *key)
{
    uint32_t hash = 2166136261U;
    for (unsigned int i = 0; i < EXACT_KEY_SIZE; ++i)
        hash = (hash ^ key[i]) * 16777619U;
    return hash;
}

static bool before(const struct exact_row *a, const struct exact_row *b)
{
    if (a->group != b->group)
        return a->group < b->group;
    if (a->priority != b->priority)
        return a->priority < b->priority;
    if (a->id != b->id)
        return a->id < b->id;
    return a->tag < b->tag;
}

/** 入口保留两个字节，地址族和传输层各占一个字节，地址区统一预留 IPv6 宽度 */
static void key_prefix(uint8_t *key, uint16_t ingress, uint8_t family, uint8_t transport)
{
    key[0] = (uint8_t)(ingress >> 8);
    key[1] = (uint8_t)ingress;
    key[2] = family;
    key[3] = transport;
}

int dppd_exact_add(struct dppd_exact *table, const struct dppd_rule *rule, uint32_t tag)
{
    char error[128];
    if (table == NULL || rule == NULL || tag == 0 || rule->domain != DPPD_RULE_DOMAIN_INGRESS ||
        dppd_rule_validate(rule, error, sizeof(error)) != 0)
        return -EINVAL;
    struct exact_row row = {.id = rule->id, .group = rule->group, .priority = rule->priority, .tag = tag};
    uint8_t family = 0, transport = 0;
    for (uint16_t i = 0; i < rule->nb_matches; ++i) {
        const struct dppd_match *match = &rule->matches[i];
        switch (match->type) {
        case DPPD_MATCH_ETH:
            break;
        case DPPD_MATCH_IPV4:
            if (match->spec.ipv4.src_mask_be != UINT32_MAX || match->spec.ipv4.dst_mask_be != UINT32_MAX)
                return -ENOTSUP;
            family = DPPD_L3_IPV4;
            memcpy(row.key + 4, &match->spec.ipv4.src_be, 4);
            memcpy(row.key + 20, &match->spec.ipv4.dst_be, 4);
            break;
        case DPPD_MATCH_IPV6:
            for (unsigned int byte = 0; byte < 16; ++byte)
                if (match->spec.ipv6.src_mask[byte] != 255 || match->spec.ipv6.dst_mask[byte] != 255)
                    return -ENOTSUP;
            family = DPPD_L3_IPV6;
            memcpy(row.key + 4, match->spec.ipv6.src, 16);
            memcpy(row.key + 20, match->spec.ipv6.dst, 16);
            break;
        case DPPD_MATCH_TCP:
        case DPPD_MATCH_UDP:
            if (match->spec.l4.src_mask_be != UINT16_MAX || match->spec.l4.dst_mask_be != UINT16_MAX)
                return -ENOTSUP;
            transport = match->type == DPPD_MATCH_TCP ? DPPD_L4_TCP : DPPD_L4_UDP;
            memcpy(row.key + 36, &match->spec.l4.src_be, 2);
            memcpy(row.key + 38, &match->spec.l4.dst_be, 2);
            break;
        default:
            return -ENOTSUP;
        }
    }
    if (family == 0 || transport == 0)
        return -ENOTSUP;
    if (table->count == table->capacity)
        return -ENOSPC;
    key_prefix(row.key, rule->install_port_id, family, transport);
    uint32_t bucket = hash_key(row.key) % table->capacity;
    row.next = buckets(table)[bucket];
    table->rows[table->count] = row;
    buckets(table)[bucket] = ++table->count;
    return 0;
}

uint32_t dppd_exact_classify(const struct dppd_exact *table, uint16_t ingress, const struct dppd_packet *packet)
{
    if (table == NULL || table->capacity == 0 || packet == NULL ||
        (packet->l3_type != DPPD_L3_IPV4 && packet->l3_type != DPPD_L3_IPV6) ||
        (packet->l4_type != DPPD_L4_TCP && packet->l4_type != DPPD_L4_UDP))
        return 0;
    uint8_t key[EXACT_KEY_SIZE] = {0};
    key_prefix(key, ingress, (uint8_t)packet->l3_type, (uint8_t)packet->l4_type);
    if (packet->l3_type == DPPD_L3_IPV4) {
        memcpy(key + 4, &packet->ipv4_src_be, 4);
        memcpy(key + 20, &packet->ipv4_dst_be, 4);
    } else {
        memcpy(key + 4, packet->ipv6_src, 16);
        memcpy(key + 20, packet->ipv6_dst, 16);
    }
    memcpy(key + 36, &packet->l4_src_port_be, 2);
    memcpy(key + 38, &packet->l4_dst_port_be, 2);
    const struct exact_row *selected = NULL;
    for (uint32_t index = buckets(table)[hash_key(key) % table->capacity]; index != 0;) {
        const struct exact_row *row = &table->rows[index - 1];
        if (memcmp(key, row->key, sizeof(key)) == 0 && (selected == NULL || before(row, selected)))
            selected = row;
        index = row->next;
    }
    return selected == NULL ? 0 : selected->tag;
}
