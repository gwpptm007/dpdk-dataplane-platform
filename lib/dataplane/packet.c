#include "dppd/packet.h"

#include <stddef.h>
#include <string.h>

#define DPPD_ETHER_HEADER_LEN 14U
#define DPPD_VLAN_HEADER_LEN 4U
#define DPPD_ARP_HEADER_LEN 28U
#define DPPD_IPV4_MIN_HEADER_LEN 20U
#define DPPD_UDP_HEADER_LEN 8U
#define DPPD_TCP_MIN_HEADER_LEN 20U

#define DPPD_ETHER_TYPE_IPV4 0x0800U
#define DPPD_ETHER_TYPE_ARP 0x0806U
#define DPPD_ETHER_TYPE_VLAN 0x8100U
#define DPPD_ETHER_TYPE_QINQ 0x88a8U

#define DPPD_IPV4_OFFSET_MASK 0x1fffU
#define DPPD_IPV4_MORE_FRAGMENTS 0x2000U
#define DPPD_IPPROTO_TCP 6U
#define DPPD_IPPROTO_UDP 17U

static uint16_t read_be16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static int require_bytes(uint32_t offset,
                         uint32_t length,
                         uint32_t available_len,
                         uint32_t packet_len)
{
    if (offset > packet_len || length > packet_len - offset)
        return -1;
    if (offset > available_len || length > available_len - offset)
        return -1;
    return 0;
}

int dppd_packet_parse_buffer(const uint8_t *data,
                             uint32_t available_len,
                             uint32_t packet_len,
                             struct dppd_packet *packet)
{
    uint32_t offset;
    uint16_t ether_type;

    if (data == NULL || packet == NULL ||
        require_bytes(0, DPPD_ETHER_HEADER_LEN, available_len, packet_len) != 0)
        return DPPD_PARSE_MALFORMED;

    memset(packet, 0, sizeof(*packet));
    packet->packet_len = packet_len;
    packet->l2_len = DPPD_ETHER_HEADER_LEN;
    ether_type = read_be16(data + 12U);
    offset = DPPD_ETHER_HEADER_LEN;

    while ((ether_type == DPPD_ETHER_TYPE_VLAN || ether_type == DPPD_ETHER_TYPE_QINQ) &&
           packet->vlan_depth < 2U) {
        if (require_bytes(offset, DPPD_VLAN_HEADER_LEN, available_len, packet_len) != 0)
            return DPPD_PARSE_MALFORMED;
        packet->vlan_tci[packet->vlan_depth++] = read_be16(data + offset);
        ether_type = read_be16(data + offset + 2U);
        offset += DPPD_VLAN_HEADER_LEN;
        packet->l2_len = (uint16_t)offset;
    }

    if (ether_type == DPPD_ETHER_TYPE_VLAN || ether_type == DPPD_ETHER_TYPE_QINQ)
        return DPPD_PARSE_UNSUPPORTED;

    packet->ether_type = ether_type;
    if (ether_type == DPPD_ETHER_TYPE_ARP) {
        if (require_bytes(offset, DPPD_ARP_HEADER_LEN, available_len, packet_len) != 0)
            return DPPD_PARSE_MALFORMED;
        packet->l3_type = DPPD_L3_ARP;
        packet->l3_len = DPPD_ARP_HEADER_LEN;
        return DPPD_PARSE_OK;
    }

    if (ether_type != DPPD_ETHER_TYPE_IPV4)
        return DPPD_PARSE_UNSUPPORTED;

    {
        uint16_t total_length;
        uint16_t fragment;
        uint8_t ihl;
        uint8_t protocol;

        if (require_bytes(offset, DPPD_IPV4_MIN_HEADER_LEN, available_len, packet_len) != 0)
            return DPPD_PARSE_MALFORMED;
        if ((data[offset] >> 4) != 4U)
            return DPPD_PARSE_MALFORMED;
        ihl = (uint8_t)((data[offset] & 0x0fU) * 4U);
        if (ihl < DPPD_IPV4_MIN_HEADER_LEN ||
            require_bytes(offset, ihl, available_len, packet_len) != 0)
            return DPPD_PARSE_MALFORMED;

        total_length = read_be16(data + offset + 2U);
        if (total_length < ihl || total_length > packet_len - offset)
            return DPPD_PARSE_MALFORMED;

        packet->l3_type = DPPD_L3_IPV4;
        packet->l3_len = ihl;
        protocol = data[offset + 9U];
        packet->ip_protocol = protocol;
        memcpy(&packet->ipv4_src_be, data + offset + 12U, sizeof(packet->ipv4_src_be));
        memcpy(&packet->ipv4_dst_be, data + offset + 16U, sizeof(packet->ipv4_dst_be));
        fragment = read_be16(data + offset + 6U);
        offset += ihl;

        packet->ipv4_more_fragments = (fragment & DPPD_IPV4_MORE_FRAGMENTS) != 0;
        if ((fragment & (DPPD_IPV4_MORE_FRAGMENTS | DPPD_IPV4_OFFSET_MASK)) != 0) {
            packet->l4_type = DPPD_L4_FRAGMENT;
            return DPPD_PARSE_OK;
        }

        if (protocol == DPPD_IPPROTO_UDP) {
            const uint32_t l4_available = (uint32_t)total_length - ihl;
            uint16_t udp_length;

            if (l4_available < DPPD_UDP_HEADER_LEN ||
                require_bytes(offset, DPPD_UDP_HEADER_LEN, available_len, packet_len) != 0)
                return DPPD_PARSE_MALFORMED;
            udp_length = read_be16(data + offset + 4U);
            if (udp_length < DPPD_UDP_HEADER_LEN || udp_length > l4_available)
                return DPPD_PARSE_MALFORMED;
            packet->l4_type = DPPD_L4_UDP;
            packet->l4_len = DPPD_UDP_HEADER_LEN;
            memcpy(&packet->l4_src_port_be, data + offset, sizeof(packet->l4_src_port_be));
            memcpy(&packet->l4_dst_port_be,
                   data + offset + 2U,
                   sizeof(packet->l4_dst_port_be));
            return DPPD_PARSE_OK;
        }

        if (protocol == DPPD_IPPROTO_TCP) {
            const uint32_t l4_available = (uint32_t)total_length - ihl;
            uint8_t tcp_length;

            if (l4_available < DPPD_TCP_MIN_HEADER_LEN ||
                require_bytes(offset, DPPD_TCP_MIN_HEADER_LEN, available_len, packet_len) != 0)
                return DPPD_PARSE_MALFORMED;
            tcp_length = (uint8_t)((data[offset + 12U] >> 4) * 4U);
            if (tcp_length < DPPD_TCP_MIN_HEADER_LEN || tcp_length > l4_available ||
                require_bytes(offset, tcp_length, available_len, packet_len) != 0)
                return DPPD_PARSE_MALFORMED;
            packet->l4_type = DPPD_L4_TCP;
            packet->l4_len = tcp_length;
            memcpy(&packet->l4_src_port_be, data + offset, sizeof(packet->l4_src_port_be));
            memcpy(&packet->l4_dst_port_be,
                   data + offset + 2U,
                   sizeof(packet->l4_dst_port_be));
            return DPPD_PARSE_OK;
        }

        packet->l4_type = DPPD_L4_OTHER;
        return DPPD_PARSE_OK;
    }
}
