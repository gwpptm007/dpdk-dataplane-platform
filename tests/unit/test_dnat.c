#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include "dppd/conntrack.h"

static struct dppd_dnat_rule rule(void)
{
    return (struct dppd_dnat_rule){.outside_port = 1,
        .inside_port = 0,
        .protocol = 17,
        .public_address_be = htonl(0xcb007109),
        .private_address_be = htonl(0x0a000009),
        .public_first = 40000,
        .public_last = 40001,
        .private_first = 8080};
}

static struct dppd_packet packet(bool reply, uint16_t port)
{
    return (struct dppd_packet){.l3_type = DPPD_L3_IPV4,
        .l4_type = DPPD_L4_UDP,
        .ip_protocol = 17,
        .l3_len = 20,
        .l4_len = 8,
        .ipv4_total_len = 28,
        .packet_len = 64,
        .ipv4_src_be = htonl(reply ? 0x0a000009 : 0xc6336407),
        .ipv4_dst_be = htonl(reply ? 0xc6336407 : 0xcb007109),
        .l4_src_port_be = htons(reply ? port : 1234),
        .l4_dst_port_be = htons(reply ? 1234 : port)};
}

/** 两端的端口按偏移对应，ACL 看内网服务，回包恢复公网源地址而不改远端身份 */
static void test_forwarding(void)
{
    struct dppd_dnat_rule dnat = rule();
    struct dppd_conntrack *table = NULL;
    assert(dppd_conntrack_create(&table, 2, 2, NULL, &dnat, 1) == 0);
    struct dppd_connection_attempt attempt;
    struct dppd_packet value = packet(false, 40000);
    assert(dppd_conntrack_begin(table, 0, &value, 1, &attempt) == -EXDEV);
    assert(dppd_conntrack_begin(table, 2, &value, 1, &attempt) == -EXDEV);
    assert(dppd_conntrack_begin(table, 1, &value, 2, &attempt) == 0);
    assert(attempt.nat && !attempt.reply && attempt.state == DPPD_CT_NEW);
    assert(
        attempt.policy.destination_be == dnat.private_address_be && attempt.policy.destination_port_be == htons(8080));
    assert(attempt.rewrite.source_be == value.ipv4_src_be && attempt.rewrite.source_port_be == value.l4_src_port_be);
    assert(dppd_conntrack_finish(&attempt, 2, true) == -EXDEV);
    assert(dppd_conntrack_begin(table, 1, &value, 3, &attempt) == 0);
    assert(dppd_conntrack_finish(&attempt, 0, false) == 0);
    struct dppd_conntrack_status status;
    dppd_conntrack_status(table, 3, &status);
    assert(status.active == 0 && status.created == 0 && status.dnat_count == 1);
    for (uint16_t offset = 0; offset < 2; ++offset) {
        value = packet(false, (uint16_t)(40000 + offset));
        assert(dppd_conntrack_begin(table, 1, &value, 10, &attempt) == 0);
        assert(attempt.rewrite.destination_port_be == htons((uint16_t)(8080 + offset)));
        assert(dppd_conntrack_finish(&attempt, 0, true) == 0);
        value = packet(true, (uint16_t)(8080 + offset));
        assert(dppd_conntrack_begin(table, 0, &value, 20, &attempt) == 0);
        assert(attempt.reply && attempt.state == DPPD_CT_ESTABLISHED);
        assert(attempt.policy.source_be == dnat.private_address_be &&
            attempt.policy.source_port_be == value.l4_src_port_be);
        assert(attempt.rewrite.source_be == dnat.public_address_be &&
            attempt.rewrite.source_port_be == htons((uint16_t)(40000 + offset)));
        assert(attempt.rewrite.destination_be == value.ipv4_dst_be);
        assert(dppd_conntrack_finish(&attempt, 1, true) == 0);
    }
    value = packet(false, 40000);
    value.l4_src_port_be = htons(2345);
    assert(dppd_conntrack_begin(table, 1, &value, 30, &attempt) == -ENOSPC);
    struct dppd_connection_page page;
    assert(dppd_conntrack_list(table, 30, 0, 0, &page) == 0 && page.count == 2);
    assert(page.entries[0].dnat_rule_id == 1 && page.entries[0].packets[1] == 1);
    /** 接收方端口、客户端地址或入口不匹配时不能命中已有映射 */
    value = packet(true, 8082);
    assert(dppd_conntrack_begin(table, 0, &value, 40, &attempt) == -ENOENT);
    value = packet(true, 8080);
    value.ipv4_dst_be++;
    assert(dppd_conntrack_begin(table, 0, &value, 40, &attempt) == -ENOENT);
    dppd_conntrack_gc(table, 2020, 2);
    value = packet(true, 8080);
    assert(dppd_conntrack_begin(table, 0, &value, 2021, &attempt) == -ENOENT);
    value = packet(false, 40000);
    assert(dppd_conntrack_begin(table, 1, &value, 2022, &attempt) == 0);
    assert(dppd_conntrack_finish(&attempt, 0, true) == 0);
    dppd_conntrack_destroy(table);
}

/** 静态服务不占动态池，取消、清空和过期都不能解除静态保留，TCP 与 UDP 仍各自独立 */
static void test_snat_reservations(void)
{
    struct dppd_dnat_rule dnat = rule();
    struct dppd_snat_config snat = {.enabled = true,
        .inside_port = 0,
        .outside_port = 1,
        .address_be = dnat.public_address_be,
        .first_port = 40000,
        .last_port = 40002};
    struct dppd_conntrack *table = NULL;
    snat.last_port = 40001;
    assert(dppd_conntrack_create(&table, 8, 2, &snat, &dnat, 1) == -ENOSPC && table == NULL);
    snat.last_port = 40002;
    assert(dppd_conntrack_create(&table, 8, 2, &snat, &dnat, 1) == 0);
    for (unsigned int round = 0; round < 3; ++round) {
        struct dppd_connection_attempt attempt;
        uint64_t now = round * 3000;
        struct dppd_packet inbound = packet(false, 40000);
        inbound.l4_src_port_be = htons(40000);
        assert(dppd_conntrack_begin(table, 1, &inbound, now, &attempt) == 0);
        assert(dppd_conntrack_finish(&attempt, 0, round != 0) == 0);
        struct dppd_packet value = packet(true, 8080);
        if (round == 0)
            assert(dppd_conntrack_begin(table, 0, &value, now, &attempt) == -ENOENT);
        value = packet(true, 1234);
        value.ipv4_src_be = htonl(0x0a000001);
        assert(dppd_conntrack_begin(table, 0, &value, now, &attempt) == 0);
        assert(attempt.rewrite.source_port_be == htons(40002));
        assert(dppd_conntrack_finish(&attempt, 1, true) == 0);
        value.l4_src_port_be = htons(1235);
        assert(dppd_conntrack_begin(table, 0, &value, now, &attempt) == -EADDRNOTAVAIL);
        value.l4_type = DPPD_L4_TCP;
        value.ip_protocol = 6;
        value.l4_len = 20;
        value.ipv4_total_len = 40;
        value.tcp_flags = 2;
        assert(dppd_conntrack_begin(table, 0, &value, now, &attempt) == 0);
        assert(attempt.rewrite.source_port_be == htons((uint16_t)(40000 + round)));
        assert(dppd_conntrack_finish(&attempt, 1, true) == 0);
        if (round == 0) {
            struct dppd_conntrack_status status;
            dppd_conntrack_status(table, now, &status);
            assert(dppd_conntrack_flush(table, status.revision) == 0);
        } else
            dppd_conntrack_gc(table, now + 2000, 8);
    }
    /** 客户端源端口碰巧等于另一个动态映射时，取消 DNAT 也不能释放别人的端口 */
    struct dppd_connection_attempt attempt;
    struct dppd_packet outbound = packet(true, 1234);
    outbound.ipv4_src_be = htonl(0x0a000001);
    assert(dppd_conntrack_begin(table, 0, &outbound, 9000, &attempt) == 0);
    assert(attempt.rewrite.source_port_be == htons(40002));
    assert(dppd_conntrack_finish(&attempt, 1, true) == 0);
    struct dppd_packet inbound = packet(false, 40000);
    inbound.l4_src_port_be = htons(40002);
    assert(dppd_conntrack_begin(table, 1, &inbound, 9000, &attempt) == 0);
    assert(dppd_conntrack_finish(&attempt, 0, false) == 0);
    outbound.l4_src_port_be = htons(1235);
    assert(dppd_conntrack_begin(table, 0, &outbound, 9000, &attempt) == -EADDRNOTAVAIL);
    dppd_conntrack_destroy(table);
}

/** 相同公网服务可以出现在不同外口，查找不能被排在前面的另一个入口错误拦住 */
static void test_multiple_outside_ports(void)
{
    struct dppd_dnat_rule rules[2] = {rule(), rule()};
    rules[1].outside_port = 2;
    rules[1].private_address_be = htonl(0x0a00000a);
    struct dppd_conntrack *table = NULL;
    assert(dppd_conntrack_create(&table, 8, 2, NULL, rules, 2) == 0);
    for (uint16_t outside = 1; outside <= 2; ++outside) {
        struct dppd_packet value = packet(false, 40000);
        struct dppd_connection_attempt attempt;
        assert(dppd_conntrack_begin(table, outside, &value, 0, &attempt) == 0);
        assert(attempt.rewrite.destination_be == rules[outside - 1].private_address_be);
        assert(dppd_conntrack_finish(&attempt, 0, true) == 0);
    }
    dppd_conntrack_destroy(table);
}

/** 整地址映射保留端口的全部位，查询分页与会话修订号无关，错误配置启动前拒绝 */
static void test_policy(void)
{
    struct dppd_dnat_rule rules[5];
    for (uint32_t i = 0; i < 5; ++i) {
        rules[i] = rule();
        rules[i].public_first = rules[i].public_last = (uint16_t)(1000 + i);
        rules[i].private_first = (uint16_t)(2000 + i);
    }
    struct dppd_conntrack *table = NULL;
    assert(dppd_conntrack_create(&table, 8, 2, NULL, rules, 5) == 0);
    struct dppd_dnat_page page;
    assert(dppd_conntrack_dnat_list(table, 0, &page) == 0 && page.count == 4 && page.more && page.next_id == 4);
    assert(page.entries[3].rule.private_first == 2003);
    assert(dppd_conntrack_dnat_list(table, 4, &page) == 0 && page.count == 1 && !page.more && page.next_id == 5);
    assert(dppd_conntrack_dnat_list(table, UINT64_MAX, &page) == 0 && page.count == 0 && !page.more);
    dppd_conntrack_destroy(table);
    rules[1] = rules[0];
    assert(dppd_nat_validate(NULL, rules, 2) == -EEXIST);
    rules[1].public_address_be++;
    assert(dppd_nat_validate(NULL, rules, 2) == -EEXIST);
    rules[1].private_first++;
    assert(dppd_nat_validate(NULL, rules, 2) == 0);
    rules[1].private_first = UINT16_MAX;
    rules[1].public_last++;
    assert(dppd_nat_validate(NULL, rules, 2) == -EINVAL);
    assert(dppd_nat_validate(NULL, rules, DPPD_MAX_DNAT_RULES + 1) == -EINVAL);
    rules[0].public_first = rules[0].private_first = 0;
    rules[0].public_last = UINT16_MAX;
    table = NULL;
    assert(dppd_conntrack_create(&table, 8, 2, NULL, rules, 1) == 0);
    for (uint32_t port = 0; port <= UINT16_MAX; port += UINT16_MAX) {
        struct dppd_packet value = packet(false, (uint16_t)port);
        struct dppd_connection_attempt attempt;
        assert(dppd_conntrack_begin(table, 1, &value, 0, &attempt) == 0);
        assert(attempt.rewrite.destination_port_be == value.l4_dst_port_be);
        assert(dppd_conntrack_finish(&attempt, 0, true) == 0);
    }
    dppd_conntrack_destroy(table);
}

int main(void)
{
    test_forwarding();
    test_snat_reservations();
    test_policy();
    test_multiple_outside_ports();
    return 0;
}
