#include "dppd/packet.h"

#include <stdint.h>
#include <rte_mbuf.h>

#define DPPD_HEADER_SCRATCH 256U

int dppd_packet_parse_mbuf(const struct rte_mbuf *mbuf, struct dppd_packet *packet)
{
    uint8_t scratch[DPPD_HEADER_SCRATCH];
    uint32_t packet_len;
    uint32_t read_len;
    const void *data;

    if (mbuf == NULL || packet == NULL)
        return DPPD_PARSE_MALFORMED;

    packet_len = rte_pktmbuf_pkt_len(mbuf);
    read_len = packet_len < sizeof(scratch) ? packet_len : sizeof(scratch);
    data = rte_pktmbuf_read(mbuf, 0, read_len, scratch);
    if (data == NULL)
        return DPPD_PARSE_MALFORMED;
    return dppd_packet_parse_buffer(data, read_len, packet_len, packet);
}
