#include "dppd/packet.h"

#include <stdint.h>
#include <rte_mbuf.h>

/**
 * 只准备固定大小的报文头缓冲区，不复制整包负载
 * 可容纳双 VLAN、IPv6 固定头、有界扩展头链和最长 TCP 头，不复制整个数据负载
 */
#define DPPD_HEADER_SCRATCH 1024U

/**
 * 提取单段或多段报文的头部字段，写入 packet 并返回解析状态
 * packet 不持有原始报文内存，解析不修改或释放 mbuf
 */
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
