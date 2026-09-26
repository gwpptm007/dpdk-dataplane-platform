#include "dppd/packet.h"

#include <stdint.h>
#include <rte_mbuf.h>

/**
 * 只准备固定大小的报文头缓冲区，不复制整包负载
 * 以太网头、两层 VLAN、最长 IPv4 头和最长 TCP 头合计 142 字节，当前解析范围可容纳
 */
#define DPPD_HEADER_SCRATCH 256U

/** 将可能分散在多个内存段的报文头交给连续缓冲区解析器，保持原始 mbuf 的布局不变 */
int dppd_packet_parse_mbuf(const struct rte_mbuf *mbuf, struct dppd_packet *packet)
{
    uint8_t scratch[DPPD_HEADER_SCRATCH];
    uint32_t packet_len;
    uint32_t read_len;
    const void *data;

    if (mbuf == NULL || packet == NULL)
        return DPPD_PARSE_MALFORMED;

    /** 获取整条 mbuf 链的总长度，不使用仅代表首段长度的字段 */
    packet_len = rte_pktmbuf_pkt_len(mbuf);
    read_len = packet_len < sizeof(scratch) ? packet_len : sizeof(scratch);
    /**
     * 请求范围在一段内连续时直接借用原始指针，跨段时由 DPDK 复制到 scratch
     * 两种情况都使用返回的 data，不能假定 scratch 一定已写入数据
     */
    data = rte_pktmbuf_read(mbuf, 0, read_len, scratch);
    if (data == NULL)
        return DPPD_PARSE_MALFORMED;
    /** 在局部缓冲区失效前完成解析；输出只保存字段值，不把 data 指针带出函数 */
    return dppd_packet_parse_buffer(data, read_len, packet_len, packet);
}
