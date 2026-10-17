#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <rte_mbuf.h>
#include "dppd/ipv4_rewrite.h"

static unsigned int copy_calls;

/** 分配失败路径不借用假的 mempool，也不释放调用方持有的测试 mbuf */
struct rte_mbuf *__wrap_rte_pktmbuf_copy(const struct rte_mbuf *mbuf, struct rte_mempool *pool, uint32_t offset,
    uint32_t length)
{
    (void)mbuf;
    (void)pool;
    assert(offset == 0 && length == UINT32_MAX);
    copy_calls++;
    return NULL;
}

static const uint8_t source_mac[6] = {2, 0, 0, 0, 1, 1};
static const struct dppd_ipv4_forward forward = {.port_id = 7, .destination_mac = {2, 0, 0, 0, 2, 2}};

/** 测试按独立的逐字节进位求和验证头部，不把实现函数当作正确性参考 */
static uint16_t header_checksum(const uint8_t *data, uint16_t length)
{
    uint32_t result = 0;
    for (uint16_t i = 0; i < length; ++i) {
        result += (uint32_t)data[i] << (i & 1U ? 0 : 8);
        result = (result & UINT16_MAX) + (result >> 16);
    }
    return (uint16_t)~result;
}

static void fix_checksum(uint8_t *data, uint16_t offset, uint16_t length)
{
    data[offset + 10] = data[offset + 11] = 0;
    uint16_t crc = header_checksum(data + offset, length);
    data[offset + 10] = (uint8_t)(crc >> 8);
    data[offset + 11] = (uint8_t)crc;
}

static void make_packet(uint8_t data[96], unsigned int vlans, unsigned int options, bool fragment)
{
    memset(data, 0, 96);
    memcpy(data, source_mac, 6);
    memcpy(data + 6, forward.destination_mac, 6);
    uint16_t layer2 = (uint16_t)(14 + 4 * vlans);
    uint16_t layer3 = (uint16_t)(20 + 4 * options);
    for (unsigned int i = 0; i < vlans; ++i) {
        data[12 + 4 * i] = 0x81;
        data[15 + 4 * i] = (uint8_t)(i + 7);
    }
    data[layer2 - 2] = 8;
    data[layer2] = (uint8_t)(0x40 | layer3 / 4);
    data[layer2 + 3] = (uint8_t)(96 - layer2);
    data[layer2 + 8] = 64;
    data[layer2 + 9] = 17;
    data[layer2 + 12] = 192;
    data[layer2 + 14] = 2;
    data[layer2 + 15] = 1;
    data[layer2 + 16] = 198;
    data[layer2 + 17] = 18;
    data[layer2 + 19] = 2;
    if (fragment)
        data[layer2 + 6] = 0x20;
    if (options != 0)
        data[layer2 + 20] = 1;
    data[layer2 + layer3] = 4;
    data[layer2 + layer3 + 1] = 0xd2;
    data[layer2 + layer3 + 2] = 0x16;
    data[layer2 + layer3 + 3] = 0x2e;
    data[layer2 + layer3 + 5] = (uint8_t)(96 - layer2 - layer3);
    for (uint16_t i = layer2 + layer3 + 8; i < 96; ++i)
        data[i] = (uint8_t)i;
    fix_checksum(data, layer2, layer3);
}

static void bind_chain(struct rte_mbuf segments[2], uint8_t data[96], uint16_t split)
{
    memset(segments, 0, 2 * sizeof(*segments));
    segments[0].buf_addr = data;
    segments[0].buf_len = 96;
    segments[0].data_len = split;
    segments[0].pkt_len = 96;
    segments[0].nb_segs = split == 96 ? 1 : 2;
    segments[0].ol_flags = RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;
    rte_mbuf_refcnt_set(&segments[0], 1);
    if (split != 96) {
        segments[0].next = &segments[1];
        segments[1].buf_addr = data + split;
        segments[1].buf_len = segments[1].data_len = (uint16_t)(96 - split);
        rte_mbuf_refcnt_set(&segments[1], 1);
    }
}

/** 分割点遍历全部字节，覆盖 MAC、VLAN、IPv4 字段和校验和跨段的情况 */
static void test_packet_shapes(void)
{
    for (unsigned int vlans = 0; vlans <= 2; ++vlans) {
        for (unsigned int options = 0; options <= 1; ++options) {
            for (unsigned int fragmented = 0; fragmented < 2; ++fragmented) {
                for (uint16_t split = 1; split <= 96; ++split) {
                    uint8_t data[96], original[96];
                    struct rte_mbuf segments[2];
                    make_packet(data, vlans, options, fragmented);
                    memcpy(original, data, sizeof(data));
                    bind_chain(segments, data, split);
                    struct dppd_packet packet;
                    assert(dppd_packet_parse_mbuf(&segments[0], &packet) == DPPD_PARSE_OK);
                    struct rte_mbuf *mbuf = &segments[0];
                    assert(dppd_ipv4_rewrite(&mbuf, &packet, &forward, source_mac) == 0 && mbuf == &segments[0]);
                    assert(memcmp(data, forward.destination_mac, 6) == 0 && memcmp(data + 6, source_mac, 6) == 0);
                    assert(data[packet.l2_len + 8] == 63);
                    assert(header_checksum(data + packet.l2_len, packet.l3_len) == 0);
                    assert(memcmp(data + 12, original + 12, packet.l2_len - 12) == 0);
                    assert(memcmp(data + packet.l2_len + packet.l3_len, original + packet.l2_len + packet.l3_len,
                               sizeof(data) - packet.l2_len - packet.l3_len) == 0);
                    assert((mbuf->ol_flags & RTE_MBUF_F_TX_OFFLOAD_MASK) == 0);
                    assert(mbuf->l2_len == packet.l2_len && mbuf->l3_len == packet.l3_len);
                }
            }
        }
    }
    assert(copy_calls == 0);
}

static void test_rejection(void)
{
    for (unsigned int mode = 0; mode < 7; ++mode) {
        uint8_t data[96], original[96];
        struct rte_mbuf segments[2];
        make_packet(data, 0, mode == 3 ? 1 : 0, false);
        if (mode == 0 || mode == 1) {
            data[22] = mode == 0 ? 0 : 1;
            fix_checksum(data, 14, 20);
        } else if (mode == 2)
            data[24] ^= 1;
        else if (mode == 3) {
            data[34] = 131;
            data[35] = 4;
            fix_checksum(data, 14, 24);
        }
        bind_chain(segments, data, 17);
        if (mode == 4)
            rte_mbuf_refcnt_set(&segments[0], 2);
        if (mode == 5)
            rte_mbuf_refcnt_set(&segments[1], 2);
        struct dppd_packet packet;
        assert(dppd_packet_parse_mbuf(&segments[0], &packet) == DPPD_PARSE_OK);
        if (mode == 6)
            packet.ipv4_dst_be ^= 1;
        memcpy(original, data, sizeof(data));
        struct rte_mbuf *mbuf = &segments[0];
        int expected = mode < 2 ? -ETIMEDOUT
            : mode == 2         ? -EBADMSG
            : mode == 3         ? -ENOTSUP
            : mode == 6         ? -EINVAL
                                : -ENOMEM;
        assert(dppd_ipv4_rewrite(&mbuf, &packet, &forward, source_mac) == expected);
        assert(mbuf == &segments[0] && memcmp(data, original, sizeof(data)) == 0);
        if (mode == 4)
            assert(rte_mbuf_refcnt_read(&segments[0]) == 2);
        if (mode == 5)
            assert(rte_mbuf_refcnt_read(&segments[1]) == 2);
    }
    assert(copy_calls == 2);
}

int main(void)
{
    test_packet_shapes();
    test_rejection();
    puts("IPv4 rewrite, checksum, TTL, VLAN and scattered-header checks passed");
    return 0;
}
