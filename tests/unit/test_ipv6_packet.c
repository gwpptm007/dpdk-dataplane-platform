#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <rte_mbuf.h>
#include "dppd/ipv6.h"

#define SIZE 768U

struct rte_mbuf *__wrap_rte_pktmbuf_copy(const struct rte_mbuf *mbuf, struct rte_mempool *pool, uint32_t offset,
    uint32_t length)
{
    (void)mbuf;
    (void)pool;
    assert(offset == 0 && length == UINT32_MAX);
    return NULL;
}

static uint16_t checksum(const uint8_t *bytes, unsigned int size)
{
    uint32_t value = 0;
    for (unsigned int i = 0; i < size; ++i) {
        value += (uint32_t)bytes[i] << (i & 1U ? 0 : 8);
        value = (value & 65535U) + (value >> 16);
    }
    return (uint16_t)~value;
}

static void put(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

/** 独立拼接完整 IPv6 伪首部和负载计算校验和，负载超过一个生产求和块并包含奇数长度 */
static unsigned int frame(uint8_t bytes[SIZE], unsigned int vlans, unsigned int extensions, uint8_t protocol,
    unsigned int odd)
{
    memset(bytes, 0, SIZE);
    bytes[0] = bytes[6] = 2;
    unsigned int l2 = 14 + 4 * vlans, l3 = 40 + 8 * extensions;
    for (unsigned int i = 0; i < vlans; ++i) {
        bytes[12 + 4 * i] = 0x81;
        bytes[15 + 4 * i] = (uint8_t)(i + 3);
    }
    put(bytes + l2 - 2, 0x86dd);
    uint8_t *ip = bytes + l2;
    memcpy(ip, (uint8_t[]){0x6a, 0xb1, 0x23, 0x45}, 4);
    unsigned int transport = protocol == 6 ? 20 : 8, length = transport + 257 + odd;
    put(ip + 4, (uint16_t)(8 * extensions + length));
    ip[6] = extensions == 0 ? protocol : 0;
    ip[7] = 64;
    assert(inet_pton(AF_INET6, "2001:db8:1::1", ip + 8) == 1);
    assert(inet_pton(AF_INET6, "2001:db8:2::7", ip + 24) == 1);
    for (unsigned int i = 0; i < extensions; ++i)
        ip[40 + 8 * i] = i + 1 == extensions ? protocol : 60;
    uint8_t *l4 = bytes + l2 + l3;
    put(l4, 1234);
    put(l4 + 2, 443);
    if (protocol == 6) {
        l4[12] = 0x50;
        l4[13] = 16;
    } else if (protocol == 17)
        put(l4 + 4, (uint16_t)length);
    else {
        memset(l4, 0, 8);
        l4[0] = 128;
    }
    unsigned int crc_offset = protocol == 6 ? 16 : protocol == 17 ? 6 : 2;
    for (unsigned int i = transport; i < length; ++i)
        l4[i] = (uint8_t)i;
    uint8_t pseudo[SIZE] = {0};
    memcpy(pseudo, ip + 8, 32);
    put(pseudo + 34, (uint16_t)length);
    pseudo[39] = protocol;
    memcpy(pseudo + 40, l4, length);
    uint16_t crc = checksum(pseudo, 40 + length);
    put(l4 + crc_offset, crc == 0 && protocol == 17 ? UINT16_MAX : crc);
    memset(bytes + l2 + l3 + length, 0xa5, SIZE - l2 - l3 - length);
    return l2;
}

static void bind_chain(struct rte_mbuf segments[2], uint8_t bytes[SIZE], uint16_t split)
{
    memset(segments, 0, 2 * sizeof(*segments));
    segments[0].buf_addr = bytes;
    segments[0].buf_len = SIZE;
    segments[0].data_len = split;
    segments[0].pkt_len = SIZE;
    segments[0].nb_segs = split == SIZE ? 1 : 2;
    segments[0].ol_flags = RTE_MBUF_F_TX_IPV6 | RTE_MBUF_F_TX_TCP_CKSUM;
    rte_mbuf_refcnt_set(segments, 1);
    if (split != SIZE) {
        segments[0].next = segments + 1;
        segments[1].buf_addr = bytes + split;
        segments[1].buf_len = segments[1].data_len = SIZE - split;
        rte_mbuf_refcnt_set(segments + 1, 1);
    }
}

static const struct dppd_ipv4_forward forward = {.destination_mac = {2, 0, 0, 0, 1, 1}};
static const uint8_t source[6] = {2, 0, 0, 0, 2, 2};

/** 预期校验和从完整新伪首部独立重算，不能复制生产代码的增量公式来验证自己 */
static void translated_frame(uint8_t bytes[SIZE], unsigned int l2, unsigned int l3,
    const struct dppd_flow_tuple *translation)
{
    uint8_t *ip = bytes + l2, *l4 = ip + l3, pseudo[SIZE] = {0};
    memcpy(ip + 8, translation->source6, 16);
    memcpy(ip + 24, translation->destination6, 16);
    memcpy(l4, &translation->source_port_be, 2);
    memcpy(l4 + 2, &translation->destination_port_be, 2);
    unsigned int length = 40 + ((unsigned int)ip[4] << 8 | ip[5]) - l3;
    unsigned int offset = translation->protocol == 6 ? 16 : 6;
    l4[offset] = l4[offset + 1] = 0;
    memcpy(pseudo, ip + 8, 32);
    put(pseudo + 34, (uint16_t)length);
    pseudo[39] = translation->protocol;
    memcpy(pseudo + 40, l4, length);
    uint16_t value = checksum(pseudo, 40 + length);
    put(l4 + offset, value == 0 && translation->protocol == 17 ? UINT16_MAX : value);
    memcpy(bytes, forward.destination_mac, 6);
    memcpy(bytes + 6, source, 6);
    ip[7]--;
}

/** 转换地址和两端端口，逐个检查跨头部分段、双 VLAN、扩展头与奇数负载 */
static void nat_shapes(void)
{
    for (unsigned int vlan = 0; vlan <= 2; ++vlan)
        for (unsigned int extension = 0; extension <= 2; ++extension)
            for (unsigned int tcp = 0; tcp < 2; ++tcp)
                for (unsigned int odd = 0; odd < 2; ++odd)
                    for (uint16_t split = 1; split <= SIZE; ++split) {
                        uint8_t bytes[SIZE], expected[SIZE];
                        unsigned int l2 = frame(bytes, vlan, extension, tcp ? 6 : 17, odd);
                        struct dppd_flow_tuple translation = {.ipv6 = true,
                            .protocol = tcp ? 6 : 17,
                            .source_port_be = htons(40001),
                            .destination_port_be = htons(8443)};
                        assert(inet_pton(AF_INET6, "2001:db8:100:1234::5678", translation.source6) == 1);
                        assert(inet_pton(AF_INET6, "fd00:2:4321::abcd", translation.destination6) == 1);
                        memcpy(expected, bytes, SIZE);
                        translated_frame(expected, l2, 40 + 8 * extension, &translation);
                        struct rte_mbuf segments[2];
                        bind_chain(segments, bytes, split);
                        struct rte_mbuf *mbuf = segments;
                        struct dppd_packet packet;
                        assert(dppd_packet_parse_mbuf(mbuf, &packet) == DPPD_PARSE_OK);
                        if (split == 17) {
                            rte_mbuf_refcnt_set(mbuf, 2);
                            uint8_t original[SIZE];
                            memcpy(original, bytes, SIZE);
                            assert(dppd_ipv6_rewrite_flow(&mbuf, &packet, &forward, source, &translation) == -ENOMEM);
                            assert(mbuf == segments && memcmp(bytes, original, SIZE) == 0);
                            rte_mbuf_refcnt_set(mbuf, 1);
                        }
                        assert(dppd_ipv6_rewrite_flow(&mbuf, &packet, &forward, source, &translation) == 0);
                        assert(memcmp(bytes, expected, SIZE) == 0);
                        assert(dppd_packet_parse_mbuf(mbuf, &packet) == DPPD_PARSE_OK);
                        assert(dppd_ipv6_transport_validate(mbuf, &packet) == 0);
                    }
}

/** 专门构造转换后计算结果为零的 UDP，线缆上必须编码为全一，不能变成禁用校验 */
static void nat_zero_checksum(void)
{
    uint8_t bytes[SIZE], expected[SIZE];
    unsigned int l2 = frame(bytes, 2, 2, 17, 1), l3 = 56;
    struct dppd_flow_tuple translation = {.ipv6 = true, .protocol = 17, .destination_port_be = htons(8443)};
    assert(inet_pton(AF_INET6, "2001:db8:100::9", translation.source6) == 1);
    assert(inet_pton(AF_INET6, "fd00:1::8", translation.destination6) == 1);
    bool found = false;
    for (uint32_t port = 1; port <= UINT16_MAX; ++port) {
        translation.source_port_be = htons((uint16_t)port);
        memcpy(expected, bytes, SIZE);
        translated_frame(expected, l2, l3, &translation);
        if (expected[l2 + l3 + 6] == 255 && expected[l2 + l3 + 7] == 255) {
            found = true;
            break;
        }
    }
    assert(found);
    struct rte_mbuf segments[2];
    bind_chain(segments, bytes, 83);
    struct rte_mbuf *mbuf = segments;
    struct dppd_packet packet;
    assert(dppd_packet_parse_mbuf(mbuf, &packet) == DPPD_PARSE_OK);
    assert(dppd_ipv6_rewrite_flow(&mbuf, &packet, &forward, source, &translation) == 0);
    assert(memcmp(bytes, expected, SIZE) == 0);
    assert(dppd_packet_parse_mbuf(mbuf, &packet) == DPPD_PARSE_OK);
    assert(dppd_ipv6_transport_validate(mbuf, &packet) == 0);
}

/** 遍历所有分段点、双 VLAN、两种填充扩展头和三种校验和，输出只允许 MAC 与 Hop Limit 变化 */
static void shapes(void)
{
    const uint8_t protocols[] = {6, 17, 58};
    for (unsigned int vlan = 0; vlan <= 2; ++vlan)
        for (unsigned int extension = 0; extension <= 2; ++extension)
            for (unsigned int protocol = 0; protocol < 3; ++protocol)
                for (unsigned int odd = 0; odd < 2; ++odd)
                    for (uint16_t split = 1; split <= SIZE; ++split) {
                        uint8_t bytes[SIZE], expected[SIZE];
                        unsigned int l2 = frame(bytes, vlan, extension, protocols[protocol], odd);
                        memcpy(expected, bytes, SIZE);
                        struct rte_mbuf segments[2];
                        bind_chain(segments, bytes, split);
                        struct rte_mbuf *mbuf = segments;
                        struct dppd_packet packet;
                        assert(dppd_packet_parse_mbuf(mbuf, &packet) == DPPD_PARSE_OK);
                        assert(packet.l3_type == DPPD_L3_IPV6 && packet.l3_len == 40 + 8 * extension);
                        assert(
                            packet.ip_protocol == protocols[protocol] && packet.connection_state == DPPD_CT_UNTRACKED);
                        if (split == 17) {
                            rte_mbuf_refcnt_set(mbuf, 2);
                            assert(dppd_ipv6_rewrite(&mbuf, &packet, &forward, source) == -ENOMEM);
                            assert(mbuf == segments && memcmp(bytes, expected, SIZE) == 0);
                            rte_mbuf_refcnt_set(mbuf, 1);
                        }
                        memcpy(expected, forward.destination_mac, 6);
                        memcpy(expected + 6, source, 6);
                        expected[l2 + 7]--;
                        assert(dppd_ipv6_rewrite(&mbuf, &packet, &forward, source) == 0);
                        assert(
                            memcmp(bytes, expected, SIZE) == 0 && (mbuf->ol_flags & RTE_MBUF_F_TX_OFFLOAD_MASK) == 0);
                    }
}

/** 错误长度、非法扩展和坏校验和都不能改包，原子分片也不能借端口零绕过范围 ACL */
static void rejection(void)
{
    for (unsigned int mode = 0; mode < 10; ++mode) {
        uint8_t bytes[SIZE], original[SIZE];
        unsigned int l2 = frame(bytes, 0, 0, 17, 0);
        if (mode == 0)
            bytes[l2 + 7] = 1;
        else if (mode == 1)
            bytes[l2 + 40 + 6] = bytes[l2 + 40 + 7] = 0;
        else if (mode == 2)
            bytes[l2 + 60] ^= 1;
        else if (mode == 3)
            bytes[l2 + 6] = 43;
        else if (mode == 4) {
            bytes[l2 + 6] = 44;
            memset(bytes + l2 + 40, 0, 8);
            bytes[l2 + 40] = 17;
        } else if (mode == 5) {
            bytes[l2 + 6] = 0;
            bytes[l2 + 40] = 17;
            bytes[l2 + 41] = 0;
            bytes[l2 + 42] = 0x80;
            bytes[l2 + 43] = 0;
        } else if (mode == 6)
            put(bytes + l2 + 4, 65535);
        else if (mode == 7) {
            bytes[l2 + 6] = 0;
            put(bytes + l2 + 4, 7);
            bytes[l2 + 41] = 0;
        } else if (mode == 8) {
            bytes[l2 + 6] = 0;
            put(bytes + l2 + 4, 300);
            bytes[l2 + 41] = 35;
        } else {
            bytes[l2 + 6] = 0;
            put(bytes + l2 + 4, 0);
        }
        memcpy(original, bytes, SIZE);
        struct rte_mbuf segments[2];
        bind_chain(segments, bytes, 33);
        struct rte_mbuf *mbuf = segments;
        struct dppd_packet packet;
        int parsed = dppd_packet_parse_mbuf(mbuf, &packet);
        if (mode == 6 || mode == 7)
            assert(parsed == DPPD_PARSE_MALFORMED);
        else {
            assert(parsed != DPPD_PARSE_MALFORMED);
            if (mode == 4)
                assert(packet.l4_type == DPPD_L4_FRAGMENT && packet.l4_src_port_be == 0 && packet.l4_dst_port_be == 0);
            int expected = mode == 0 ? -ETIMEDOUT : mode <= 2 ? -EBADMSG : -ENOTSUP;
            assert(dppd_ipv6_rewrite(&mbuf, &packet, &forward, source) == expected);
        }
        assert(memcmp(bytes, original, SIZE) == 0);
    }
}

int main(void)
{
    shapes();
    nat_shapes();
    nat_zero_checksum();
    rejection();
    /** 零负载配 No Next Header 是合法报文，以太网尾部填充不能被误认成巨型负载 */
    uint8_t bytes[SIZE], expected[SIZE];
    unsigned int l2 = frame(bytes, 0, 0, 17, 0);
    put(bytes + l2 + 4, 0);
    bytes[l2 + 6] = 59;
    memcpy(expected, bytes, SIZE);
    memcpy(expected, forward.destination_mac, 6);
    memcpy(expected + 6, source, 6);
    expected[l2 + 7]--;
    struct rte_mbuf segments[2];
    bind_chain(segments, bytes, 41);
    struct rte_mbuf *mbuf = segments;
    struct dppd_packet packet;
    assert(dppd_packet_parse_mbuf(mbuf, &packet) == DPPD_PARSE_OK && packet.ipv6_payload_len == 0);
    assert(dppd_ipv6_rewrite(&mbuf, &packet, &forward, source) == 0 && memcmp(bytes, expected, SIZE) == 0);
    /** ICMPv6 Echo 的标识和序号缺失时，即便长度足够通用首部也必须拒绝 */
    frame(bytes, 0, 0, 58, 0);
    put(bytes + 14 + 4, 4);
    bind_chain(segments, bytes, 41);
    assert(dppd_packet_parse_mbuf(segments, &packet) == DPPD_PARSE_MALFORMED);
    return 0;
}
