#include "dppd/packet.h"

#include <stdint.h>
#include <rte_mbuf.h>

/**
 * 为报文头解析预留固定大小的备用缓冲区，限制一次请求读取的范围
 * 小包的读取范围可能覆盖整包，大包最多读取前 1024 字节，后续只解析所需字段
 */
#define DPPD_HEADER_SCRATCH 1024U

/**
 * 提取单段或多段报文的头部字段，写入 packet 并返回解析状态
 * packet.c 根据以太网类型和 IP 协议等字段逐层定位报文头，检查长度后提取地址和端口
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

    /** packet_len 是整条 mbuf 链的总长度，read_len 取总长度与缓冲区上限中的较小值 */
    packet_len = rte_pktmbuf_pkt_len(mbuf);
    read_len = packet_len < sizeof(scratch) ? packet_len : sizeof(scratch);
    /**
     * 用于安全、连续地读取网络报文数据（mbuf）的核心函数
     * 连续时返回mbuf原始报文数据指针，零拷贝且不写 scratch
     * 跨段时将 read_len 个字节复制到 scratch，再返回其数据指针，发生拷贝
     * 返回值指向报文字节而非 struct rte_mbuf 管理结构，实际读取位置统一以 data 为准
     */
    data = rte_pktmbuf_read(mbuf, 0, read_len, scratch);
    if (data == NULL)
        return DPPD_PARSE_MALFORMED;
    /** 在局部缓冲区失效前完成解析；输出只保存字段值，不把 data 指针带出函数 */
    return dppd_packet_parse_buffer(data, read_len, packet_len, packet);
}
