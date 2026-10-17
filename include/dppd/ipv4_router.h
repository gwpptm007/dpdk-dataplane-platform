#ifndef DPPD_IPV4_ROUTER_H
#define DPPD_IPV4_ROUTER_H

#include <stddef.h>
#include <stdint.h>
#include "dppd/config.h"

#define DPPD_IPV4_ROUTE_LIMIT 256U
#define DPPD_IPV4_NEIGHBOR_LIMIT 256U
#define DPPD_IPV4_RETIRED_LIMIT 64U

struct dppd_ipv4_router;

/** 地址保持网络字节序，depth 表示前缀长度，gateway 为零时直接查目的地址的静态邻居 */
struct dppd_ipv4_route {
    uint32_t prefix_be, gateway_be;
    uint16_t port_id;
    uint8_t depth, reserved;
};

/** 同一 IP 可以出现在不同出口，邻居由出口端口和下一跳 IP 共同定位 */
struct dppd_ipv4_neighbor {
    uint32_t address_be;
    uint16_t port_id;
    uint8_t mac[6];
};

/** 路由和邻居共用版本，未使用的数组元素由实现清零，不直接作为磁盘格式 */
struct dppd_ipv4_configuration {
    uint64_t generation;
    uint32_t route_count, neighbor_count;
    struct dppd_ipv4_route routes[DPPD_IPV4_ROUTE_LIMIT];
    struct dppd_ipv4_neighbor neighbors[DPPD_IPV4_NEIGHBOR_LIMIT];
};

/** 只复制单个报文需要的结果，不把快照内部指针交给后续报文改写步骤 */
struct dppd_ipv4_forward {
    uint64_t generation;
    uint32_t next_hop_be;
    uint16_t port_id;
    uint8_t destination_mac[6];
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

#endif
