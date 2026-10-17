#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "dppd/exact.h"

static uint32_t seed = 0x6c24187b;
static uint32_t next_random(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

/** 直接解释 IR 原字段，既不使用哈希键，也不调用编译表的排序或匹配函数 */
static uint32_t reference(const struct dppd_rule *rules, uint32_t count, uint16_t port,
    const struct dppd_packet *packet)
{
    uint32_t best = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const struct dppd_rule *rule = &rules[i];
        const struct dppd_match *ip = &rule->matches[1], *l4 = &rule->matches[2];
        if (rule->install_port_id != port ||
            packet->l4_type != (l4->type == DPPD_MATCH_TCP ? DPPD_L4_TCP : DPPD_L4_UDP) ||
            packet->l4_src_port_be != l4->spec.l4.src_be || packet->l4_dst_port_be != l4->spec.l4.dst_be)
            continue;
        if (ip->type == DPPD_MATCH_IPV4) {
            if (packet->l3_type != DPPD_L3_IPV4 || packet->ipv4_src_be != ip->spec.ipv4.src_be ||
                packet->ipv4_dst_be != ip->spec.ipv4.dst_be)
                continue;
        } else if (packet->l3_type != DPPD_L3_IPV6 || memcmp(packet->ipv6_src, ip->spec.ipv6.src, 16) != 0 ||
            memcmp(packet->ipv6_dst, ip->spec.ipv6.dst, 16) != 0)
            continue;
        const struct dppd_rule *old = best == 0 ? NULL : &rules[best - 1];
        if (old == NULL || rule->group < old->group || (rule->group == old->group && rule->priority < old->priority) ||
            (rule->group == old->group && rule->priority == old->priority && rule->id < old->id))
            best = i + 1;
    }
    return best;
}

static struct dppd_packet from_rule(const struct dppd_rule *rule)
{
    struct dppd_packet packet = {.l4_type = rule->matches[2].type == DPPD_MATCH_TCP ? DPPD_L4_TCP : DPPD_L4_UDP,
        .l4_src_port_be = rule->matches[2].spec.l4.src_be,
        .l4_dst_port_be = rule->matches[2].spec.l4.dst_be};
    if (rule->matches[1].type == DPPD_MATCH_IPV4) {
        packet.l3_type = DPPD_L3_IPV4;
        packet.ipv4_src_be = rule->matches[1].spec.ipv4.src_be;
        packet.ipv4_dst_be = rule->matches[1].spec.ipv4.dst_be;
    } else {
        packet.l3_type = DPPD_L3_IPV6;
        memcpy(packet.ipv6_src, rule->matches[1].spec.ipv6.src, 16);
        memcpy(packet.ipv6_dst, rule->matches[1].spec.ipv6.dst, 16);
    }
    return packet;
}

int main(void)
{
    enum { COUNT = 129 };
    struct dppd_rule rules[COUNT];
    struct dppd_exact *table = dppd_exact_init(calloc(1, dppd_exact_size(COUNT)), COUNT);
    assert(table != NULL);
    for (unsigned int i = 0; i < COUNT; ++i) {
        struct dppd_rule rule = {.id = next_random() % 8 + 1,
            .generation = i + 1,
            .install_port_id = (uint16_t)(next_random() % 4),
            .domain = DPPD_RULE_DOMAIN_INGRESS,
            .fallback = DPPD_FALLBACK_SOFTWARE_ONLY,
            .group = next_random() % 3,
            .priority = next_random() % 5,
            .nb_matches = 3,
            .nb_actions = 1};
        rule.matches[0].type = DPPD_MATCH_ETH;
        rule.actions[0].type = DPPD_ACTION_ALLOW;
        rule.matches[2].type = i % 2 == 0 ? DPPD_MATCH_TCP : DPPD_MATCH_UDP;
        rule.matches[2].spec.l4 =
            (struct dppd_match_l4){(uint16_t)next_random(), UINT16_MAX, (uint16_t)next_random(), UINT16_MAX};
        if (i % 4 < 2) {
            rule.matches[1].type = DPPD_MATCH_IPV4;
            rule.matches[1].spec.ipv4 = (struct dppd_match_ipv4){next_random(), UINT32_MAX, next_random(), UINT32_MAX};
        } else {
            rule.matches[1].type = DPPD_MATCH_IPV6;
            for (unsigned int byte = 0; byte < 16; ++byte) {
                rule.matches[1].spec.ipv6.src[byte] = (uint8_t)next_random();
                rule.matches[1].spec.ipv6.dst[byte] = (uint8_t)next_random();
                rule.matches[1].spec.ipv6.src_mask[byte] = 255;
                rule.matches[1].spec.ipv6.dst_mask[byte] = 255;
            }
        }
        /** 同键不同排序条件及完全同排序键都参与比较，满表自然包含哈希冲突 */
        if (i >= 8 && i % 3 == 0) {
            uint64_t id = rule.id;
            uint32_t group = rule.group, priority = rule.priority;
            rule = rules[i - 8];
            rule.id = id;
            rule.group = group;
            rule.priority = priority;
        }
        rules[i] = rule;
        assert(dppd_exact_add(table, &rule, i + 1) == 0);
    }
    unsigned int hits = 0;
    for (unsigned int sample = 0; sample < 40000; ++sample) {
        const struct dppd_rule *basis = &rules[next_random() % COUNT];
        struct dppd_packet packet = from_rule(basis);
        uint16_t port = basis->install_port_id;
        switch (sample % 7) {
        case 0:
            port++;
            break;
        case 1:
            packet.l4_type = DPPD_L4_FRAGMENT;
            break;
        case 2:
            packet.l4_dst_port_be ^= 1;
            break;
        case 3:
            packet.l3_type = packet.l3_type == DPPD_L3_IPV4 ? DPPD_L3_IPV6 : DPPD_L3_IPV4;
            break;
        case 4:
            packet.ipv4_dst_be ^= 1;
            packet.ipv6_dst[15] ^= 1;
            break;
        default:
            break;
        }
        uint32_t expected = reference(rules, COUNT, port, &packet);
        assert(dppd_exact_classify(table, port, &packet) == expected);
        hits += expected != 0;
    }
    assert(hits > 10000);
    assert(dppd_exact_add(table, &rules[0], COUNT + 1) == -ENOSPC);
    struct dppd_rule partial = rules[0];
    partial.matches[1].spec.ipv4.src_mask_be = 0;
    assert(dppd_exact_add(table, &partial, 1) == -ENOTSUP);
    partial = rules[0];
    partial.matches[2].spec.l4.dst_mask_be = 0;
    assert(dppd_exact_add(table, &partial, 1) == -ENOTSUP);
    partial = rules[0];
    partial.nb_matches = 4;
    partial.matches[3].type = DPPD_MATCH_CONNECTION_STATE;
    partial.matches[3].spec.connection_states = DPPD_CT_NEW;
    assert(dppd_exact_add(table, &partial, 1) == -ENOTSUP);
    assert(dppd_exact_add(table, &rules[0], 0) == -EINVAL);
    free(table);
    table = dppd_exact_init(calloc(1, dppd_exact_size(0)), 0);
    struct dppd_packet packet = from_rule(&rules[0]);
    assert(dppd_exact_add(table, &rules[0], 1) == -ENOSPC);
    assert(dppd_exact_classify(table, 0, &packet) == 0);
    free(table);
    return 0;
}
