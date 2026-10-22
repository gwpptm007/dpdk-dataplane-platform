#include "dppd/acl.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

/** 由配置规则编译出的单条 ACL 匹配项，保存筛选条件和排序信息，动作通过 tag 查找 */
struct acl_row {
    /** 规则 ID，group 和 priority 相同时用于排序 */
    uint64_t id;
    /** 规则组和组内优先级，匹配同一入口时依次比较，数值小的优先 */
    uint32_t group, priority;
    /** 规则对象下标加一，命中后交给软件后端查找动作，也用于最终排序 */
    uint32_t tag;
    /** IPv4 源地址及掩码，保留网络字节序，src 已预先与 src_mask 按位与 */
    uint32_t src, src_mask;
    /** IPv4 目的地址及掩码，保留网络字节序，dst 已预先与 dst_mask 按位与 */
    uint32_t dst, dst_mask;
    /** 规则适用的入口网口 ID，用于定位本入口的规则段 */
    uint16_t ingress;
    /**
     * 源/目的端口条件，含义由 range 决定
     * false：first 为目标值，last 为掩码，均为网络字节序
     * true：first 为下界，last 为上界，均为主机字节序，包含两个端点
     */
    uint16_t src_first, src_last, dst_first, dst_last;
    /** 传输层分类，使用 DPPD_L4_UDP/TCP；0 表示不限制传输层类型和端口 */
    uint8_t l4;
    /** IP 协议号及掩码，如 TCP 为 6、UDP 为 17；掩码为 0 表示不限制 */
    uint8_t protocol, protocol_mask;
    /** 允许的连接状态位集合，0 表示不限制，非零时须与报文状态有交集 */
    uint8_t connection_states;
    /** 是否启用对应地址族的匹配，为真时同时校验报文类型和地址条件 */
    bool ipv4, ipv6;
    /** 端口是否采用范围匹配，为假时采用掩码匹配 */
    bool range;
    /** IPv6 源/目的地址及各自掩码，每个地址或掩码占 16 字节 */
    struct dppd_match_ipv6 address6;
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
        case DPPD_MATCH_IPV6:
            row.ipv6 = true;
            row.address6 = match->spec.ipv6;
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

/**
 * 检查当前报文是否满足一条 ACL 规则，入口已由调用方筛选
 * 依次检查连接状态、IP 类型/地址/协议及传输层条件，任一不符返回 false
 * 满足全部条件返回 true，仅表示规则命中，不代表允许转发
 * row: 待匹配的 ACL 规则行
 * packet: 待匹配的报文
 * 返回: 若报文满足规则返回 true，否则返回 false
 * 返回 false 表示这条规则不匹配，外面继续检查下一条规则；并不表示丢包
 */
static bool matches(const struct acl_row *row, const struct dppd_packet *packet)
{
    /** 状态条件为 0 时不限，否则报文状态必须属于规则允许的状态集合 */
    if (row->connection_states != 0 && (row->connection_states & packet->connection_state) == 0)
        return false;
    /**
     * 规则地址已在 dppd_acl_add 中执行 地址 & 掩码，报文地址用同一掩码后比较
     * 掩码全 0 → 地址不限；全 1 → 完整地址相等；其余仅比较掩码指定的位
     * 地址不限仍须满足 IPv4 类型，地址和掩码均按网络字节序直接做位运算
     */
    if (row->ipv4 &&
        (packet->l3_type != DPPD_L3_IPV4 || (packet->ipv4_src_be & row->src_mask) != row->src ||
            (packet->ipv4_dst_be & row->dst_mask) != row->dst))
        return false;
    /** IP 协议号按掩码比较，掩码为 0 时不限，为 0xff 时要求协议号完全相等 */
    if ((packet->ip_protocol & row->protocol_mask) != (row->protocol & row->protocol_mask))
        return false;
    /** 仅配置 IPv6 地址条件时进入，按字节比较源/目的地址中掩码指定的位 */
    if (row->ipv6) {
        if (packet->l3_type != DPPD_L3_IPV6)
            return false;
        for (unsigned int i = 0; i < 16; ++i)
            if ((packet->ipv6_src[i] & row->address6.src_mask[i]) !=
                    (row->address6.src[i] & row->address6.src_mask[i]) ||
                (packet->ipv6_dst[i] & row->address6.dst_mask[i]) != (row->address6.dst[i] & row->address6.dst_mask[i]))
                return false;
    }
    /**
     * 前面的条件已通过，未配置传输层条件即可命中，否则先核对 UDP/TCP 类型
     * 分片分类为 FRAGMENT，无法通过 UDP/TCP 类型检查，不会用空端口误匹配
     */
    if (row->l4 == 0)
        return true;
    if (packet->l4_type != row->l4)
        return false;
    /**
     * 掩码模式：first 保存目标端口，last 保存掩码，均保留网络字节序
     * 掩码为 0 时该端口不限，为 0xffff 时要求端口完全相等，源和目的条件须同时满足
     * R1 示例：源端口掩码为 0，目的端口值为 9000 且掩码为 0xffff
     */
    if (!row->range)
        return (packet->l4_src_port_be & row->src_last) == (row->src_first & row->src_last) &&
            (packet->l4_dst_port_be & row->dst_last) == (row->dst_first & row->dst_last);
    /** 范围模式：转为主机字节序后与预存上下界比较，源和目的范围均包含两个端点 */
    uint16_t source = ntohs(packet->l4_src_port_be), destination = ntohs(packet->l4_dst_port_be);
    return source >= row->src_first && source <= row->src_last && destination >= row->dst_first &&
        destination <= row->dst_last;
}

/**
 * 在 ACL 表中匹配入口和报文字段，支持掩码、端口范围及连接状态等条件
 * 二分定位入口对应的规则段，再按优先顺序检查，返回首条命中规则的 tag
 * 未命中或参数无效返回 0，仅查找候选，不执行规则动作
 */
uint32_t dppd_acl_classify(const struct dppd_acl *acl, uint16_t ingress, const struct dppd_packet *packet)
{
    if (acl == NULL || packet == NULL)
        return 0;
    /**
     * 写入时先按 ingress 排序，同入口规则连续存放，内部按规则优先顺序排列
     * 在 [first, last) 内二分，找到首个 ingress >= 目标入口的位置
     * 入口示例：[0, 0, 1, 1, 1, 3]，查入口 1 → first = 2
     */
    uint32_t first = 0, last = acl->count;
    while (first < last) {
        uint32_t middle = first + (last - first) / 2;
        if (acl->rows[middle].ingress < ingress)
            first = middle + 1;
        else
            last = middle;
    }
    /** 只遍历目标入口的规则段，首条命中即返回，不再检查后续规则 */
    for (; first < acl->count && acl->rows[first].ingress == ingress; ++first)
        if (matches(&acl->rows[first], packet))
            return acl->rows[first].tag;
    return 0;
}
