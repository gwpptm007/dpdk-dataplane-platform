#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "dppd/acl.h"
#include "dppd/offload.h"

static uint32_t random_state = 0x5ac1907b;
static uint32_t next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

/** 参考解释器直接读取 IR，不复用编译字段、排序函数或 ACL 查询，便于发现两边语义偏差 */
static bool reference_match(const struct dppd_rule *rule, uint16_t port, const struct dppd_packet *packet)
{
    if (rule->install_port_id != port)
        return false;
    for (uint16_t i = 0; i < rule->nb_matches; ++i) {
        const struct dppd_match *m = &rule->matches[i];
        if (m->type == DPPD_MATCH_ETH)
            continue;
        if (m->type == DPPD_MATCH_IPV4) {
            if (packet->l3_type != DPPD_L3_IPV4 ||
                ((packet->ipv4_src_be ^ m->spec.ipv4.src_be) & m->spec.ipv4.src_mask_be) != 0 ||
                ((packet->ipv4_dst_be ^ m->spec.ipv4.dst_be) & m->spec.ipv4.dst_mask_be) != 0)
                return false;
        } else if (m->type == DPPD_MATCH_IP_PROTOCOL) {
            if (((packet->ip_protocol ^ m->spec.protocol.value) & m->spec.protocol.mask) != 0)
                return false;
        } else {
            bool udp = m->type == DPPD_MATCH_UDP || m->type == DPPD_MATCH_UDP_RANGE;
            if (packet->l4_type != (udp ? DPPD_L4_UDP : DPPD_L4_TCP))
                return false;
            if (m->type == DPPD_MATCH_UDP || m->type == DPPD_MATCH_TCP) {
                if (((packet->l4_src_port_be ^ m->spec.l4.src_be) & m->spec.l4.src_mask_be) != 0 ||
                    ((packet->l4_dst_port_be ^ m->spec.l4.dst_be) & m->spec.l4.dst_mask_be) != 0)
                    return false;
            } else if (ntohs(packet->l4_src_port_be) < ntohs(m->spec.range.src_first_be) ||
                ntohs(packet->l4_src_port_be) > ntohs(m->spec.range.src_last_be) ||
                ntohs(packet->l4_dst_port_be) < ntohs(m->spec.range.dst_first_be) ||
                ntohs(packet->l4_dst_port_be) > ntohs(m->spec.range.dst_last_be))
                return false;
        }
    }
    return true;
}

/** 只在真正相等的排序键上保留先出现的槽位，与既有软件规则语义一致 */
static uint32_t reference(const struct dppd_rule *rules, uint32_t count, uint16_t port,
    const struct dppd_packet *packet)
{
    uint32_t selected = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!reference_match(&rules[i], port, packet))
            continue;
        const struct dppd_rule *old = selected == 0 ? NULL : &rules[selected - 1], *current = &rules[i];
        if (old == NULL || current->group < old->group ||
            (current->group == old->group && current->priority < old->priority) ||
            (current->group == old->group && current->priority == old->priority && current->id < old->id))
            selected = i + 1;
    }
    return selected;
}

static struct dppd_rule rule(uint64_t id)
{
    struct dppd_rule value = {.id = id,
        .generation = 1,
        .install_port_id = 3,
        .domain = DPPD_RULE_DOMAIN_INGRESS,
        .fallback = DPPD_FALLBACK_SOFTWARE_ONLY,
        .nb_matches = 3,
        .nb_actions = 1};
    value.matches[0].type = DPPD_MATCH_ETH;
    value.matches[1].type = DPPD_MATCH_IPV4;
    value.matches[2].type = DPPD_MATCH_UDP_RANGE;
    value.matches[2].spec.range.src_first_be = htons(1000);
    value.matches[2].spec.range.src_last_be = htons(2000);
    value.matches[2].spec.range.dst_first_be = htons(80);
    value.matches[2].spec.range.dst_last_be = htons(90);
    value.actions[0].type = DPPD_ACTION_ALLOW;
    return value;
}

static void test_boundaries(void)
{
    struct dppd_acl *acl = malloc(dppd_acl_size(2));
    assert(acl != NULL);
    dppd_acl_init(acl, 2);
    struct dppd_rule value = rule(1);
    assert(dppd_acl_add(acl, &value, 1) == 0);
    struct dppd_packet packet = {.l3_type = DPPD_L3_IPV4, .l4_type = DPPD_L4_UDP, .ip_protocol = 17};
    const uint16_t sources[] = {0, 999, 1000, 1001, 1999, 2000, 2001, 65535};
    const uint16_t destinations[] = {0, 79, 80, 81, 89, 90, 91, 65535};
    for (size_t i = 0; i < sizeof(sources) / sizeof(*sources); ++i)
        for (size_t j = 0; j < sizeof(destinations) / sizeof(*destinations); ++j) {
            packet.l4_src_port_be = htons(sources[i]);
            packet.l4_dst_port_be = htons(destinations[j]);
            assert(dppd_acl_classify(acl, 3, &packet) == reference(&value, 1, 3, &packet));
            assert(dppd_acl_classify(acl, 2, &packet) == 0);
        }
    packet.l4_src_port_be = htons(1000);
    packet.l4_dst_port_be = htons(80);
    packet.l4_type = DPPD_L4_FRAGMENT;
    assert(dppd_acl_classify(acl, 3, &packet) == 0);
    value.matches[2].type = DPPD_MATCH_IP_PROTOCOL;
    value.matches[2].spec.protocol.value = 17;
    value.matches[2].spec.protocol.mask = 255;
    assert(dppd_acl_add(acl, &value, 2) == 0);
    assert(dppd_acl_classify(acl, 3, &packet) == 2);
    packet.l3_type = DPPD_L3_ARP;
    assert(dppd_acl_classify(acl, 3, &packet) == 0);
    assert(dppd_acl_add(acl, &value, 3) == -ENOSPC);
    dppd_acl_init(acl, 2);
    value = rule(1);
    value.matches[2].spec.range.dst_first_be = htons(100);
    assert(dppd_acl_add(acl, &value, 1) == -EINVAL);
    value = rule(1);
    /** 新增语义不能假装已有硬件等价实现，必须在调用 PMD 前明确拒绝 */
    value.fallback = DPPD_FALLBACK_REQUIRE_HARDWARE;
    struct dppd_flow_error flow_error;
    assert(dppd_flow_validate(3, &value, &flow_error) == -ENOTSUP);
    value.nb_matches = 2;
    assert(dppd_flow_validate(3, &value, &flow_error) == -ENOTSUP);
    value = rule(1);
    value.actions[0].type = DPPD_ACTION_QUEUE;
    value.nb_actions = 2;
    value.actions[1].type = DPPD_ACTION_ALLOW;
    assert(dppd_acl_add(acl, &value, 1) == -EINVAL);
    free(acl);
}

/** 同时覆盖连续和非连续掩码、范围、协议、不同入口及排序平局，规则数超过单个缓存行 */
static void test_reference(void)
{
    enum { COUNT = 256 };
    struct dppd_rule rules[COUNT];
    struct dppd_acl *acl = malloc(dppd_acl_size(COUNT));
    assert(acl != NULL);
    dppd_acl_init(acl, COUNT);
    for (uint32_t i = 0; i < COUNT; ++i) {
        rules[i] = rule(i + 1);
        rules[i].install_port_id = (uint16_t)(next_random() % 4);
        rules[i].group = next_random() % 3;
        rules[i].priority = i % 2 == 0 ? UINT32_MAX : next_random() % 5;
        struct dppd_match *ip = &rules[i].matches[1], *l4 = &rules[i].matches[2];
        ip->spec.ipv4.src_be = next_random();
        ip->spec.ipv4.dst_be = next_random();
        ip->spec.ipv4.src_mask_be = next_random() & next_random();
        ip->spec.ipv4.dst_mask_be = next_random() & next_random();
        if (i % 3 == 0) {
            l4->type = i % 2 == 0 ? DPPD_MATCH_UDP : DPPD_MATCH_TCP;
            l4->spec.l4 = (struct dppd_match_l4){.src_be = (uint16_t)next_random(),
                .src_mask_be = (uint16_t)next_random(),
                .dst_be = (uint16_t)next_random(),
                .dst_mask_be = (uint16_t)next_random()};
        } else if (i % 3 == 1) {
            l4->type = i % 2 == 0 ? DPPD_MATCH_UDP_RANGE : DPPD_MATCH_TCP_RANGE;
            l4->spec.range.src_first_be = htons((uint16_t)(next_random() % 32768));
            l4->spec.range.src_last_be = htons((uint16_t)(32768 + next_random() % 32768));
            l4->spec.range.dst_first_be = htons((uint16_t)(next_random() % 32768));
            l4->spec.range.dst_last_be = htons((uint16_t)(32768 + next_random() % 32768));
        } else {
            l4->type = DPPD_MATCH_IP_PROTOCOL;
            l4->spec.protocol.value = (uint8_t)next_random();
            l4->spec.protocol.mask = (uint8_t)next_random();
        }
        if (i % 7 == 0)
            rules[i].nb_matches = 1;
        assert(dppd_acl_add(acl, &rules[i], i + 1) == 0);
    }
    uint32_t matched = 0;
    for (unsigned int sample = 0; sample < 30000; ++sample) {
        const struct dppd_rule *basis = &rules[next_random() % COUNT];
        struct dppd_packet packet = {.l3_type = DPPD_L3_IPV4,
            .l4_type = (enum dppd_l4_type)(next_random() % 5),
            .ip_protocol = (uint8_t)next_random(),
            .ipv4_src_be = next_random(),
            .ipv4_dst_be = next_random(),
            .l4_src_port_be = (uint16_t)next_random(),
            .l4_dst_port_be = (uint16_t)next_random()};
        uint16_t port = (uint16_t)(next_random() % 6);
        if (sample % 2 == 0) {
            packet.ipv4_src_be = basis->matches[1].spec.ipv4.src_be;
            packet.ipv4_dst_be = basis->matches[1].spec.ipv4.dst_be;
            port = basis->install_port_id;
        }
        uint32_t expected = reference(rules, COUNT, port, &packet);
        assert(dppd_acl_classify(acl, port, &packet) == expected);
        matched += expected != 0;
    }
    assert(matched > 10000);
    free(acl);
}

int main(void)
{
    test_boundaries();
    test_reference();
    return 0;
}
