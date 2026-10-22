#include "dppd/packet.h"
#include "dppd/ipv6.h"
#include "dppd/tunnel.h"

#include <stddef.h>
#include <string.h>

#define DPPD_ETHER_HEADER_LEN 14U /** 目的 MAC 6 + 源 MAC 6 + 类型 2，不含 VLAN 标签 */
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

/**
 * 将网络中高字节在前的两个字节拼成主机可直接比较的数值，用于长度和协议编号
 * 逐字节读取避免未对齐访问，调用方必须先确认这两个字节都在可读范围内
 */
static uint16_t read_be16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

/**
 * 检查从 offset 开始的 length 个字节既属于整包，也实际存在于当前连续缓冲区
 * 先检查偏移，再用减法检查剩余长度，避免 offset 加 length 时发生整数溢出
 */
static int require_bytes(uint32_t offset, uint32_t length, uint32_t available_len, uint32_t packet_len)
{
    if (offset > packet_len || length > packet_len - offset)
        return -1;
    if (offset > available_len || length > available_len - offset)
        return -1;
    return 0;
}

/** 逐层解析以太网、VLAN、IP 与传输层，将字段写入 packet 并返回解析状态 */
int dppd_packet_parse_headers(const uint8_t *data, uint32_t available_len, uint32_t packet_len,
    struct dppd_packet *packet)
{
    uint32_t offset;
    uint16_t ether_type;

    if (data == NULL || packet == NULL || require_bytes(0, DPPD_ETHER_HEADER_LEN, available_len, packet_len) != 0)
        return DPPD_PARSE_MALFORMED;

    /** 初始以太网头检查成功后才清零输出，后续未解析的字段保持默认值 */
    memset(packet, 0, sizeof(*packet));
    packet->connection_state = DPPD_CT_UNTRACKED;
    packet->packet_len = packet_len;
    packet->l2_len = DPPD_ETHER_HEADER_LEN;
    /**
     * 无 VLAN 的 IPv4 报文布局
     * 整包偏移   0～5         6～11        12～13       14 开始
     * 内容       目的 MAC     源 MAC       协议类型      IPv4 头
     *            └──────── 以太网头 14 字节 ────────┘
     */
    ether_type = read_be16(data + 12U);
    offset = DPPD_ETHER_HEADER_LEN;

    /** 每层标签包含 TCI 和下一层协议类型，逐层跳过，最多读取两层 */
    while ((ether_type == DPPD_ETHER_TYPE_VLAN || ether_type == DPPD_ETHER_TYPE_QINQ) && packet->vlan_depth < 2U) {
        if (require_bytes(offset, DPPD_VLAN_HEADER_LEN, available_len, packet_len) != 0)
            return DPPD_PARSE_MALFORMED;
        packet->vlan_tpid[packet->vlan_depth] = ether_type;
        packet->vlan_tci[packet->vlan_depth++] = read_be16(data + offset);
        ether_type = read_be16(data + offset + 2U);
        offset += DPPD_VLAN_HEADER_LEN;
        packet->l2_len = (uint16_t)offset;
    }

    /** 两层之后仍是 VLAN 表示超出当前支持范围，不能误把下一层标签当作 IP 头 */
    if (ether_type == DPPD_ETHER_TYPE_VLAN || ether_type == DPPD_ETHER_TYPE_QINQ)
        return DPPD_PARSE_UNSUPPORTED;

    packet->ether_type = ether_type;
    /** ARP 这里只检查常见格式所需的 28 字节并记录类型，不进一步校验或提取其内部字段 */
    if (ether_type == DPPD_ETHER_TYPE_ARP) {
        if (require_bytes(offset, DPPD_ARP_HEADER_LEN, available_len, packet_len) != 0)
            return DPPD_PARSE_MALFORMED;
        packet->l3_type = DPPD_L3_ARP;
        packet->l3_len = DPPD_ARP_HEADER_LEN;
        return DPPD_PARSE_OK;
    }

    if (ether_type == 0x86ddU)
        return dppd_ipv6_parse(data, available_len, packet_len, packet);
    /** 其他以太网协议暂不深入解析，这种情况与报文头被截断不同 */
    if (ether_type != DPPD_ETHER_TYPE_IPV4)
        return DPPD_PARSE_UNSUPPORTED;

    {
        uint16_t total_length;
        uint16_t fragment;
        uint8_t ihl;
        uint8_t protocol;

        if (require_bytes(offset, DPPD_IPV4_MIN_HEADER_LEN, available_len, packet_len) != 0)
            return DPPD_PARSE_MALFORMED;
        /**
         * IPv4 首字节同时保存版本与 IHL，IHL 以 4 字节为单位
         * 0x45 = 0100 0101
         *        └──┘ └──┘
         *        版本 IHL
         *         4    5 × 4 = 20 字节
         * IHL 包含选项长度，后续按实际头长检查可读范围
         */
        if ((data[offset] >> 4) != 4U)
            return DPPD_PARSE_MALFORMED;
        ihl = (uint8_t)((data[offset] & 0x0fU) * 4U);
        if (ihl < DPPD_IPV4_MIN_HEADER_LEN || require_bytes(offset, ihl, available_len, packet_len) != 0)
            return DPPD_PARSE_MALFORMED;

        /**
         * IP 头偏移 2 处的两字节是总长度，包含 IP 头和负载，须介于头长与整包剩余长度之间
         * 本例：[以太网头 14][IPv4 头 20 | UDP 头 8 | 数据 18]
         * 整包 packet_len = 60，IP 总长 total_length = 46，IP 头长 ihl = 20
         * 不要求全部负载位于当前连续缓冲区，IP 声明范围以整包长度校验
         * 以太网帧末尾可能有填充，因此允许 IP 总长度小于整包剩余长度
         */
        total_length = read_be16(data + offset + 2U);
        if (total_length < ihl || total_length > packet_len - offset)
            return DPPD_PARSE_MALFORMED;

        packet->l3_type = DPPD_L3_IPV4;
        packet->l3_len = ihl;
        packet->ipv4_total_len = total_length;
        /**
         * IP 头内偏移   9         12..15       16..19
         * 字段          协议号    源地址       目的地址
         * 本例：源地址起点 14 + 12 = 26，UDP 头起点 14 + 20 = 34
         * 地址原样复制，保留网络字节序并避免未对齐访问
         */
        protocol = data[offset + 9U];
        packet->ip_protocol = protocol;
        memcpy(&packet->ipv4_src_be, data + offset + 12U, sizeof(packet->ipv4_src_be));
        memcpy(&packet->ipv4_dst_be, data + offset + 16U, sizeof(packet->ipv4_dst_be));
        fragment = read_be16(data + offset + 6U);
        offset += ihl;

        /**
         * 分片字段：[保留 1 位][DF 1 位][MF 1 位][分片偏移 13 位]
         * DF：禁止分片；MF：还有后续分片
         * MF = 1 或分片偏移非零 → 标记 FRAGMENT，返回 OK
         * 首片带有 UDP/TCP 头，也不提取端口，与后续分片统一处理
         */
        packet->ipv4_more_fragments = (fragment & DPPD_IPV4_MORE_FRAGMENTS) != 0;
        if ((fragment & (DPPD_IPV4_MORE_FRAGMENTS | DPPD_IPV4_OFFSET_MASK)) != 0) {
            packet->l4_type = DPPD_L4_FRAGMENT;
            return DPPD_PARSE_OK;
        }

        if (protocol == DPPD_IPPROTO_UDP) {
            /**
             * 本例：[IP 头 20][UDP 头 8 | 数据 18]，IP 总长 46
             * l4_available = 46 - 20 = 26，包含 UDP 头和 UDP 数据
             * 以 IP 声明的长度为上限，避免把以太网填充算作 UDP 数据
             */
            const uint32_t l4_available = (uint32_t)total_length - ihl;
            uint16_t udp_length;

            if (l4_available < DPPD_UDP_HEADER_LEN ||
                require_bytes(offset, DPPD_UDP_HEADER_LEN, available_len, packet_len) != 0)
                return DPPD_PARSE_MALFORMED;
            /**
             * UDP 头内偏移   0..1      2..3      4..5      6..7
             * 字段          源端口    目的端口  UDP 总长  校验和
             * UDP 头固定 8 字节，长度字段包含头和数据，本例为 8 + 18 = 26
             * UDP 总长不能小于头长，也不能超过 IP 负载长度 l4_available
             */
            udp_length = read_be16(data + offset + 4U);
            if (udp_length < DPPD_UDP_HEADER_LEN || udp_length > l4_available)
                return DPPD_PARSE_MALFORMED;
            packet->l4_type = DPPD_L4_UDP;
            packet->l4_len = DPPD_UDP_HEADER_LEN;
            /**
             * 本例整包偏移：[34..35 源端口][36..37 目的端口]
             * 端口以网络字节序复制到 packet，修改副本不会写回原始报文
             */
            memcpy(&packet->l4_src_port_be, data + offset, sizeof(packet->l4_src_port_be));
            memcpy(&packet->l4_dst_port_be, data + offset + 2U, sizeof(packet->l4_dst_port_be));
            return DPPD_PARSE_OK;
        }

        if (protocol == DPPD_IPPROTO_TCP) {
            const uint32_t l4_available = (uint32_t)total_length - ihl;
            uint8_t tcp_length;

            if (l4_available < DPPD_TCP_MIN_HEADER_LEN ||
                require_bytes(offset, DPPD_TCP_MIN_HEADER_LEN, available_len, packet_len) != 0)
                return DPPD_PARSE_MALFORMED;
            /**
             * TCP 数据偏移以 4 字节为单位给出头长，包含可选字段，最少为 20 字节
             * 同时检查 IP 负载上限和连续缓冲区边界，避免把不完整的选项区视为完整头部
             */
            tcp_length = (uint8_t)((data[offset + 12U] >> 4) * 4U);
            if (tcp_length < DPPD_TCP_MIN_HEADER_LEN || tcp_length > l4_available ||
                require_bytes(offset, tcp_length, available_len, packet_len) != 0)
                return DPPD_PARSE_MALFORMED;
            packet->l4_type = DPPD_L4_TCP;
            packet->l4_len = tcp_length;
            packet->tcp_flags = data[offset + 13U];
            memcpy(&packet->tcp_sequence_be, data + offset + 4U, 4);
            memcpy(&packet->tcp_acknowledgment_be, data + offset + 8U, 4);
            memcpy(&packet->l4_src_port_be, data + offset, sizeof(packet->l4_src_port_be));
            memcpy(&packet->l4_dst_port_be, data + offset + 2U, sizeof(packet->l4_dst_port_be));
            return DPPD_PARSE_OK;
        }

        /** 已识别出合法的 IPv4 头，但不是当前解析的 UDP 或 TCP，保留 IP 字段供后续使用 */
        packet->l4_type = DPPD_L4_OTHER;
        return DPPD_PARSE_OK;
    }
}

/**
 * 解析报文头，将协议、地址、端口等字段写入 packet
 * 基础解析成功后补充隧道信息，返回基础解析状态
 */
int dppd_packet_parse_buffer(const uint8_t *data, uint32_t available_len, uint32_t packet_len,
    struct dppd_packet *packet)
{
    int rc = dppd_packet_parse_headers(data, available_len, packet_len, packet);
    if (rc == DPPD_PARSE_OK)
        (void)dppd_tunnel_parse_buffer(data, available_len, packet_len, packet);
    return rc;
}
