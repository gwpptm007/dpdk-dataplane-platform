#include "dppd/acl.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

/** 将完整 IR 编译为紧凑匹配项，查询时不遍历无效槽位，也不反复解释协议层次 */
struct acl_row {
    uint64_t id;
    uint32_t group, priority, tag, src, src_mask, dst, dst_mask;
    uint16_t ingress, src_first, src_last, dst_first, dst_last;
    uint8_t l4, protocol, protocol_mask, connection_states;
    bool ipv4, range;
};

struct dppd_acl {
    uint32_t count, capacity;
    struct acl_row rows[];
};

size_t dppd_acl_size(uint32_t capacity)
{
    return sizeof(struct dppd_acl) + (size_t)capacity * sizeof(struct acl_row);
}

struct dppd_acl *dppd_acl_init(void *memory, uint32_t capacity)
{
    struct dppd_acl *acl = memory;
    acl->count = 0;
    acl->capacity = capacity;
    return acl;
}

/** 相同 ID 的两个版本短暂并存时延续原槽位次序，避免改变已有单规则事务的语义 */
static bool before(const struct acl_row *a, const struct acl_row *b)
{
    if (a->ingress != b->ingress)
        return a->ingress < b->ingress;
    if (a->group != b->group)
        return a->group < b->group;
    if (a->priority != b->priority)
        return a->priority < b->priority;
    if (a->id != b->id)
        return a->id < b->id;
    return a->tag < b->tag;
}

int dppd_acl_add(struct dppd_acl *acl, const struct dppd_rule *rule, uint32_t tag)
{
    char error[128];
    if (acl == NULL || rule == NULL || tag == 0 || rule->domain != DPPD_RULE_DOMAIN_INGRESS ||
        dppd_rule_validate(rule, error, sizeof(error)) != 0)
        return -EINVAL;
    if (acl->count == acl->capacity)
        return -ENOSPC;
    struct acl_row row = {.id = rule->id,
        .group = rule->group,
        .priority = rule->priority,
        .tag = tag,
        .ingress = rule->install_port_id};
    for (uint16_t i = 0; i < rule->nb_matches; ++i) {
        const struct dppd_match *match = &rule->matches[i];
        switch (match->type) {
        case DPPD_MATCH_ETH:
            break;
        case DPPD_MATCH_IPV4:
            row.ipv4 = true;
            row.src_mask = match->spec.ipv4.src_mask_be;
            row.dst_mask = match->spec.ipv4.dst_mask_be;
            row.src = match->spec.ipv4.src_be & row.src_mask;
            row.dst = match->spec.ipv4.dst_be & row.dst_mask;
            break;
        case DPPD_MATCH_IP_PROTOCOL:
            row.protocol = match->spec.protocol.value;
            row.protocol_mask = match->spec.protocol.mask;
            break;
        case DPPD_MATCH_CONNECTION_STATE:
            row.connection_states = (uint8_t)match->spec.connection_states;
            break;
        case DPPD_MATCH_UDP:
        case DPPD_MATCH_TCP:
            row.l4 = match->type == DPPD_MATCH_UDP ? DPPD_L4_UDP : DPPD_L4_TCP;
            row.src_first = match->spec.l4.src_be;
            row.src_last = match->spec.l4.src_mask_be;
            row.dst_first = match->spec.l4.dst_be;
            row.dst_last = match->spec.l4.dst_mask_be;
            break;
        case DPPD_MATCH_UDP_RANGE:
        case DPPD_MATCH_TCP_RANGE:
            row.l4 = match->type == DPPD_MATCH_UDP_RANGE ? DPPD_L4_UDP : DPPD_L4_TCP;
            row.range = true;
            row.src_first = ntohs(match->spec.range.src_first_be);
            row.src_last = ntohs(match->spec.range.src_last_be);
            row.dst_first = ntohs(match->spec.range.dst_first_be);
            row.dst_last = ntohs(match->spec.range.dst_last_be);
            break;
        default:
            return -ENOTSUP;
        }
    }
    uint32_t position = 0;
    while (position < acl->count && before(&acl->rows[position], &row))
        ++position;
    memmove(acl->rows + position + 1, acl->rows + position, (acl->count - position) * sizeof(row));
    acl->rows[position] = row;
    acl->count++;
    return 0;
}

/** IPv4 地址掩码按原网络字节比较，范围端口只转换一次，分片不能冒充端口为零的 TCP/UDP */
static bool matches(const struct acl_row *row, const struct dppd_packet *packet)
{
    if (row->connection_states != 0 && (row->connection_states & packet->connection_state) == 0)
        return false;
    if (row->ipv4 &&
        (packet->l3_type != DPPD_L3_IPV4 || (packet->ipv4_src_be & row->src_mask) != row->src ||
            (packet->ipv4_dst_be & row->dst_mask) != row->dst))
        return false;
    if ((packet->ip_protocol & row->protocol_mask) != (row->protocol & row->protocol_mask))
        return false;
    if (row->l4 == 0)
        return true;
    if (packet->l4_type != row->l4)
        return false;
    if (!row->range)
        return (packet->l4_src_port_be & row->src_last) == (row->src_first & row->src_last) &&
            (packet->l4_dst_port_be & row->dst_last) == (row->dst_first & row->dst_last);
    uint16_t source = ntohs(packet->l4_src_port_be), destination = ntohs(packet->l4_dst_port_be);
    return source >= row->src_first && source <= row->src_last && destination >= row->dst_first &&
        destination <= row->dst_last;
}

uint32_t dppd_acl_classify(const struct dppd_acl *acl, uint16_t ingress, const struct dppd_packet *packet)
{
    if (acl == NULL || packet == NULL)
        return 0;
    uint32_t first = 0, last = acl->count;
    while (first < last) {
        uint32_t middle = first + (last - first) / 2;
        if (acl->rows[middle].ingress < ingress)
            first = middle + 1;
        else
            last = middle;
    }
    for (; first < acl->count && acl->rows[first].ingress == ingress; ++first)
        if (matches(&acl->rows[first], packet))
            return acl->rows[first].tag;
    return 0;
}
