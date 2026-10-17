#include "dppd/ipv6.h"

#include <string.h>

static uint16_t word(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] << 8 | data[1]);
}

bool dppd_ipv6_unicast(const uint8_t address[16], bool allow_link_local)
{
    const uint8_t zero[16] = {0}, loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255};
    return memcmp(address, zero, 16) != 0 && memcmp(address, loopback, 16) != 0 && address[0] != 255 &&
        memcmp(address, mapped, 12) != 0 && (allow_link_local || address[0] != 0xfe || (address[1] & 0xc0U) != 0x80);
}

static bool covered(uint32_t offset, uint32_t size, uint32_t available, uint32_t end)
{
    return offset <= end && size <= end - offset && offset <= available && size <= available - offset;
}

static int unsupported(struct dppd_packet *packet)
{
    packet->ipv6_unsupported = true;
    packet->l4_type = DPPD_L4_OTHER;
    return DPPD_PARSE_UNSUPPORTED;
}

/** 长度始终以 IPv6 声明的负载为界，不能把以太网填充或下一段外部内存当作头部 */
int dppd_ipv6_parse(const uint8_t *data, uint32_t available, uint32_t length, struct dppd_packet *packet)
{
    uint32_t start = packet->l2_len;
    if (!covered(start, 40, available, length) || data[start] >> 4 != 6)
        return DPPD_PARSE_MALFORMED;
    packet->l3_type = DPPD_L3_IPV6;
    packet->l3_len = 40;
    packet->ipv6_payload_len = word(data + start + 4);
    memcpy(packet->ipv6_src, data + start + 8, 16);
    memcpy(packet->ipv6_dst, data + start + 24, 16);
    uint32_t offset = start + 40, end = offset + packet->ipv6_payload_len;
    uint8_t next = data[start + 6];
    packet->ip_protocol = next;
    if (end > length)
        return DPPD_PARSE_MALFORMED;
    if (packet->ipv6_payload_len == 0 && next == 0)
        return unsupported(packet);
    unsigned int extensions = 0, extension_bytes = 0;
    bool hop = false, destination = false;
    while (next == 0 || next == 60 || next == 44) {
        if (++extensions > 8)
            return unsupported(packet);
        if (!covered(offset, 2, available, end))
            return DPPD_PARSE_MALFORMED;
        uint32_t size = next == 44 ? 8 : ((uint32_t)data[offset + 1] + 1) * 8;
        if (size > end - offset)
            return DPPD_PARSE_MALFORMED;
        if (extension_bytes + size > 256)
            return unsupported(packet);
        if (!covered(offset, size, available, end))
            return DPPD_PARSE_MALFORMED;
        if (next == 44) {
            if (data[offset + 1] != 0 || (word(data + offset + 2) & 6U) != 0)
                return DPPD_PARSE_MALFORMED;
            packet->l3_len += 8;
            packet->ip_protocol = data[offset];
            packet->l4_type = DPPD_L4_FRAGMENT;
            return DPPD_PARSE_OK;
        }
        /** 仅接受首个 Hop-by-Hop 和一个目的选项头，未知选项、路由头与安全头不猜测语义 */
        if ((next == 0 && (hop || offset != start + 40)) || (next == 60 && destination))
            return unsupported(packet);
        hop |= next == 0;
        destination |= next == 60;
        for (uint32_t option = offset + 2; option < offset + size;) {
            uint8_t type = data[option++];
            if (type == 0)
                continue;
            if (option == offset + size || data[option] > offset + size - option - 1)
                return DPPD_PARSE_MALFORMED;
            uint32_t padding = data[option++];
            if (type != 1)
                return unsupported(packet);
            for (uint32_t i = 0; i < padding; ++i)
                if (data[option + i] != 0)
                    return unsupported(packet);
            option += padding;
        }
        next = data[offset];
        packet->ip_protocol = next;
        packet->l3_len += (uint16_t)size;
        offset += size;
        extension_bytes += size;
    }
    if (next == 43 || next == 50 || next == 51 || next == 135 || next == 139 || next == 140)
        return unsupported(packet);
    uint32_t remaining = end - offset;
    uint16_t minimum = next == 17 ? 8 : next == 6 ? 20 : next == 58 ? 4 : 0;
    if (minimum != 0 && !covered(offset, minimum, available, end))
        return DPPD_PARSE_MALFORMED;
    /** Echo 请求和回包还必须包含标识与序号，不能把只有通用四字节首部的截断包视为完整 Echo */
    if (next == 58 && (data[offset] == 128 || data[offset] == 129) && remaining < 8)
        return DPPD_PARSE_MALFORMED;
    if (next == 17) {
        if (word(data + offset + 4) < 8 || word(data + offset + 4) != remaining)
            return DPPD_PARSE_MALFORMED;
        packet->l4_type = DPPD_L4_UDP;
        packet->l4_len = 8;
    } else if (next == 6) {
        uint16_t size = (uint16_t)(data[offset + 12] >> 4) * 4;
        if (size < 20 || !covered(offset, size, available, end))
            return DPPD_PARSE_MALFORMED;
        packet->l4_type = DPPD_L4_TCP;
        packet->l4_len = size;
        packet->tcp_flags = data[offset + 13];
        memcpy(&packet->tcp_sequence_be, data + offset + 4, 4);
        memcpy(&packet->tcp_acknowledgment_be, data + offset + 8, 4);
    } else {
        packet->l4_type = DPPD_L4_OTHER;
        packet->l4_len = minimum;
    }
    if (next == 6 || next == 17) {
        memcpy(&packet->l4_src_port_be, data + offset, 2);
        memcpy(&packet->l4_dst_port_be, data + offset + 2, 2);
    }
    return DPPD_PARSE_OK;
}
