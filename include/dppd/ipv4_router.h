#ifndef DPPD_IPV4_ROUTER_H
#define DPPD_IPV4_ROUTER_H

#include <stddef.h>
#include <stdint.h>
#include "dppd/config.h"

#define DPPD_IPV4_ROUTE_LIMIT 256U
#define DPPD_IPV4_NEIGHBOR_LIMIT 256U
#define DPPD_IPV4_RETIRED_LIMIT 64U

struct dppd_ipv4_router;

/**
 * 一条 IPv4 路由，按目的地址前缀确定出口和下一跳
 * 字段使用无符号整数：uint32_t 占 4 字节，uint16_t 占 2 字节，uint8_t 占 1 字节
 *
 * 示例：目的网段 192.168.1.0/24 → 出口网口 1 → 下一跳 192.168.1.1
 *   prefix_be  = htonl(0xC0A80100U)
 *   gateway_be = htonl(0xC0A80101U)
 *   port_id    = 1
 *   depth      = 24
 *   reserved   = 0
 *
 * IPv4 地址四段各占 1 字节：192.168.1.0 → C0 A8 01 00，共 4 字节
 * htonl 将整数转换为网络字节序，保证内存中的地址字节与报文顺序一致
 */
struct dppd_ipv4_route {
    /** 网络前缀，按网络字节序保存，前缀长度之外的位必须为 0 */
    uint32_t prefix_be;
    /** 下一跳网关的 IPv4 地址，网络字节序；为 0 时直连，使用报文目的 IP 查邻居 */
    uint32_t gateway_be;
    /** 出口网口的实际 ID，须属于路由器创建时指定的网口集合 */
    uint16_t port_id;
    /** 前缀长度，单位为位，范围 0～32；0 表示默认路由 */
    uint8_t depth;
    /** 保留字段，配置时必须为 0 */
    uint8_t reserved;
};

/**
 * 保存下一跳 IP 与 MAC 的对应关系，由出口网口和下一跳 IP 共同定位
 * address_be 使用 4 字节无符号整数，port_id 使用 2 字节无符号整数，mac 保存 6 个字节
 *
 * 承接上述路由：在出口网口 1 上，192.168.1.1 对应 MAC 02:11:22:33:44:55
 *   address_be = htonl(0xC0A80101U)
 *   port_id    = 1
 *   mac        = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55}
 *
 * 路由确定出口和下一跳 IP，邻居提供发往该下一跳时使用的以太网目的 MAC
 * 同一 IP 在不同出口可对应不同邻居，因此查询时必须同时匹配 port_id
 */
struct dppd_ipv4_neighbor {
    /** 邻居 IPv4 地址，网络字节序，与 port_id 共同作为查询键 */
    uint32_t address_be;
    /** 邻居所在的出口网口 ID，同一 IP 在不同出口可对应不同邻居 */
    uint16_t port_id;
    /** 邻居的 6 字节单播 MAC，用于填写发出报文的以太网目的地址 */
    uint8_t mac[6];
};

/** IPv6 地址按字节保存，下一跳全零表示直连，链路本地下一跳由 port_id 区分作用域 */
struct dppd_ipv6_route {
    /** 16 字节网络前缀，按报文字节顺序保存，前缀长度之外的位必须为 0 */
    uint8_t prefix[16];
    /** 16 字节下一跳地址，全 0 时直连，使用报文目的 IPv6 地址查邻居 */
    uint8_t gateway[16];
    /** 出口网口 ID，也用于区分链路本地下一跳所属的链路 */
    uint16_t port_id;
    /** 前缀长度，单位为位，范围 0～128；0 表示默认路由 */
    uint8_t depth;
    /** 保留字段，配置时必须为 0 */
    uint8_t reserved;
};

/** 一条静态 IPv6 邻居，由出口网口和邻居地址共同定位 */
struct dppd_ipv6_neighbor {
    /** 邻居 IPv6 地址，按报文字节顺序保存，占 16 字节 */
    uint8_t address[16];
    /** 邻居所在的出口网口 ID，与 address 共同作为查询键 */
    uint16_t port_id;
    /** 邻居的 6 字节单播 MAC，用于填写发出报文的以太网目的地址 */
    uint8_t mac[6];
};

/** 路由和邻居共用版本，未使用的数组元素由实现清零，不直接作为磁盘格式 */
struct dppd_ipv4_configuration {
    /** 双栈路由和邻居共用的配置版本，替换发布时递增 */
    uint64_t generation;
    /** routes 中有效 IPv4 路由的条数 */
    uint32_t route_count;
    /** neighbors 中有效 IPv4 静态邻居的条数 */
    uint32_t neighbor_count;
    /** IPv4 路由数组，仅前 route_count 项有效 */
    struct dppd_ipv4_route routes[DPPD_IPV4_ROUTE_LIMIT];
    /** IPv4 静态邻居数组，仅前 neighbor_count 项有效 */
    struct dppd_ipv4_neighbor neighbors[DPPD_IPV4_NEIGHBOR_LIMIT];
    /** routes6 中有效 IPv6 路由的条数，与 IPv4 配置一起发布 */
    uint32_t route6_count;
    /** neighbors6 中有效 IPv6 静态邻居的条数 */
    uint32_t neighbor6_count;
    /** IPv6 路由数组，仅前 route6_count 项有效 */
    struct dppd_ipv6_route routes6[DPPD_IPV4_ROUTE_LIMIT];
    /** IPv6 静态邻居数组，仅前 neighbor6_count 项有效 */
    struct dppd_ipv6_neighbor neighbors6[DPPD_IPV4_NEIGHBOR_LIMIT];
};

/**
 * IPv4/IPv6 共用的单包路由结果，复制所需字段供后续改写，不持有快照内部指针
 * IPv6 缺少静态邻居时仍保留出口和下一跳，供 ND 继续解析，目的 MAC 尚待补齐
 */
struct dppd_ipv4_forward {
    /** 本次查询使用的路由和邻居配置版本 */
    uint64_t generation;
    /** IPv4 下一跳地址，网络字节序；经网关时为网关 IP，直连时为报文目的 IP */
    uint32_t next_hop_be;
    /** 查询选出的实际出口网口 ID，IPv4/IPv6 共用 */
    uint16_t port_id;
    /** 下一跳的目的 MAC，由静态邻居或 ND 解析取得，后续写入以太网目的地址 */
    uint8_t destination_mac[6];
    /** IPv6 下一跳地址，占 16 字节；IPv4 查询不使用此字段 */
    uint8_t next_hop6[16];
};

/** 初始配置为 NULL 时创建版本一的空表，恢复文件可传入已核验且版本非零的完整配置 */
int dppd_ipv4_router_create(struct dppd_ipv4_router **router, const uint16_t *ports, uint16_t port_count,
    const struct dppd_ipv4_configuration *initial);
/** 有读者仍在线时返回 EBUSY 并保留对象，调用方必须先停止工作线程 */
int dppd_ipv4_router_destroy(struct dppd_ipv4_router *router);
/** 控制面复制完整的规范配置，返回后不再借用任何快照内存 */
int dppd_ipv4_router_get(struct dppd_ipv4_router *router, struct dppd_ipv4_configuration *configuration);
/**
 * 按精确旧版本整表替换，generation 自动加一，输入中的 generation 不决定新版本
 * durable 在全部校验和分配成功后、发布前执行，可用来持久保存候选配置
 * 回调不得重入路由器接口，失败时不会发布新表，成功后不再执行可能失败的分配
 */
int dppd_ipv4_router_replace(struct dppd_ipv4_router *router, uint64_t expected_generation,
    const struct dppd_ipv4_configuration *configuration,
    int (*durable)(void *context, const struct dppd_ipv4_configuration *candidate), void *context);
/** 读者以固定 queue_id 注册，在一次完整端口扫描结束后报告静默点 */
int dppd_ipv4_router_reader_register(struct dppd_ipv4_router *router, unsigned int reader);
void dppd_ipv4_router_reader_quiescent(struct dppd_ipv4_router *router, unsigned int reader);
void dppd_ipv4_router_reader_unregister(struct dppd_ipv4_router *router, unsigned int reader);
/** 调用线程必须已注册，无路由返回 ENOENT，最长匹配的下一跳缺失返回 EHOSTUNREACH */
int dppd_ipv4_router_lookup(struct dppd_ipv4_router *router, uint32_t destination_be,
    struct dppd_ipv4_forward *forward);
int dppd_ipv6_router_lookup(struct dppd_ipv4_router *router, const uint8_t destination[16],
    struct dppd_ipv4_forward *forward);

#endif
