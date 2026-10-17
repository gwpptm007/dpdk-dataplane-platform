#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include "dppd/conntrack.h"

static void address(uint8_t output[16], const char *text)
{
    assert(inet_pton(AF_INET6, text, output) == 1);
}

static struct dppd_packet packet(void)
{
    struct dppd_packet value = {.packet_len = 128,
        .l2_len = 14,
        .l3_len = 40,
        .l4_len = 8,
        .ipv6_payload_len = 74,
        .l3_type = DPPD_L3_IPV6,
        .l4_type = DPPD_L4_UDP,
        .ip_protocol = 17,
        .l4_src_port_be = htons(1234),
        .l4_dst_port_be = htons(443)};
    address(value.ipv6_src, "fd00:1::1");
    address(value.ipv6_dst, "2001:db8:2::7");
    return value;
}

static struct dppd_packet reverse(const struct dppd_flow_tuple *tuple)
{
    struct dppd_packet value = packet();
    memcpy(value.ipv6_src, tuple->destination6, 16);
    memcpy(value.ipv6_dst, tuple->source6, 16);
    value.l4_src_port_be = tuple->destination_port_be;
    value.l4_dst_port_be = tuple->source_port_be;
    return value;
}

/** 静态端口与动态池按协议分别保留，两个地址族可以同时使用同一个数字端口 */
static void reservations(void)
{
    struct dppd_snat_config snat = {.enabled = true,
        .inside_port = 0,
        .outside_port = 1,
        .first_port = 40000,
        .last_port = 40001,
        .address_be = htonl(0xcb007109)};
    struct dppd_snat_config snat6 = snat;
    snat6.ipv6 = true;
    address(snat6.address6, "2001:db8:100::9");
    struct dppd_dnat_rule rule = {.ipv6 = true,
        .outside_port = 1,
        .inside_port = 0,
        .protocol = 17,
        .public_first = 40000,
        .public_last = 40000,
        .private_first = 8080};
    memcpy(rule.public_address6, snat6.address6, 16);
    address(rule.private_address6, "fd00:1::8");
    rule.public_last = 40001;
    assert(dppd_nat_validate(&snat6, &rule, 1) == -ENOSPC);
    rule.public_last = 40000;
    struct dppd_conntrack *table = NULL;
    assert(dppd_conntrack_create_dual(&table, 8, 2, &snat, &snat6, &rule, 1) == 0);
    struct dppd_packet value = packet();
    struct dppd_connection_attempt attempt;
    assert(dppd_conntrack_begin(table, 0, &value, 1, &attempt) == 0 && attempt.nat);
    assert(ntohs(attempt.rewrite.source_port_be) == 40001);
    assert(memcmp(attempt.policy.source6, value.ipv6_src, 16) == 0);
    assert(memcmp(attempt.rewrite.source6, snat6.address6, 16) == 0);
    struct dppd_packet reply = reverse(&attempt.rewrite);
    assert(dppd_conntrack_finish(&attempt, 1, false) == 0);
    assert(dppd_conntrack_begin(table, 0, &value, 2, &attempt) == 0);
    assert(dppd_conntrack_finish(&attempt, 1, true) == 0);
    assert(dppd_conntrack_begin(table, 1, &reply, 3, &attempt) == 0 && attempt.reply);
    assert(attempt.state == DPPD_CT_ESTABLISHED);
    assert(memcmp(attempt.rewrite.destination6, value.ipv6_src, 16) == 0);
    assert(attempt.rewrite.destination_port_be == value.l4_src_port_be);
    assert(dppd_conntrack_finish(&attempt, 0, true) == 0);
    reply.ipv6_src[0] ^= 1;
    assert(dppd_conntrack_begin(table, 1, &reply, 4, &attempt) == -ENOENT);
    reply.ipv6_src[0] ^= 1;
    assert(dppd_conntrack_begin(table, 2, &reply, 4, &attempt) == -ENOENT);
    value.l4_src_port_be++;
    assert(dppd_conntrack_begin(table, 0, &value, 5, &attempt) == -EADDRNOTAVAIL);
    struct dppd_packet tcp = value;
    tcp.l4_type = DPPD_L4_TCP;
    tcp.ip_protocol = 6;
    tcp.tcp_flags = 2;
    tcp.l4_len = tcp.ipv6_payload_len = 20;
    assert(dppd_conntrack_begin(table, 0, &tcp, 5, &attempt) == 0);
    assert(ntohs(attempt.rewrite.source_port_be) == 40000);
    assert(dppd_conntrack_finish(&attempt, 1, false) == 0);
    /** IPv4 使用自己的端口零号组，不受 IPv6 的静态保留和动态分配影响 */
    struct dppd_packet ipv4 = value;
    ipv4.l3_type = DPPD_L3_IPV4;
    ipv4.ipv4_src_be = htonl(0x0a000001);
    ipv4.ipv4_dst_be = htonl(0xc6336407);
    assert(dppd_conntrack_begin(table, 0, &ipv4, 6, &attempt) == 0);
    assert(ntohs(attempt.rewrite.source_port_be) == 40000 && !attempt.rewrite.ipv6);
    assert(dppd_conntrack_finish(&attempt, 1, true) == 0);
    struct dppd_conntrack_status status;
    dppd_conntrack_status(table, 6, &status);
    assert(status.active == 2 && status.snat6.enabled);
    assert(dppd_conntrack_flush(table, status.revision - 1) == -ESTALE);
    assert(dppd_conntrack_flush(table, status.revision) == 0);
    assert(dppd_conntrack_begin(table, 0, &value, 7, &attempt) == 0);
    assert(ntohs(attempt.rewrite.source_port_be) == 40001);
    assert(dppd_conntrack_finish(&attempt, 2, true) == -EXDEV);
    assert(dppd_conntrack_begin(table, 0, &value, 8, &attempt) == 0);
    assert(dppd_conntrack_finish(&attempt, 1, true) == 0);
    dppd_conntrack_status(table, 2010, &status);
    assert(status.active == 0 && status.expired == 1);
    assert(dppd_conntrack_begin(table, 1, &reply, 2011, &attempt) == -ENOENT);
    dppd_conntrack_destroy(table);
}

/** DNAT 按相同偏移映射端口，ACL 看后端逻辑地址，回包恢复公网身份 */
static void destination_nat(void)
{
    struct dppd_dnat_rule rules[2] = {{.ipv6 = true,
        .outside_port = 1,
        .inside_port = 0,
        .protocol = 17,
        .public_first = 8443,
        .public_last = 8445,
        .private_first = 443}};
    address(rules[0].public_address6, "2001:db8:100::9");
    address(rules[0].private_address6, "fd00:1::8");
    rules[1] = rules[0];
    assert(dppd_nat_validate(NULL, rules, 2) == -EEXIST);
    address(rules[1].public_address6, "2001:db8:101::9");
    assert(dppd_nat_validate(NULL, rules, 2) == -EEXIST);
    address(rules[1].private_address6, "fd00:2::8");
    assert(dppd_nat_validate(NULL, rules, 2) == 0);
    struct dppd_conntrack *table = NULL;
    assert(dppd_conntrack_create_dual(&table, 8, 2, NULL, NULL, rules, 2) == 0);
    for (uint16_t i = 0; i < 3; ++i) {
        struct dppd_packet inbound = packet();
        address(inbound.ipv6_src, "2001:db8:2::7");
        memcpy(inbound.ipv6_dst, rules[0].public_address6, 16);
        inbound.l4_dst_port_be = htons((uint16_t)(8443 + i));
        struct dppd_connection_attempt attempt;
        assert(dppd_conntrack_begin(table, 0, &inbound, 10, &attempt) == -EXDEV);
        assert(dppd_conntrack_begin(table, 1, &inbound, 10, &attempt) == 0 && attempt.nat);
        assert(memcmp(attempt.policy.destination6, rules[0].private_address6, 16) == 0);
        assert(ntohs(attempt.rewrite.destination_port_be) == 443 + i);
        struct dppd_packet reply = reverse(&attempt.rewrite);
        assert(dppd_conntrack_finish(&attempt, 2, true) == -EXDEV);
        assert(dppd_conntrack_begin(table, 1, &inbound, 11, &attempt) == 0);
        assert(dppd_conntrack_finish(&attempt, 0, true) == 0);
        assert(dppd_conntrack_begin(table, 0, &reply, 12, &attempt) == 0 && attempt.reply);
        assert(memcmp(attempt.rewrite.source6, inbound.ipv6_dst, 16) == 0);
        assert(attempt.rewrite.source_port_be == inbound.l4_dst_port_be);
        assert(dppd_conntrack_finish(&attempt, 1, true) == 0);
    }
    struct dppd_connection_page page;
    assert(dppd_conntrack_list(table, 12, 0, 0, &page) == 0 && page.count == 3);
    assert(page.entries[0].dnat_rule_id == 1 && page.entries[0].original.ipv6);
    struct dppd_dnat_page static_page;
    assert(dppd_conntrack_dnat_list(table, 0, &static_page) == 0 && static_page.count == 2);
    assert(static_page.entries[1].rule.ipv6);
    dppd_conntrack_destroy(table);
}

/** 改变完整地址中的每个有效位，不能只把最后四字节拿来索引 IPv6 会话 */
static void full_addresses(void)
{
    struct dppd_conntrack *table = NULL;
    assert(dppd_conntrack_create(&table, 129, 60, NULL, NULL, 0) == 0);
    for (unsigned int bit = 0; bit < 128; ++bit) {
        struct dppd_packet value = packet();
        address(value.ipv6_src, "2001:db8:1::1");
        value.ipv6_src[bit / 8] ^= (uint8_t)(1U << (bit % 8));
        struct dppd_connection_attempt attempt;
        assert(dppd_conntrack_begin(table, 0, &value, 1, &attempt) == 0 && attempt.state == DPPD_CT_NEW);
        struct dppd_packet reply = reverse(&attempt.rewrite);
        assert(dppd_conntrack_finish(&attempt, 1, true) == 0);
        assert(dppd_conntrack_begin(table, 1, &reply, 2, &attempt) == 0 && attempt.reply);
        assert(dppd_conntrack_finish(&attempt, 0, true) == 0);
    }
    struct dppd_conntrack_status status;
    dppd_conntrack_status(table, 2, &status);
    assert(status.active == 128 && status.created == 128);
    struct dppd_packet unsupported = packet();
    unsupported.ipv6_unsupported = true;
    unsupported.l4_type = DPPD_L4_OTHER;
    struct dppd_connection_attempt attempt;
    assert(dppd_conntrack_begin(table, 0, &unsupported, 3, &attempt) == -ENOTSUP && attempt.owner == NULL);
    dppd_conntrack_destroy(table);
}

int main(void)
{
    reservations();
    destination_nat();
    full_addresses();
    return 0;
}
