#include "dppd/tunnel.h"

#include <string.h>

static uint16_t word(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] << 8 | data[1]);
}

static uint32_t number(const uint8_t *data)
{
    return (uint32_t)word(data) << 16 | word(data + 2);
}

/** 先核对协议声明的结束位置，再核对当前观察窗口，窗口不足不能冒充线缆报文损坏 */
static int require(uint32_t offset, uint32_t size, uint32_t available, uint32_t end)
{
    if (offset > end || size > end - offset)
        return DPPD_PARSE_MALFORMED;
    if (offset > available || size > available - offset)
        return DPPD_PARSE_UNSUPPORTED;
    return DPPD_PARSE_OK;
}

static void copy_inner(struct dppd_tunnel_info *info, const struct dppd_packet *inner)
{
    info->inner_valid = true;
    info->inner_l3 = inner->l3_type;
    info->inner_l4 = inner->l4_type;
    info->inner_ip_protocol = inner->ip_protocol;
    info->inner_vlan_depth = inner->vlan_depth;
    memcpy(info->inner_vlan_tci, inner->vlan_tci, sizeof(info->inner_vlan_tci));
    memcpy(info->inner_vlan_tpid, inner->vlan_tpid, sizeof(info->inner_vlan_tpid));
    info->inner_src_be = inner->ipv4_src_be;
    info->inner_dst_be = inner->ipv4_dst_be;
    memcpy(info->inner_src6, inner->ipv6_src, 16);
    memcpy(info->inner_dst6, inner->ipv6_dst, 16);
    info->inner_src_port_be = inner->l4_src_port_be;
    info->inner_dst_port_be = inner->l4_dst_port_be;
}

/** 原始 IP 负载临时补一个以太网头复用基础解析器，临时副本不会进入发送路径 */
static int parse_inner(const uint8_t *data, uint32_t available, struct dppd_tunnel_info *info)
{
    struct dppd_packet inner;
    uint32_t offset = info->inner_offset, length = info->inner_length;
    if (available < offset)
        return DPPD_PARSE_UNSUPPORTED;
    uint32_t readable = available - offset;
    if (readable > length)
        readable = length;
    int rc;
    if (info->inner_protocol == 0x6558) {
        rc = dppd_packet_parse_headers(data + offset, readable, length, &inner);
    } else if (info->inner_protocol == 0x0800 || info->inner_protocol == 0x86dd) {
        uint8_t scratch[512] = {0};
        scratch[12] = (uint8_t)(info->inner_protocol >> 8);
        scratch[13] = (uint8_t)info->inner_protocol;
        if (readable > sizeof(scratch) - 14)
            readable = sizeof(scratch) - 14;
        memcpy(scratch + 14, data + offset, readable);
        rc = dppd_packet_parse_headers(scratch, readable + 14, length + 14, &inner);
    } else
        return DPPD_PARSE_UNSUPPORTED;
    if (rc == DPPD_PARSE_OK)
        copy_inner(info, &inner);
    if (rc == DPPD_PARSE_MALFORMED && readable < length)
        return DPPD_PARSE_UNSUPPORTED;
    return rc;
}

static int udp_tunnel(const uint8_t *data, uint32_t available, uint32_t start, uint32_t end,
    struct dppd_tunnel_info *info)
{
    int rc = require(start, 8, available, end);
    if (rc != DPPD_PARSE_OK)
        return rc;
    info->header_len = 8;
    info->identifier = number(data + start + 4) >> 8;
    if (info->type == DPPD_TUNNEL_VXLAN) {
        /** 标准 VXLAN 必须有 I 位，接收时忽略保留位，不把保留位当作新扩展格式解释 */
        if ((data[start] & 8U) == 0)
            return DPPD_PARSE_UNSUPPORTED;
        info->inner_protocol = 0x6558;
    } else {
        if (data[start] >> 6 != 0)
            return DPPD_PARSE_UNSUPPORTED;
        uint32_t options = (data[start] & 0x3fU) * 4U;
        rc = require(start + 8, options, available, end);
        if (rc != DPPD_PARSE_OK)
            return rc;
        info->header_len += (uint16_t)options;
        info->inner_protocol = word(data + start + 2);
        info->critical = (data[start + 1] & 0x40U) != 0;
        for (uint32_t option = start + 8; option < start + info->header_len;) {
            uint32_t size = 4U + (data[option + 3] & 0x1fU) * 4U;
            if (size > start + info->header_len - option)
                return DPPD_PARSE_MALFORMED;
            info->critical |= (data[option + 2] & 0x80U) != 0;
            option += size;
        }
    }
    info->inner_offset = start + info->header_len;
    info->inner_length = end - info->inner_offset;
    /** 当前没有已注册的 Geneve 选项解释器，关键选项不产生可供终端使用的内层字段 */
    if (info->critical)
        return DPPD_PARSE_UNSUPPORTED;
    return parse_inner(data, available, info);
}

static int gre_tunnel(const uint8_t *data, uint32_t available, uint32_t start, uint32_t end,
    struct dppd_tunnel_info *info)
{
    int rc = require(start, 4, available, end);
    if (rc != DPPD_PARSE_OK)
        return rc;
    uint16_t flags = word(data + start);
    /** 只支持 GRE v0 与 C/K/S 字段，旧路由、严格路由和未知版本不猜测可变头布局 */
    if ((flags & 0x4c07U) != 0)
        return DPPD_PARSE_UNSUPPORTED;
    info->has_checksum = (flags & 0x8000U) != 0;
    info->has_key = (flags & 0x2000U) != 0;
    info->has_sequence = (flags & 0x1000U) != 0;
    info->inner_protocol = word(data + start + 2);
    info->header_len = (uint16_t)(4 + 4 * info->has_checksum + 4 * info->has_key + 4 * info->has_sequence);
    rc = require(start, info->header_len, available, end);
    if (rc != DPPD_PARSE_OK)
        return rc;
    uint32_t field = start + 4 + 4 * info->has_checksum;
    if (info->has_key) {
        info->identifier = number(data + field);
        field += 4;
    }
    if (info->has_sequence)
        info->sequence = number(data + field);
    info->inner_offset = start + info->header_len;
    info->inner_length = end - info->inner_offset;
    return parse_inner(data, available, info);
}

int dppd_tunnel_parse_buffer(const uint8_t *data, uint32_t available, uint32_t length, struct dppd_packet *packet)
{
    if (data == NULL || packet == NULL)
        return DPPD_PARSE_MALFORMED;
    struct dppd_tunnel_info *info = &packet->tunnel;
    memset(info, 0, sizeof(*info));
    if ((packet->l3_type != DPPD_L3_IPV4 && packet->l3_type != DPPD_L3_IPV6) || packet->l4_type == DPPD_L4_FRAGMENT ||
        packet->ipv6_unsupported)
        return DPPD_PARSE_OK;
    uint32_t start = (uint32_t)packet->l2_len + packet->l3_len;
    uint32_t end =
        packet->l2_len + (packet->l3_type == DPPD_L3_IPV4 ? packet->ipv4_total_len : 40U + packet->ipv6_payload_len);
    if (end > length || start > end)
        return DPPD_PARSE_MALFORMED;
    if (packet->l4_type == DPPD_L4_UDP) {
        int rc = require(start, 8, available, end);
        if (rc != DPPD_PARSE_OK)
            return rc;
        uint16_t port = word(data + start + 2);
        if (port != 4789 && port != 6081)
            return DPPD_PARSE_OK;
        info->type = port == 4789 ? DPPD_TUNNEL_VXLAN : DPPD_TUNNEL_GENEVE;
        uint16_t udp_length = word(data + start + 4);
        if (udp_length < 8 || udp_length > end - start)
            return info->status = DPPD_PARSE_MALFORMED;
        end = start + udp_length;
        start += 8;
        return info->status = udp_tunnel(data, available, start, end, info);
    }
    if (packet->ip_protocol == 47) {
        info->type = DPPD_TUNNEL_GRE;
        return info->status = gre_tunnel(data, available, start, end, info);
    }
    return DPPD_PARSE_OK;
}
