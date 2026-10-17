#include "dppd/ipv4_rewrite.h"

#include <errno.h>
#include <string.h>
#include <rte_mbuf.h>

static uint16_t word(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] << 8 | bytes[1]);
}

static uint32_t sum_bytes(uint32_t sum, const uint8_t *bytes, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i)
        sum += (uint32_t)bytes[i] << (i & 1U ? 0 : 8);
    while (sum >> 16)
        sum = (sum & UINT16_MAX) + (sum >> 16);
    return sum;
}

/** 分块读取全传输层，偶数字节块保持奇数长度负载的校验和进位次序，不要求 mbuf 连续 */
int dppd_ipv4_transport_validate(const struct rte_mbuf *mbuf, const struct dppd_packet *packet)
{
    if (mbuf == NULL || packet == NULL || packet->packet_len != rte_pktmbuf_pkt_len(mbuf))
        return -EINVAL;
    if (packet->l3_type != DPPD_L3_IPV4 || (packet->l4_type != DPPD_L4_TCP && packet->l4_type != DPPD_L4_UDP))
        return -ENOTSUP;
    if (packet->l2_len < 14 || packet->l2_len > 22 || packet->l3_len < 20 || packet->l3_len > 60 ||
        (packet->l3_len & 3U) != 0)
        return -EINVAL;
    uint8_t header[102];
    uint16_t transport = packet->l4_type == DPPD_L4_TCP ? 20 : 8;
    uint16_t prefix = packet->l2_len + packet->l3_len;
    const uint8_t *data = rte_pktmbuf_read(mbuf, 0, prefix + transport, header);
    if (data == NULL)
        return -EINVAL;
    const uint8_t *ip = data + packet->l2_len, *l4 = data + prefix;
    uint16_t total = word(ip + 2);
    if ((ip[0] >> 4) != 4 || (ip[0] & 15U) * 4U != packet->l3_len || total < packet->l3_len + transport ||
        total > packet->packet_len - packet->l2_len || ip[9] != packet->ip_protocol ||
        ip[9] != (packet->l4_type == DPPD_L4_TCP ? 6 : 17) || memcmp(ip + 12, &packet->ipv4_src_be, 4) != 0 ||
        memcmp(ip + 16, &packet->ipv4_dst_be, 4) != 0 || memcmp(l4, &packet->l4_src_port_be, 2) != 0 ||
        memcmp(l4 + 2, &packet->l4_dst_port_be, 2) != 0)
        return -EINVAL;
    if ((word(ip + 6) & 0x3fffU) != 0)
        return -ENOTSUP;
    if (sum_bytes(0, ip, packet->l3_len) != UINT16_MAX)
        return -EBADMSG;
    if (ip[8] <= 1)
        return -ETIMEDOUT;
    bool ended = false;
    for (uint16_t i = 20; i < packet->l3_len; ++i) {
        if (ip[i] == 0)
            ended = true;
        else if (ended || ip[i] != 1)
            return -ENOTSUP;
    }
    uint16_t length = total - packet->l3_len;
    if (packet->l4_type == DPPD_L4_UDP) {
        length = word(l4 + 4);
        if (length < 8 || length > total - packet->l3_len)
            return -EINVAL;
        if (word(l4 + 6) == 0)
            return 0;
    } else if ((l4[12] >> 4) * 4U != packet->l4_len || packet->l4_len < 20 || packet->l4_len > length)
        return -EINVAL;
    uint32_t sum = sum_bytes(0, ip + 12, 8) + ip[9] + length;
    uint8_t bytes[256];
    for (uint32_t offset = 0; offset < length;) {
        uint32_t size = length - offset;
        if (size > sizeof(bytes))
            size = sizeof(bytes);
        const uint8_t *chunk = rte_pktmbuf_read(mbuf, prefix + offset, size, bytes);
        if (chunk == NULL)
            return -EINVAL;
        sum = sum_bytes(sum, chunk, size);
        offset += size;
    }
    return sum == UINT16_MAX ? 0 : -EBADMSG;
}
