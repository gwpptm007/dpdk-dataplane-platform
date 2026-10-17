#include "dppd/ipv4_rewrite.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <rte_mbuf.h>

static uint16_t checksum(const uint8_t *header, uint16_t length)
{
    uint32_t sum = 0;
    for (uint16_t i = 0; i < length; i += 2)
        sum += (uint32_t)header[i] << 8 | header[i + 1];
    while (sum >> 16)
        sum = (sum & 0xffffU) + (sum >> 16);
    return (uint16_t)~sum;
}

static bool valid_mac(const uint8_t address[6])
{
    const uint8_t zero[6] = {0};
    return (address[0] & 1U) == 0 && memcmp(address, zero, sizeof(zero)) != 0;
}

/** 检查整条链，后续分段共享时也不能直接改写本报文看到的头部或借用的缓冲区 */
static bool needs_copy(const struct rte_mbuf *mbuf)
{
    for (const struct rte_mbuf *segment = mbuf; segment != NULL; segment = segment->next)
        if (!RTE_MBUF_DIRECT(segment) || RTE_MBUF_HAS_EXTBUF(segment) || rte_mbuf_refcnt_read(segment) != 1)
            return true;
    return false;
}

/** 此前 read 已核对完整前缀可读，独占链在此期间不变，写入无需合并整包或分配头部空间 */
static void write_header(struct rte_mbuf *mbuf, const uint8_t *header, uint16_t length)
{
    for (struct rte_mbuf *segment = mbuf; segment != NULL && length != 0; segment = segment->next) {
        uint16_t size = segment->data_len < length ? segment->data_len : length;
        memcpy(rte_pktmbuf_mtod(segment, uint8_t *), header, size);
        header += size;
        length -= size;
    }
}

int dppd_ipv4_rewrite(struct rte_mbuf **mbuf, const struct dppd_packet *packet, const struct dppd_ipv4_forward *forward,
    const uint8_t source_mac[6])
{
    if (mbuf == NULL || *mbuf == NULL || packet == NULL || forward == NULL || source_mac == NULL ||
        !valid_mac(source_mac) || !valid_mac(forward->destination_mac))
        return -EINVAL;
    if (packet->l3_type != DPPD_L3_IPV4 || packet->l2_len < 14 || packet->l2_len > 22 || packet->l3_len < 20 ||
        packet->l3_len > 60 || (packet->l3_len & 3U) != 0 || packet->packet_len != rte_pktmbuf_pkt_len(*mbuf))
        return -EINVAL;
    uint8_t header[82];
    uint16_t length = packet->l2_len + packet->l3_len;
    const void *source = rte_pktmbuf_read(*mbuf, 0, length, header);
    if (source == NULL)
        return -EINVAL;
    if (source != header)
        memcpy(header, source, length);
    uint8_t *ipv4 = header + packet->l2_len;
    uint16_t total = (uint16_t)((uint16_t)ipv4[2] << 8 | ipv4[3]);
    if (header[packet->l2_len - 2] != 8 || header[packet->l2_len - 1] != 0 || (ipv4[0] >> 4) != 4 ||
        (ipv4[0] & 15U) * 4U != packet->l3_len || total < packet->l3_len ||
        total > packet->packet_len - packet->l2_len || memcmp(ipv4 + 16, &packet->ipv4_dst_be, 4) != 0)
        return -EINVAL;
    if (checksum(ipv4, packet->l3_len) != 0)
        return -EBADMSG;
    if (ipv4[8] <= 1)
        return -ETIMEDOUT;
    bool ended = false;
    for (uint16_t i = 20; i < packet->l3_len; ++i) {
        if (ipv4[i] == 0)
            ended = true;
        else if (ended || ipv4[i] != 1)
            return -ENOTSUP;
    }
    /* 所有校验都已通过，先在局部头部完成改写，再取得独占报文副本 */
    memcpy(header, forward->destination_mac, 6);
    memcpy(header + 6, source_mac, 6);
    ipv4[8]--;
    ipv4[10] = ipv4[11] = 0;
    uint16_t updated = checksum(ipv4, packet->l3_len);
    ipv4[10] = (uint8_t)(updated >> 8);
    ipv4[11] = (uint8_t)updated;
    struct rte_mbuf *writable = *mbuf;
    if (needs_copy(writable)) {
        writable = rte_pktmbuf_copy(*mbuf, (*mbuf)->pool, 0, UINT32_MAX);
        if (writable == NULL)
            return -ENOMEM;
        if (needs_copy(writable)) {
            rte_pktmbuf_free(writable);
            return -ENOTSUP;
        }
    }
    write_header(writable, header, length);
    /* IPv4 校验和由软件写好，不能继承旧报文的发送卸载请求 */
    writable->ol_flags &= ~RTE_MBUF_F_TX_OFFLOAD_MASK;
    writable->tx_offload = 0;
    writable->l2_len = packet->l2_len;
    writable->l3_len = packet->l3_len;
    writable->l4_len = packet->l4_len;
    if (writable != *mbuf) {
        rte_pktmbuf_free(*mbuf);
        *mbuf = writable;
    }
    return 0;
}
