#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include "dppd/conntrack.h"

static unsigned int fail_allocation;
void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_allocation != 0 && --fail_allocation == 0)
        return NULL;
    return __real_calloc(count, size);
}

static struct dppd_packet packet(bool reply, bool tcp)
{
    return (struct dppd_packet){.packet_len = 64,
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
    assert(dppd_conntrack_create(&table, 3, 2, NULL) == 0);
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
    assert(dppd_conntrack_create(&table, 8, 60, NULL) == 0);
    struct dppd_packet forward = packet(false, true), reply = packet(true, true);
    struct dppd_connection_attempt attempt;
    forward.tcp_flags = 16;
    assert(dppd_conntrack_begin(table, 0, &forward, 1, &attempt) == -EPROTO);
    assert(attempt.owner == NULL && attempt.state == DPPD_CT_INVALID);
    forward.tcp_flags = 2;
    forward.ipv4_total_len++;
    assert(dppd_conntrack_begin(table, 0, &forward, 5, &attempt) == -EPROTO);
    forward.ipv4_total_len--;
    accept_packet(table, false, &forward, 10, DPPD_CT_NEW);
    forward.ipv4_total_len++;
    assert(dppd_conntrack_begin(table, 0, &forward, 15, &attempt) == -EPROTO);
    forward.ipv4_total_len--;
    accept_packet(table, false, &forward, 20, DPPD_CT_NEW);
    reply.tcp_flags = 18;
    reply.tcp_sequence_be = htonl(500);
    reply.tcp_acknowledgment_be = htonl(102);
    assert(dppd_conntrack_begin(table, 1, &reply, 30, &attempt) == -EPROTO);
    reply.tcp_acknowledgment_be = htonl(101);
    reply.ipv4_total_len++;
    assert(dppd_conntrack_begin(table, 1, &reply, 35, &attempt) == -EPROTO);
    reply.ipv4_total_len--;
    accept_packet(table, true, &reply, 40, DPPD_CT_ESTABLISHED);
    struct dppd_connection_page page;
    /** 已见 SYN/ACK 仍未完成握手，不能提前得到已建立连接的长超时 */
    assert(dppd_conntrack_list(table, 40, 0, 0, &page) == 0 && page.entries[0].expires_ms == 30040);
    reply.ipv4_total_len++;
    assert(dppd_conntrack_begin(table, 1, &reply, 45, &attempt) == -EPROTO);
    reply.ipv4_total_len--;
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
    assert(dppd_conntrack_begin(table, 0, &forward, 75, &attempt) == -EPROTO);
    forward.ipv4_total_len--;
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
    assert(dppd_conntrack_create(&table, 7, 2, NULL) == 0);
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
    assert(dppd_conntrack_create(&table, 32, 60, NULL) == 0);
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
    assert(dppd_conntrack_create(&table, 3, 2, &nat) == 0);
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

int main(void)
{
    for (unsigned int i = 1; i <= 4; ++i) {
        struct dppd_conntrack *table = NULL;
        fail_allocation = i;
        assert(dppd_conntrack_create(&table, 8, 60, NULL) == -ENOMEM && table == NULL);
        fail_allocation = 0;
    }
    test_udp();
    test_tcp();
    test_capacity();
    test_concurrent();
    test_snat();
    return 0;
}
