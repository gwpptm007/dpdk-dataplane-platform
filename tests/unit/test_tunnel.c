#include <arpa/inet.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <rte_mbuf.h>
#include "dppd/tunnel.h"

#define SIZE 1536U

static void put(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

/** 构造器独立填写各层长度，测试期望来自输入参数，不读取生产解析器算出的偏移 */
static unsigned int inner_frame(uint8_t *bytes, bool ipv6, unsigned int vlans, bool raw, bool tcp)
{
    unsigned int l2 = raw ? 0 : 14 + 4 * vlans, l3 = ipv6 ? 40 : 20, l4 = tcp ? 20 : 8;
    if (!raw) {
        bytes[0] = bytes[6] = 2;
        for (unsigned int i = 0; i < vlans; ++i) {
            put(bytes + 12 + i * 4, vlans == 2 && i == 0 ? 0x88a8 : 0x8100);
            put(bytes + 14 + i * 4, (uint16_t)(100 + i));
        }
        put(bytes + l2 - 2, ipv6 ? 0x86dd : 0x0800);
    }
    uint8_t *ip = bytes + l2, *transport = ip + l3;
    if (ipv6) {
        ip[0] = 0x60;
        put(ip + 4, (uint16_t)(l4 + 37));
        ip[6] = tcp ? 6 : 17;
        ip[7] = 64;
        memset(ip + 8, 0x11, 16);
        memset(ip + 24, 0x22, 16);
    } else {
        ip[0] = 0x45;
        put(ip + 2, (uint16_t)(l3 + l4 + 37));
        ip[8] = 64;
        ip[9] = tcp ? 6 : 17;
        memset(ip + 12, 0x11, 4);
        memset(ip + 16, 0x22, 4);
    }
    put(transport, 1234);
    put(transport + 2, 443);
    if (tcp)
        transport[12] = 0x50;
    else
        put(transport + 4, (uint16_t)(l4 + 37));
    memset(transport + l4, 0x5a, 37);
    return l2 + l3 + l4 + 37;
}

struct layout {
    unsigned int total, start, inner, end, inner_length;
};

static struct layout frame(uint8_t bytes[SIZE], enum dppd_tunnel_type type, bool outer6, bool inner6,
    unsigned int vlans, bool raw, bool tcp, unsigned int flags)
{
    memset(bytes, 0, SIZE);
    unsigned int l2 = 14 + 4 * vlans, l3 = outer6 ? 48 : 20;
    for (unsigned int i = 0; i < vlans; ++i) {
        put(bytes + 12 + 4 * i, vlans == 2 && i == 0 ? 0x88a8 : 0x8100);
        put(bytes + 14 + 4 * i, (uint16_t)(7 + i));
    }
    put(bytes + l2 - 2, outer6 ? 0x86dd : 0x0800);
    unsigned int start = l2 + l3 + (type == DPPD_TUNNEL_GRE ? 0 : 8);
    unsigned int header;
    uint16_t protocol = raw ? (inner6 ? 0x86dd : 0x0800) : 0x6558;
    if (type == DPPD_TUNNEL_GRE) {
        put(bytes + start,
            (uint16_t)(((flags & 1) ? 0x8000 : 0) | ((flags & 2) ? 0x2000 : 0) | ((flags & 4) ? 0x1000 : 0)));
        put(bytes + start + 2, protocol);
        header = 4;
        if (flags & 1)
            header += 4;
        if (flags & 2) {
            memcpy(bytes + start + header, (uint8_t[]){0xab, 0xcd, 0xef, 1}, 4);
            header += 4;
        }
        if (flags & 4) {
            memcpy(bytes + start + header, (uint8_t[]){0xab, 0xcd, 0xef, 2}, 4);
            header += 4;
        }
    } else {
        header = type == DPPD_TUNNEL_VXLAN ? 8 : 20;
        bytes[start] = type == DPPD_TUNNEL_VXLAN ? 8 : 3;
        memcpy(bytes + start + 4, (uint8_t[]){1, 2, 3}, 3);
        if (type == DPPD_TUNNEL_GENEVE) {
            put(bytes + start + 2, protocol);
            put(bytes + start + 8, 0x1234);
            put(bytes + start + 12, 0x5678);
            bytes[start + 15] = 1;
        }
        put(bytes + l2 + l3, 12345);
        put(bytes + l2 + l3 + 2, type == DPPD_TUNNEL_VXLAN ? 4789 : 6081);
    }
    unsigned int inner = start + header;
    unsigned int inner_length = inner_frame(bytes + inner, inner6, vlans, raw, tcp), end = inner + inner_length;
    if (type != DPPD_TUNNEL_GRE)
        put(bytes + l2 + l3 + 4, (uint16_t)(end - l2 - l3));
    uint8_t *ip = bytes + l2;
    if (outer6) {
        ip[0] = 0x60;
        put(ip + 4, (uint16_t)(end - l2 - 40));
        ip[6] = 0;
        ip[7] = 64;
        ip[40] = type == DPPD_TUNNEL_GRE ? 47 : 17;
    } else {
        ip[0] = 0x45;
        put(ip + 2, (uint16_t)(end - l2));
        ip[8] = 64;
        ip[9] = type == DPPD_TUNNEL_GRE ? 47 : 17;
    }
    memset(bytes + end, 0xa5, 16);
    return (struct layout){end + 16, start, inner, end, inner_length};
}

/** 用每个字节位置拆成两段，确认隧道头、VLAN、内层 IP 和端口均不要求连续存放 */
static void check_scattered(uint8_t *bytes, unsigned int length, const struct dppd_tunnel_info *expected)
{
    for (unsigned int split = 1; split < length; ++split) {
        struct rte_mbuf segments[2];
        memset(segments, 0, sizeof(segments));
        segments[0].buf_addr = bytes;
        segments[0].buf_len = segments[0].data_len = (uint16_t)split;
        segments[0].pkt_len = length;
        segments[0].nb_segs = 2;
        segments[0].next = segments + 1;
        segments[1].buf_addr = bytes + split;
        segments[1].buf_len = segments[1].data_len = (uint16_t)(length - split);
        struct dppd_packet packet;
        assert(dppd_packet_parse_mbuf(segments, &packet) == DPPD_PARSE_OK);
        assert(memcmp(&packet.tunnel, expected, sizeof(*expected)) == 0);
    }
}

static void test_matrix(void)
{
    uint8_t bytes[SIZE];
    for (enum dppd_tunnel_type type = DPPD_TUNNEL_VXLAN; type <= DPPD_TUNNEL_GRE; ++type)
        for (unsigned int outer6 = 0; outer6 < 2; ++outer6)
            for (unsigned int inner6 = 0; inner6 < 2; ++inner6)
                for (unsigned int vlans = 0; vlans < 3; ++vlans)
                    for (unsigned int tcp = 0; tcp < 2; ++tcp)
                        for (unsigned int flags = 0; flags < (type == DPPD_TUNNEL_GRE ? 8U : 1U); ++flags)
                            for (unsigned int raw = 0; raw <= (type != DPPD_TUNNEL_VXLAN); ++raw) {
                                struct layout where = frame(bytes, type, outer6, inner6, vlans, raw, tcp, flags);
                                struct dppd_packet packet;
                                assert(dppd_packet_parse_buffer(bytes, where.total, where.total, &packet) == 0);
                                const struct dppd_tunnel_info *info = &packet.tunnel;
                                assert(info->type == type && info->status == DPPD_PARSE_OK && info->inner_valid);
                                assert(info->inner_offset == where.inner && info->inner_length == where.inner_length);
                                assert(info->inner_l3 == (inner6 ? DPPD_L3_IPV6 : DPPD_L3_IPV4));
                                assert(info->inner_l4 == (tcp ? DPPD_L4_TCP : DPPD_L4_UDP));
                                assert(info->inner_ip_protocol == (tcp ? 6 : 17));
                                assert(info->inner_src_port_be == htons(1234) && info->inner_dst_port_be == htons(443));
                                assert(info->inner_vlan_depth == (raw ? 0 : vlans));
                                assert(info->identifier ==
                                    (type == DPPD_TUNNEL_GRE ? (flags & 2 ? 0xabcdef01 : 0) : 0x010203));
                                assert(info->has_checksum == (type == DPPD_TUNNEL_GRE && (flags & 1) != 0));
                                assert(info->has_key == (type == DPPD_TUNNEL_GRE && (flags & 2) != 0));
                                assert(info->has_sequence == (type == DPPD_TUNNEL_GRE && (flags & 4) != 0));
                                assert(info->sequence == (type == DPPD_TUNNEL_GRE && (flags & 4) ? 0xabcdef02 : 0));
                                assert(packet.vlan_depth == vlans &&
                                    packet.l3_type == (outer6 ? DPPD_L3_IPV6 : DPPD_L3_IPV4));
                                assert(info->inner_src_be == (inner6 ? 0 : 0x11111111));
                                if (inner6)
                                    for (unsigned int byte = 0; byte < 16; ++byte)
                                        assert(info->inner_src6[byte] == 0x11 && info->inner_dst6[byte] == 0x22);
                                check_scattered(bytes, where.total, info);
                            }
}

static void test_boundaries(void)
{
    uint8_t bytes[SIZE];
    for (enum dppd_tunnel_type type = DPPD_TUNNEL_VXLAN; type <= DPPD_TUNNEL_GRE; ++type) {
        struct layout where = frame(bytes, type, false, false, 0, false, false, 7);
        struct dppd_packet packet;
        /** 每次只分配声明可读的字节数，让 sanitizer 检查所有截断点的实际越界访问 */
        for (unsigned int cut = 1; cut < where.total; ++cut) {
            uint8_t *short_view = malloc(cut);
            assert(short_view != NULL);
            memcpy(short_view, bytes, cut);
            int rc = dppd_packet_parse_buffer(short_view, cut, where.total, &packet);
            assert(rc == DPPD_PARSE_MALFORMED || rc == DPPD_PARSE_OK);
            if (rc == DPPD_PARSE_OK && cut < where.inner)
                assert(!packet.tunnel.inner_valid);
            free(short_view);
        }
        assert(dppd_packet_parse_buffer(bytes, where.end - 1, where.end - 1, &packet) == DPPD_PARSE_MALFORMED);
        if (type == DPPD_TUNNEL_VXLAN)
            bytes[where.start] = 0;
        else if (type == DPPD_TUNNEL_GENEVE)
            bytes[where.start + 1] = 0x40;
        else
            bytes[where.start + 1] = 1;
        assert(dppd_packet_parse_buffer(bytes, where.total, where.total, &packet) == DPPD_PARSE_OK);
        assert(packet.tunnel.status == DPPD_PARSE_UNSUPPORTED && !packet.tunnel.inner_valid);
        where = frame(bytes, type, false, false, 0, false, false, 7);
        /** 外层分片不能借首片恰好含有隧道头而提前提取内层端口 */
        put(bytes + 20, 0x2000);
        assert(dppd_packet_parse_buffer(bytes, where.total, where.total, &packet) == DPPD_PARSE_OK);
        assert(packet.tunnel.type == DPPD_TUNNEL_NONE);
    }
    struct layout where = frame(bytes, DPPD_TUNNEL_GENEVE, false, false, 0, false, false, 0);
    bytes[where.start + 11] = 31;
    struct dppd_packet packet;
    assert(dppd_packet_parse_buffer(bytes, where.total, where.total, &packet) == DPPD_PARSE_OK);
    assert(packet.tunnel.status == DPPD_PARSE_MALFORMED);
    where = frame(bytes, DPPD_TUNNEL_GENEVE, false, false, 0, false, false, 0);
    bytes[where.start + 10] = 0x80;
    assert(dppd_packet_parse_buffer(bytes, where.total, where.total, &packet) == DPPD_PARSE_OK);
    assert(packet.tunnel.critical && packet.tunnel.status == DPPD_PARSE_UNSUPPORTED);
    where = frame(bytes, DPPD_TUNNEL_VXLAN, false, false, 0, false, false, 0);
    bytes[where.inner + 14] = 0x40;
    assert(dppd_packet_parse_buffer(bytes, where.total, where.total, &packet) == DPPD_PARSE_OK);
    assert(packet.tunnel.status == DPPD_PARSE_MALFORMED && !packet.tunnel.inner_valid);
    /** UDP 声明长度是隧道边界，即使以太网填充恰好包含完整内层也不能借用 */
    where = frame(bytes, DPPD_TUNNEL_VXLAN, false, false, 0, false, false, 0);
    put(bytes + 38, 16);
    assert(dppd_packet_parse_buffer(bytes, where.total, where.total, &packet) == DPPD_PARSE_OK);
    assert(packet.tunnel.inner_length == 0 && packet.tunnel.status == DPPD_PARSE_MALFORMED);
}

/** 固定种子的随机变异只检查可证明的不变量，让 sanitizer 覆盖协议字段组合与短窗口 */
static void test_mutations(void)
{
    uint8_t bytes[SIZE], original[SIZE];
    uint32_t random = 0x43a271b9;
    for (unsigned int sample = 0; sample < 20000; ++sample) {
        random = random * 1664525U + 1013904223U;
        enum dppd_tunnel_type type = (enum dppd_tunnel_type)(1 + sample % 3);
        struct layout where =
            frame(bytes, type, (random & 1) != 0, (random & 2) != 0, sample % 3, false, (random & 4) != 0, random % 8);
        for (unsigned int change = 0; change < 3; ++change) {
            random = random * 1664525U + 1013904223U;
            bytes[random % where.total] ^= (uint8_t)(random >> 24);
        }
        memcpy(original, bytes, sizeof(bytes));
        uint32_t available = sample % 2 == 0 ? where.total : random % where.total;
        struct dppd_packet packet;
        int rc = dppd_packet_parse_buffer(bytes, available, where.total, &packet);
        assert(rc == DPPD_PARSE_OK || rc == DPPD_PARSE_MALFORMED || rc == DPPD_PARSE_UNSUPPORTED);
        if (rc == DPPD_PARSE_OK && packet.tunnel.inner_valid) {
            assert(packet.tunnel.status == DPPD_PARSE_OK);
            assert(packet.tunnel.inner_offset <= where.total);
            assert(packet.tunnel.inner_length <= where.total - packet.tunnel.inner_offset);
        }
        assert(memcmp(bytes, original, sizeof(bytes)) == 0);
    }
}

static void insert_zero(uint8_t *bytes, unsigned int *total, unsigned int offset, unsigned int count)
{
    assert(*total + count <= SIZE && offset <= *total);
    memmove(bytes + offset + count, bytes + offset, *total - offset);
    memset(bytes + offset, 0, count);
    *total += count;
}

/** 最大 Geneve 选项与双层 IPv6 扩展头共存，长负载无需放入 1024 字节观察窗口 */
static void test_deep_headers(void)
{
    uint8_t bytes[SIZE];
    struct layout where = frame(bytes, DPPD_TUNNEL_GENEVE, true, true, 2, false, true, 0);
    unsigned int total = where.total;
    insert_zero(bytes, &total, 22 + 48, 248);
    unsigned int start = where.start + 248, inner = where.inner + 248;
    memset(bytes + 62, 0, 256);
    bytes[62] = 60;
    bytes[63] = 15;
    bytes[190] = 17;
    bytes[191] = 15;
    insert_zero(bytes, &total, inner, 240);
    memset(bytes + start + 8, 0, 252);
    bytes[start] = 63;
    inner += 240;
    unsigned int ip = inner + 22;
    insert_zero(bytes, &total, ip + 40, 256);
    bytes[ip + 6] = 0;
    bytes[ip + 40] = 60;
    bytes[ip + 41] = 15;
    bytes[ip + 168] = 6;
    bytes[ip + 169] = 15;
    insert_zero(bytes, &total, total - 16, 400);
    put(bytes + ip + 4, (uint16_t)(256 + 20 + 37 + 400));
    put(bytes + 22 + 4, (uint16_t)(total - 16 - 22 - 40));
    put(bytes + 22 + 296 + 4, (uint16_t)(total - 16 - 22 - 296));
    struct dppd_packet packet;
    assert(total > 1024);
    assert(dppd_packet_parse_buffer(bytes, 1024, total, &packet) == DPPD_PARSE_OK);
    assert(packet.tunnel.status == DPPD_PARSE_OK && packet.tunnel.inner_valid);
    assert(packet.tunnel.header_len == 260 && packet.tunnel.inner_offset == inner);
    assert(packet.tunnel.inner_src_port_be == htons(1234) && packet.tunnel.inner_dst_port_be == htons(443));
    check_scattered(bytes, total, &packet.tunnel);
}

int main(void)
{
    test_matrix();
    test_boundaries();
    test_mutations();
    test_deep_headers();
    return 0;
}
