#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "dppd/packet.h"

/** 将测试数值按网络字节序写入报文，不依赖运行测试的机器采用哪种字节序 */
static void put_be16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

/** 构造与解析器端口字段相同的内存表示，避免在小端机器上直接用主机数值断言 */
static uint16_t network_u16(uint16_t value)
{
    uint8_t bytes[2];
    uint16_t network_value;

    put_be16(bytes, value);
    memcpy(&network_value, bytes, sizeof(network_value));
    return network_value;
}

/** 构造无负载的 IPv4 和 UDP 测试报文，可选一层 VLAN；未填校验和，因为解析器不检查它 */
static uint32_t make_udp_packet(uint8_t *data, int vlan)
{
    uint32_t ip_offset;
    uint32_t udp_offset;

    memset(data, 0, 64);
    if (vlan) {
        put_be16(&data[12], 0x8100);
        put_be16(&data[14], 7);
        put_be16(&data[16], 0x0800);
        ip_offset = 18;
    } else {
        put_be16(&data[12], 0x0800);
        ip_offset = 14;
    }

    data[ip_offset] = 0x45;
    put_be16(&data[ip_offset + 2], 28);
    data[ip_offset + 8] = 64;
    data[ip_offset + 9] = 17;
    data[ip_offset + 12] = 192;
    data[ip_offset + 13] = 0;
    data[ip_offset + 14] = 2;
    data[ip_offset + 15] = 1;
    data[ip_offset + 16] = 198;
    data[ip_offset + 17] = 51;
    data[ip_offset + 18] = 100;
    data[ip_offset + 19] = 2;

    udp_offset = ip_offset + 20;
    put_be16(&data[udp_offset], 1234);
    put_be16(&data[udp_offset + 2], 5678);
    put_be16(&data[udp_offset + 4], 8);
    return udp_offset + 8;
}

int main(void)
{
    uint8_t data[64];
    struct dppd_packet packet;
    uint32_t length;
    int rc;

    /** 验证普通 UDP 报文的层次识别、以太网头长，以及网络字节序的源端口和目的端口 */
    length = make_udp_packet(data, 0);
    rc = dppd_packet_parse_buffer(data, length, length, &packet);
    assert(rc == DPPD_PARSE_OK);
    assert(packet.l2_len == 14);
    assert(packet.l3_type == DPPD_L3_IPV4);
    assert(packet.l4_type == DPPD_L4_UDP);
    assert(packet.l4_src_port_be == network_u16(1234));
    assert(packet.l4_dst_port_be == network_u16(5678));

    /** 插入一层 VLAN 后，标签值应保留，以太网层长度应增加 4 字节 */
    length = make_udp_packet(data, 1);
    rc = dppd_packet_parse_buffer(data, length, length, &packet);
    assert(rc == DPPD_PARSE_OK);
    assert(packet.vlan_depth == 1);
    assert(packet.vlan_tci[0] == 7);
    assert(packet.l2_len == 18);

    length = make_udp_packet(data, 0);
    /** 把 IPv4 总长度改为超出实际报文的 200 字节，应识别为畸形报文 */
    put_be16(&data[16], 200);
    assert(dppd_packet_parse_buffer(data, length, length, &packet) ==
           DPPD_PARSE_MALFORMED);

    length = make_udp_packet(data, 0);
    /** 设置 MF 位模拟首片，解析成功但只标记为分片，不继续按完整 UDP 包解释 */
    put_be16(&data[20], 0x2000);
    rc = dppd_packet_parse_buffer(data, length, length, &packet);
    assert(rc == DPPD_PARSE_OK);
    assert(packet.l4_type == DPPD_L4_FRAGMENT);
    assert(packet.ipv4_more_fragments);

    /** 未支持的以太网协议应返回 UNSUPPORTED，与头部截断的 MALFORMED 区分 */
    memset(data, 0, sizeof(data));
    put_be16(&data[12], 0x88b5);
    assert(dppd_packet_parse_buffer(data, 14, 14, &packet) ==
           DPPD_PARSE_UNSUPPORTED);
    return 0;
}
