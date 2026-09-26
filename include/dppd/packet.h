#ifndef DPPD_PACKET_H
#define DPPD_PACKET_H

#include <stdbool.h>
#include <stdint.h>

struct rte_mbuf;

/**
 * 解析结果区分三种情况：已识别、暂不支持、报文结构异常
 * OK 不代表已解析全部协议，例如 IPv4 分片和其他传输层协议也可返回 OK
 * UNSUPPORTED 不等于报文损坏，调用方仍可使用已解析字段或按静态端口配对转发
 * MALFORMED 时不能依赖输出字段完整或已清零，应直接走畸形报文处理路径
 */
enum dppd_parse_status {
    DPPD_PARSE_OK = 0,
    DPPD_PARSE_UNSUPPORTED = 1,
    DPPD_PARSE_MALFORMED = -1,
};

/** 已识别的网络层类型，NONE 表示尚未识别，不能据此读取 IPv4 专用字段 */
enum dppd_l3_type {
    DPPD_L3_NONE = 0,
    DPPD_L3_ARP,
    DPPD_L3_IPV4,
};

/** 传输层分类；FRAGMENT 表示未重组的 IPv4 分片，不提供可用于 TCP 或 UDP 匹配的端口 */
enum dppd_l4_type {
    DPPD_L4_NONE = 0,
    DPPD_L4_UDP,
    DPPD_L4_TCP,
    DPPD_L4_FRAGMENT,
    DPPD_L4_OTHER,
};

/**
 * 从报文头提取的字段副本，不保存原始报文指针，也不拥有报文内存
 * 软件规则匹配使用这里的字段，读取协议专用字段前必须先检查对应的协议类型
 */
struct dppd_packet {
    /** 整个报文的长度，供流量统计使用，不等于本次复制或读取的报文头长度 */
    uint32_t packet_len;
    /** 已剥离支持范围内的 VLAN 标签后得到的协议类型，按主机数值保存 */
    uint16_t ether_type;
    /** 各层头部的字节数，l2_len 包含 VLAN 标签，l3_len 和 l4_len 不包含该层负载 */
    uint16_t l2_len;
    uint16_t l3_len;
    uint16_t l4_len;
    /** 最多保存两层 VLAN 的完整 TCI 数值，包含优先级等位，不能直接当作 VLAN ID */
    uint16_t vlan_tci[2];
    uint8_t vlan_depth;
    enum dppd_l3_type l3_type;
    enum dppd_l4_type l4_type;
    /** IPv4 头中的原始协议编号，即使报文是分片也会记录 */
    uint8_t ip_protocol;
    /** 地址与端口保留网络字节序，便于与规则中同样格式的值按位匹配，不能直接当作主机数值 */
    uint32_t ipv4_src_be;
    uint32_t ipv4_dst_be;
    uint16_t l4_src_port_be;
    uint16_t l4_dst_port_be;
    /** IPv4 的 MF 位，表示后面还有分片；末尾分片此位为假，但仍可能属于 FRAGMENT */
    bool ipv4_more_fragments;
};

/**
 * 从连续内存解析报文头，data 必须覆盖 available_len 个可读取字节
 * packet_len 是整包长度，可以大于 available_len，负载无需全部放进连续缓冲区
 * 每次读取头部都会同时检查两个长度；所需头部不完整时返回 MALFORMED
 * 本函数不验证校验和、不重组分片，也不解析应用层内容，packet 必须为有效输出对象
 */
int dppd_packet_parse_buffer(const uint8_t *data,
                             uint32_t available_len,
                             uint32_t packet_len,
                             struct dppd_packet *packet);
/** 从单段或多段 mbuf 提取有限长度的连续报文头后解析，不修改、不释放原始报文 */
int dppd_packet_parse_mbuf(const struct rte_mbuf *mbuf, struct dppd_packet *packet);

#endif
