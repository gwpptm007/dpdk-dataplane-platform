#include "dppd/ipv6.h"

#include <errno.h>
#include <string.h>
#include <rte_mbuf.h>

static uint32_t sum(uint32_t value, const uint8_t *bytes, uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i)
        value += (uint32_t)bytes[i] << (i & 1U ? 0 : 8);
    while (value >> 16)
        value = (value & UINT16_MAX) + (value >> 16);
    return value;
}

/** 使用最终传输协议和扩展头之后的长度构造伪首部，奇数负载在最后一块补零求和 */
static int transport(const struct rte_mbuf *mbuf, const struct dppd_packet *packet)
{
    uint8_t protocol = packet->ip_protocol;
    if (protocol != 6 && protocol != 17 && protocol != 58)
        return 0;
    uint32_t length = 40U + packet->ipv6_payload_len - packet->l3_len;
    uint32_t prefix = packet->l2_len + packet->l3_len;
    if (protocol != 6) {
        uint8_t head[8];
        const uint8_t *first = rte_pktmbuf_read(mbuf, prefix, protocol == 17 ? 8 : 4, head);
        if (first == NULL)
            return -EINVAL;
        if (protocol == 17 && first[6] == 0 && first[7] == 0)
            return -EBADMSG;
        if (protocol == 58 && first[0] >= 133 && first[0] <= 137)
            return -ENOTSUP;
    }
    uint32_t value = sum(sum(0, packet->ipv6_src, 16), packet->ipv6_dst, 16) + protocol + length;
    uint8_t scratch[256];
    for (uint32_t offset = 0; offset < length;) {
        uint32_t size = length - offset > sizeof(scratch) ? sizeof(scratch) : length - offset;
        const uint8_t *bytes = rte_pktmbuf_read(mbuf, prefix + offset, size, scratch);
        if (bytes == NULL)
            return -EINVAL;
        value = sum(value, bytes, size);
        offset += size;
    }
    return value == UINT16_MAX ? 0 : -EBADMSG;
}

static bool valid_mac(const uint8_t *mac)
{
    const uint8_t zero[6] = {0};
    return (mac[0] & 1U) == 0 && memcmp(mac, zero, 6) != 0;
}

int dppd_ipv6_rewrite(struct rte_mbuf **mbuf, const struct dppd_packet *packet, const struct dppd_ipv4_forward *forward,
    const uint8_t source_mac[6])
{
    if (mbuf == NULL || *mbuf == NULL || packet == NULL || forward == NULL || source_mac == NULL ||
        !valid_mac(source_mac) || !valid_mac(forward->destination_mac))
        return -EINVAL;
    struct dppd_packet actual = {0};
    int parsed = dppd_packet_parse_mbuf(*mbuf, &actual);
    if (parsed == DPPD_PARSE_MALFORMED || actual.l3_type != DPPD_L3_IPV6 || packet->l3_type != DPPD_L3_IPV6 ||
        packet->packet_len != actual.packet_len || packet->l3_len != actual.l3_len || packet->l4_len != actual.l4_len ||
        packet->l2_len != actual.l2_len || packet->ip_protocol != actual.ip_protocol ||
        memcmp(packet->ipv6_src, actual.ipv6_src, 16) != 0 || memcmp(packet->ipv6_dst, actual.ipv6_dst, 16) != 0)
        return -EINVAL;
    if (actual.ipv6_unsupported || actual.l4_type == DPPD_L4_FRAGMENT)
        return -ENOTSUP;
    if (!dppd_ipv6_unicast(actual.ipv6_src, false) || !dppd_ipv6_unicast(actual.ipv6_dst, false))
        return -ENOTSUP;
    uint8_t header[62];
    uint16_t size = actual.l2_len + 40;
    const void *data = rte_pktmbuf_read(*mbuf, 0, size, header);
    if (data == NULL)
        return -EINVAL;
    if (data != header)
        memcpy(header, data, size);
    if (header[actual.l2_len + 7] <= 1)
        return -ETIMEDOUT;
    int rc = transport(*mbuf, &actual);
    if (rc != 0)
        return rc;
    memcpy(header, forward->destination_mac, 6);
    memcpy(header + 6, source_mac, 6);
    header[actual.l2_len + 7]--;
    bool copy = false;
    /** 后续分段也可能共享或借用外部内存，必须先检查整条链再决定是否复制 */
    for (const struct rte_mbuf *part = *mbuf; part != NULL; part = part->next)
        copy |= !RTE_MBUF_DIRECT(part) || RTE_MBUF_HAS_EXTBUF(part) || rte_mbuf_refcnt_read(part) != 1;
    struct rte_mbuf *writable = copy ? rte_pktmbuf_copy(*mbuf, (*mbuf)->pool, 0, UINT32_MAX) : *mbuf;
    if (writable == NULL)
        return -ENOMEM;
    for (struct rte_mbuf *part = writable; part != NULL; part = part->next)
        if (!RTE_MBUF_DIRECT(part) || RTE_MBUF_HAS_EXTBUF(part) || rte_mbuf_refcnt_read(part) != 1) {
            if (copy)
                rte_pktmbuf_free(writable);
            return -ENOTSUP;
        }
    const uint8_t *cursor = header;
    for (struct rte_mbuf *part = writable; part != NULL && size != 0; part = part->next) {
        uint16_t bytes = part->data_len < size ? part->data_len : size;
        memcpy(rte_pktmbuf_mtod(part, uint8_t *), cursor, bytes);
        cursor += bytes;
        size -= bytes;
    }
    writable->ol_flags &= ~RTE_MBUF_F_TX_OFFLOAD_MASK;
    writable->tx_offload = 0;
    writable->l2_len = actual.l2_len;
    writable->l3_len = actual.l3_len;
    writable->l4_len = actual.l4_len;
    if (copy) {
        rte_pktmbuf_free(*mbuf);
        *mbuf = writable;
    }
    return 0;
}
