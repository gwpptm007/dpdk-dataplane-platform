#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "dppd/conntrack.h"

static unsigned int fail_allocation;
static bool ipv6_case;
void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_allocation != 0 && --fail_allocation == 0)
        return NULL;
    return __real_calloc(count, size);
}

static struct dppd_packet packet(bool reply, bool tcp)
{
    struct dppd_packet value = {.packet_len = 64,
        .l2_len = 14,
        .l3_len = 20,
        .l4_len = tcp ? 20 : 8,
        .ipv4_total_len = tcp ? 40 : 28,
        .l3_type = DPPD_L3_IPV4,
        .l4_type = tcp ? DPPD_L4_TCP : DPPD_L4_UDP,
        .ip_protocol = tcp ? 6 : 17,
        .ipv4_src_be = htonl(reply ? 0xc6336401 : 0x0a000001),
        .ipv4_dst_be = htonl(reply ? 0x0a000001 : 0xc6336401),
        .l4_src_port_be = htons(reply ? 443 : 1234),
        .l4_dst_port_be = htons(reply ? 1234 : 443),
        .tcp_flags = 2,
        .tcp_sequence_be = htonl(100)};
    if (ipv6_case) {
        value.l3_type = DPPD_L3_IPV6;
        value.l3_len = 56;
        value.ipv6_payload_len = (uint16_t)(16 + value.l4_len);
        assert(inet_pton(AF_INET6, reply ? "2001:db8:2::1" : "fd00:1::1", value.ipv6_src) == 1);
        assert(inet_pton(AF_INET6, reply ? "fd00:1::1" : "2001:db8:2::1", value.ipv6_dst) == 1);
    }
    return value;
}

static void accept_packet(struct dppd_conntrack *table, bool reply, const struct dppd_packet *value, uint64_t now,
    uint8_t state)
{
    struct dppd_connection_attempt attempt;
    assert(dppd_conntrack_begin(table, reply ? 1 : 0, value, now, &attempt) == 0);
    assert(attempt.state == state && attempt.reply == reply);
    assert(dppd_conntrack_finish(&attempt, reply ? 0 : 1, true) == 0);
}

/** 正反向只对应一个会话，未放行的回包不能改变已见回复标志，过期后释放两个索引 */
static void test_udp(void)
{
    struct dppd_conntrack *table = NULL;
    assert(dppd_conntrack_create(&table, 3, 2, NULL, NULL, 0) == 0);
    struct dppd_packet forward = packet(false, false), reply = packet(true, false);
    struct dppd_connection_attempt attempt;
    assert(dppd_conntrack_begin(table, 0, &forward, 10, &attempt) == 0 && attempt.state == DPPD_CT_NEW);
    assert(dppd_conntrack_finish(&attempt, 1, false) == 0);
    struct dppd_conntrack_status status;
    dppd_conntrack_status(table, 10, &status);
    assert(status.active == 0 && status.created == 0);
    accept_packet(table, false, &forward, 20, DPPD_CT_NEW);
    assert(dppd_conntrack_begin(table, 1, &reply, 30, &attempt) == 0 && attempt.state == DPPD_CT_ESTABLISHED);
    assert(dppd_conntrack_finish(&attempt, 0, false) == 0);
    accept_packet(table, false, &forward, 40, DPPD_CT_NEW);
    accept_packet(table, true, &reply, 50, DPPD_CT_ESTABLISHED);
    accept_packet(table, false, &forward, 60, DPPD_CT_ESTABLISHED);
    struct dppd_connection_page page;
    assert(dppd_conntrack_list(table, 0, 0, 0, &page) == 0 && page.count == 1);
    assert(page.entries[0].packets[0] == 3 && page.entries[0].packets[1] == 1);
    assert(page.entries[0].bytes[0] == 192 && page.entries[0].bytes[1] == 64);
    assert(page.entries[0].expires_ms == 2060 && page.entries[0].seen_reply);
    assert(dppd_conntrack_begin(table, 0, &forward, 100, &attempt) == 0);
    assert(dppd_conntrack_finish(&attempt, 2, true) == -EXDEV);
    dppd_conntrack_status(table, 2060, &status);
    assert(status.active == 0 && status.expired == 1);
    accept_packet(table, false, &forward, 2070, DPPD_CT_NEW);
    dppd_conntrack_destroy(table);
}

/** 验证握手序号、重传、错误 ACK、FIN 和 RST，不能用首个中途 ACK 创建连接 */
static void test_tcp(void)
{
    struct dppd_conntrack *table = NULL;
    assert(dppd_conntrack_create(&table, 8, 60, NULL, NULL, 0) == 0);
    struct dppd_packet forward = packet(false, true), reply = packet(true, true);
    struct dppd_connection_attempt attempt;
    forward.tcp_flags = 16;
    assert(dppd_conntrack_begin(table, 0, &forward, 1, &attempt) == -EPROTO);
    assert(attempt.owner == NULL && attempt.state == DPPD_CT_INVALID);
    forward.tcp_flags = 2;
    forward.ipv4_total_len++;
    forward.ipv6_payload_len++;
    assert(dppd_conntrack_begin(table, 0, &forward, 5, &attempt) == -EPROTO);
    forward.ipv4_total_len--;
    forward.ipv6_payload_len--;
    accept_packet(table, false, &forward, 10, DPPD_CT_NEW);
    forward.ipv4_total_len++;
    forward.ipv6_payload_len++;
    assert(dppd_conntrack_begin(table, 0, &forward, 15, &attempt) == -EPROTO);
    forward.ipv4_total_len--;
    forward.ipv6_payload_len--;
    accept_packet(table, false, &forward, 20, DPPD_CT_NEW);
    reply.tcp_flags = 18;
    reply.tcp_sequence_be = htonl(500);
    reply.tcp_acknowledgment_be = htonl(102);
    assert(dppd_conntrack_begin(table, 1, &reply, 30, &attempt) == -EPROTO);
    reply.tcp_acknowledgment_be = htonl(101);
    reply.ipv4_total_len++;
    reply.ipv6_payload_len++;
    assert(dppd_conntrack_begin(table, 1, &reply, 35, &attempt) == -EPROTO);
    reply.ipv4_total_len--;
    reply.ipv6_payload_len--;
    accept_packet(table, true, &reply, 40, DPPD_CT_ESTABLISHED);
    struct dppd_connection_page page;
    /** 已见 SYN/ACK 仍未完成握手，不能提前得到已建立连接的长超时 */
    assert(dppd_conntrack_list(table, 40, 0, 0, &page) == 0 && page.entries[0].expires_ms == 30040);
    reply.ipv4_total_len++;
    reply.ipv6_payload_len++;
    assert(dppd_conntrack_begin(table, 1, &reply, 45, &attempt) == -EPROTO);
    reply.ipv4_total_len--;
    reply.ipv6_payload_len--;
    accept_packet(table, true, &reply, 50, DPPD_CT_ESTABLISHED);
    forward.tcp_flags = 16;
    forward.tcp_sequence_be = htonl(101);
    forward.tcp_acknowledgment_be = htonl(501);
    accept_packet(table, false, &forward, 60, DPPD_CT_ESTABLISHED);
    assert(dppd_conntrack_list(table, 60, 0, 0, &page) == 0 && page.entries[0].tcp_state == DPPD_TCP_OPEN);
    assert(page.entries[0].expires_ms == 60060);
    forward.tcp_flags = 17;
    accept_packet(table, false, &forward, 70, DPPD_CT_ESTABLISHED);
    assert(dppd_conntrack_list(table, 70, 0, 0, &page) == 0 && page.entries[0].tcp_state == DPPD_TCP_FIN);
    assert(page.entries[0].expires_ms == 30070);
    forward.ipv4_total_len++;
    forward.ipv6_payload_len++;
    assert(dppd_conntrack_begin(table, 0, &forward, 75, &attempt) == -EPROTO);
    forward.ipv4_total_len--;
    forward.ipv6_payload_len--;
    reply.tcp_flags = 20;
    accept_packet(table, true, &reply, 80, DPPD_CT_ESTABLISHED);
    assert(dppd_conntrack_list(table, 80, 0, 0, &page) == 0 && page.entries[0].tcp_state == DPPD_TCP_CLOSED);
    struct dppd_conntrack_status status;
    dppd_conntrack_status(table, 2080, &status);
    assert(status.active == 0);
    dppd_conntrack_destroy(table);
}

/** 固定容量、分页修订号、清空和重新分配共同覆盖索引链删除后的复用 */
static void test_capacity(void)
{
    struct dppd_conntrack *table = NULL;
    assert(dppd_conntrack_create(&table, 7, 2, NULL, NULL, 0) == 0);
    struct dppd_packet value = packet(false, false);
    for (uint16_t i = 0; i < 7; ++i) {
        value.l4_src_port_be = htons((uint16_t)(1000 + i));
        accept_packet(table, false, &value, 10, DPPD_CT_NEW);
    }
    value.l4_src_port_be = htons(2000);
    struct dppd_connection_attempt attempt;
    assert(dppd_conntrack_begin(table, 0, &value, 10, &attempt) == -ENOSPC);
    struct dppd_connection_page first, second;
    assert(dppd_conntrack_list(table, 10, 0, 0, &first) == 0 && first.count == 4 && first.more);
    assert(dppd_conntrack_list(table, 10, first.next_id, first.revision, &second) == 0 && second.count == 3 &&
        !second.more);
    assert(dppd_conntrack_flush(table, first.revision - 1) == -ESTALE);
    assert(dppd_conntrack_flush(table, first.revision) == 0);
    assert(dppd_conntrack_list(table, 10, 0, first.revision, &second) == -ESTALE);
    for (uint16_t i = 0; i < 20; ++i) {
        accept_packet(table, false, &value, 3000 + i * 2001, DPPD_CT_NEW);
        dppd_conntrack_gc(table, 5000 + i * 2001, 7);
    }
    struct dppd_conntrack_status status;
    dppd_conntrack_status(table, 100000, &status);
    assert(status.active == 0 && status.expired == 20 && status.flushed == 7 && status.table_full == 1);
    dppd_conntrack_destroy(table);
}

struct reader {
    struct dppd_conntrack *table;
    bool reply;
};
static void *traffic(void *context)
{
    struct reader *reader = context;
    struct dppd_packet value = packet(reader->reply, false);
    for (unsigned int i = 0; i < 10000; ++i) {
        struct dppd_connection_attempt attempt;
        assert(dppd_conntrack_begin(reader->table, reader->reply ? 1 : 0, &value, 100, &attempt) == 0);
        assert(dppd_conntrack_finish(&attempt, reader->reply ? 0 : 1, true) == 0);
    }
    return NULL;
}

/** 四个线程从两个方向并发命中同一条流，不能重复分配会话或丢失计数 */
static void test_concurrent(void)
{
    struct dppd_conntrack *table = NULL;
    assert(dppd_conntrack_create(&table, 32, 60, NULL, NULL, 0) == 0);
    struct dppd_packet value = packet(false, false);
    accept_packet(table, false, &value, 1, DPPD_CT_NEW);
    struct reader readers[4];
    pthread_t threads[4];
    for (unsigned int i = 0; i < 4; ++i) {
        readers[i] = (struct reader){.table = table, .reply = i % 2};
        assert(pthread_create(&threads[i], NULL, traffic, &readers[i]) == 0);
    }
    for (unsigned int i = 0; i < 4; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    struct dppd_connection_page page;
    assert(dppd_conntrack_list(table, 100, 0, 0, &page) == 0 && page.count == 1);
    assert(page.entries[0].packets[0] == 20001 && page.entries[0].packets[1] == 20000);
    dppd_conntrack_destroy(table);
}

/** NAT 端口在同协议内独占，取消候选、路由错误和过期都要归还，回包必须匹配远端和入口 */
static void test_snat(void)
{
    struct dppd_snat_config nat = {.enabled = true,
        .inside_port = 0,
        .outside_port = 1,
        .first_port = 40000,
        .last_port = 40001,
        .address_be = htonl(0xcb007101)};
    struct dppd_conntrack *table = NULL;
    assert(dppd_conntrack_create(&table, 3, 2, &nat, NULL, 0) == 0);
    struct dppd_packet value = packet(false, false), reply = packet(true, false);
    struct dppd_connection_attempt attempt;
    assert(dppd_conntrack_begin(table, 0, &value, 10, &attempt) == 0 && attempt.nat);
    assert(dppd_conntrack_finish(&attempt, 1, false) == 0);
    assert(dppd_conntrack_begin(table, 0, &value, 20, &attempt) == 0);
    assert(attempt.policy.source_be == value.ipv4_src_be && attempt.rewrite.source_be == nat.address_be);
    uint16_t mapped = attempt.rewrite.source_port_be;
    assert(dppd_conntrack_finish(&attempt, 1, true) == 0);
    reply.ipv4_dst_be = nat.address_be;
    reply.l4_dst_port_be = mapped;
    assert(dppd_conntrack_begin(table, 1, &reply, 30, &attempt) == 0 && attempt.state == DPPD_CT_ESTABLISHED);
    assert(attempt.rewrite.destination_be == value.ipv4_src_be &&
        attempt.rewrite.destination_port_be == value.l4_src_port_be);
    assert(dppd_conntrack_finish(&attempt, 0, true) == 0);
    reply.ipv4_src_be ^= htonl(1);
    assert(dppd_conntrack_begin(table, 1, &reply, 40, &attempt) == -ENOENT);
    reply.ipv4_src_be ^= htonl(1);
    assert(dppd_conntrack_begin(table, 2, &reply, 40, &attempt) == -ENOENT);
    value.l4_src_port_be = htons(1235);
    assert(dppd_conntrack_begin(table, 0, &value, 50, &attempt) == 0 && attempt.rewrite.source_port_be != mapped);
    assert(dppd_conntrack_finish(&attempt, 1, true) == 0);
    value.l4_src_port_be = htons(1236);
    assert(dppd_conntrack_begin(table, 0, &value, 60, &attempt) == -EADDRNOTAVAIL);
    value = packet(false, true);
    assert(dppd_conntrack_begin(table, 0, &value, 70, &attempt) == 0);
    assert(dppd_conntrack_finish(&attempt, 1, true) == 0);
    struct dppd_conntrack_status status;
    dppd_conntrack_status(table, 10000, &status);
    assert(status.active == 0 && status.expired == 3 && status.pool_full == 1);
    value = packet(false, false);
    assert(dppd_conntrack_begin(table, 0, &value, 10001, &attempt) == 0);
    assert(dppd_conntrack_finish(&attempt, 2, true) == -EXDEV);
    accept_packet(table, false, &value, 10002, DPPD_CT_NEW);
    dppd_conntrack_destroy(table);
}

struct publication {
    unsigned int calls;
    int result;
};

static int publish_policy(void *context)
{
    struct publication *publication = context;
    publication->calls++;
    return publication->result;
}

/** 通过真实端口池检查替换结果，不能只看管理状态中的地址是否更新 */
static struct dppd_conntrack_policy nat_policy(void)
{
    struct dppd_conntrack_policy policy = {.capacity = 8, .timeout_s = 60, .dnat_count = 1};
    struct dppd_snat_config *nat = ipv6_case ? &policy.snat6 : &policy.snat;
    *nat = (struct dppd_snat_config){.enabled = true, .inside_port = 0, .outside_port = 1,
        .first_port = 40000, .last_port = 40001, .ipv6 = ipv6_case};
    policy.dnat[0] = (struct dppd_dnat_rule){.outside_port = 1, .inside_port = 0, .public_first = 40000,
        .public_last = 40000, .private_first = 80, .protocol = 17, .ipv6 = ipv6_case};
    if (ipv6_case) {
        assert(inet_pton(AF_INET6, "2001:db8:1::1", nat->address6) == 1);
        memcpy(policy.dnat[0].public_address6, nat->address6, 16);
        assert(inet_pton(AF_INET6, "fd00:1::9", policy.dnat[0].private_address6) == 1);
    } else {
        nat->address_be = htonl(0xcb007101);
        policy.dnat[0].public_address_be = nat->address_be;
        policy.dnat[0].private_address_be = htonl(0x0a000009);
    }
    return policy;
}

/** 预分配和发布失败都保留原会话，明确清空后才切换策略，关闭再启用也不能复用旧会话 ID */
static void test_policy_replace(void)
{
    struct dppd_conntrack_policy disabled = {0}, policy = nat_policy();
    struct dppd_conntrack *table = NULL;
    assert(dppd_conntrack_policy_validate(&disabled) == 0);
    disabled.timeout_s = 1;
    assert(dppd_conntrack_policy_validate(&disabled) == -EINVAL);
    disabled.timeout_s = 0;
    disabled.snat.inside_port = 1;
    assert(dppd_conntrack_policy_validate(&disabled) == -EINVAL);
    disabled.snat.inside_port = 0;
    disabled.dnat[31].protocol = 17;
    assert(dppd_conntrack_policy_validate(&disabled) == -EINVAL);
    disabled.dnat[31].protocol = 0;
    assert(dppd_conntrack_create_policy(&table, &disabled) == 0);
    struct dppd_conntrack *stable = table;
    struct dppd_conntrack_status before, after;
    dppd_conntrack_status(table, 0, &before);
    assert(!before.enabled && before.capacity == 0 && before.revision == 1 && before.epoch != 0);
    uint64_t other_epoch = before.epoch == UINT64_MAX ? 1 : before.epoch + 1;
    struct publication publication = {0};
    assert(dppd_conntrack_replace(table, &policy, false, 0, before.revision, publish_policy, &publication) == -EINVAL);
    assert(dppd_conntrack_replace(table, &policy, false, other_epoch, before.revision, NULL, NULL) == -ESTALE);
    assert(dppd_conntrack_replace(table, &policy, false, before.epoch, before.revision, publish_policy,
               &publication) == 0 && publication.calls == 1 && table == stable);
    struct dppd_packet value = packet(false, false);
    struct dppd_connection_attempt attempt;
    assert(dppd_conntrack_begin(table, 0, &value, 100, &attempt) == 0 && attempt.tracking && attempt.nat);
    assert(ntohs(attempt.rewrite.source_port_be) == 40001);
    assert(dppd_conntrack_finish(&attempt, 1, true) == 0);
    dppd_conntrack_status(table, 100, &before);
    assert(before.active == 1 && before.created == 1 && before.revision == 3);
    struct dppd_connection_page page;
    assert(dppd_conntrack_list_guarded(table, 100, 0, 0, 0, &page) == 0 && page.epoch == before.epoch &&
        page.entries[0].id == 1);
    assert(dppd_conntrack_list_guarded(table, 100, 1, 0, 0, &page) == -EINVAL);
    assert(dppd_conntrack_list_guarded(table, 100, 0, other_epoch, before.revision, &page) == -ESTALE);
    policy.capacity = 16;
    policy.timeout_s = 2;
    struct dppd_snat_config *nat = ipv6_case ? &policy.snat6 : &policy.snat;
    nat->first_port = policy.dnat[0].public_first = policy.dnat[0].public_last = 50000;
    nat->last_port = 50001;
    if (ipv6_case) {
        nat->address6[15] = 2;
        memcpy(policy.dnat[0].public_address6, nat->address6, 16);
    } else
        policy.dnat[0].public_address_be = nat->address_be = htonl(0xcb007102);
    for (unsigned int allocation = 1; allocation <= 4; ++allocation) {
        fail_allocation = allocation;
        assert(dppd_conntrack_replace(table, &policy, true, before.epoch, before.revision, publish_policy,
                   &publication) == -ENOMEM);
        fail_allocation = 0;
        dppd_conntrack_status(table, 100, &after);
        assert(after.revision == before.revision && after.active == 1 && after.capacity == 8);
    }
    assert(publication.calls == 1);
    assert(dppd_conntrack_replace(table, &policy, false, before.epoch, before.revision, publish_policy,
               &publication) == -EBUSY && publication.calls == 1);
    assert(dppd_conntrack_replace(table, &policy, true, before.epoch, before.revision - 1, NULL, NULL) == -ESTALE);
    assert(dppd_conntrack_flush_guarded(table, 0, before.revision) == -EINVAL);
    assert(dppd_conntrack_flush_guarded(table, other_epoch, before.revision) == -ESTALE);
    publication.result = -EIO;
    assert(dppd_conntrack_replace(table, &policy, true, before.epoch, before.revision, publish_policy,
               &publication) == -EIO && publication.calls == 2);
    assert(dppd_conntrack_list(table, 100, 0, before.revision, &page) == 0 && page.count == 1 &&
        ntohs(page.entries[0].translated.source_port_be) == 40001);
    publication.result = 0;
    assert(dppd_conntrack_replace(table, &policy, true, before.epoch, before.revision, publish_policy,
               &publication) == 0 && publication.calls == 3);
    dppd_conntrack_status(table, 100, &after);
    assert(after.epoch == before.epoch && after.revision == before.revision + 1 && after.active == 0 &&
        after.flushed == 1 && after.created == 1 && after.peak == 1 && after.capacity == 16 && after.timeout_s == 2);
    assert(dppd_conntrack_list_guarded(table, 100, 0, before.epoch, before.revision, &page) == -ESTALE);
    struct dppd_dnat_page rules;
    assert(dppd_conntrack_dnat_list(table, 0, &rules) == 0 && rules.count == 1 &&
        rules.entries[0].rule.public_first == 50000);
    assert(dppd_conntrack_begin(table, 0, &value, 200, &attempt) == 0 && attempt.tracking);
    assert(ntohs(attempt.rewrite.source_port_be) == 50001);
    assert(ipv6_case ? memcmp(attempt.rewrite.source6, nat->address6, 16) == 0
                    : attempt.rewrite.source_be == nat->address_be);
    assert(dppd_conntrack_finish(&attempt, 1, true) == 0);
    assert(dppd_conntrack_list(table, 200, 0, 0, &page) == 0 && page.entries[0].id == 2 &&
        page.entries[0].expires_ms == 2200);
    dppd_conntrack_gc(table, 2200, 16);
    dppd_conntrack_status(table, 2200, &before);
    assert(before.active == 0 && before.expired == 1);
    assert(dppd_conntrack_replace(table, &disabled, false, before.epoch, before.revision, NULL, NULL) == 0);
    dppd_conntrack_gc(table, 2201, UINT32_MAX);
    dppd_conntrack_status(table, 2201, &after);
    assert(!after.enabled && after.epoch == before.epoch && after.created == 2 && after.flushed == 1);
    value.ip_protocol = 0;
    assert(dppd_conntrack_begin(table, 0, &value, 2201, &attempt) == 0 && !attempt.tracking &&
        attempt.owner == NULL && attempt.state == DPPD_CT_UNTRACKED);
    assert(dppd_conntrack_list_guarded(table, 2201, 0, after.epoch, after.revision, &page) == 0 && page.count == 0);
    assert(dppd_conntrack_replace(table, &policy, false, after.epoch, after.revision, NULL, NULL) == 0);
    value = packet(false, false);
    accept_packet(table, false, &value, 2202, DPPD_CT_NEW);
    assert(dppd_conntrack_list(table, 2202, 0, 0, &page) == 0 && page.entries[0].id == 3);
    assert(dppd_conntrack_flush_guarded(table, page.epoch, page.revision) == 0);
    struct dppd_conntrack *restarted = NULL;
    assert(dppd_conntrack_create_policy(&restarted, &policy) == 0);
    dppd_conntrack_status(restarted, 0, &after);
    assert(after.epoch != before.epoch && after.revision == 1 && after.active == 0);
    assert(dppd_conntrack_replace(restarted, &disabled, true, before.epoch, 1, NULL, NULL) == -ESTALE);
    dppd_conntrack_destroy(restarted);
    dppd_conntrack_destroy(table);
}

/** 更新时 worker 可继续持有旧地址，表锁保证一次 begin/finish 不跨越两个策略版本 */
static void test_replace_concurrent(void)
{
    struct dppd_conntrack_policy enabled = {.capacity = 8, .timeout_s = 60}, disabled = {0};
    struct dppd_conntrack *table = NULL;
    assert(dppd_conntrack_create_policy(&table, &enabled) == 0);
    struct reader readers[2] = {{.table = table}, {.table = table}};
    pthread_t threads[2];
    for (unsigned int i = 0; i < 2; ++i)
        assert(pthread_create(&threads[i], NULL, traffic, &readers[i]) == 0);
    for (unsigned int i = 0; i < 100; ++i) {
        struct dppd_conntrack_status status;
        dppd_conntrack_status(table, 100, &status);
        int rc = dppd_conntrack_replace(table, i % 2 ? &enabled : &disabled, true, status.epoch, status.revision,
            NULL, NULL);
        assert(rc == 0 || rc == -ESTALE);
        struct dppd_dnat_page rules;
        assert(dppd_conntrack_dnat_list(table, 0, &rules) == 0 && rules.count == 0);
    }
    for (unsigned int i = 0; i < 2; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    dppd_conntrack_destroy(table);
}

int main(void)
{
    for (unsigned int i = 1; i <= 4; ++i) {
        struct dppd_conntrack *table = NULL;
        fail_allocation = i;
        assert(dppd_conntrack_create(&table, 8, 60, NULL, NULL, 0) == -ENOMEM && table == NULL);
        fail_allocation = 0;
    }
    /** 相同的状态机验收分别使用 IPv4 和含扩展头长度的 IPv6，不能只验证地址字段替换 */
    for (unsigned int family = 0; family < 2; ++family) {
        ipv6_case = family != 0;
        test_udp();
        test_tcp();
        test_capacity();
        test_concurrent();
        test_policy_replace();
        test_replace_concurrent();
    }
    ipv6_case = false;
    test_snat();
    return 0;
}
