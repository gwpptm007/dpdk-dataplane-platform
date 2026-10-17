#ifndef DPPD_CONNTRACK_H
#define DPPD_CONNTRACK_H

#include "dppd/config.h"
#include "dppd/packet.h"

#define DPPD_CT_PAGE_SIZE 4U

struct dppd_conntrack;

/** 地址和端口保持网络字节序，protocol 使用 IP 协议编号，不比较结构体填充字节 */
struct dppd_flow_tuple {
    uint32_t source_be, destination_be;
    uint16_t source_port_be, destination_port_be;
    uint8_t protocol;
};

enum dppd_tcp_state {
    DPPD_TCP_NONE,
    DPPD_TCP_SYN,
    DPPD_TCP_SYN_ACK,
    DPPD_TCP_OPEN,
    DPPD_TCP_FIN,
    DPPD_TCP_CLOSED,
};

struct dppd_connection_view {
    uint64_t id, last_seen_ms, expires_ms, packets[2], bytes[2];
    struct dppd_flow_tuple original, translated;
    uint16_t ingress, egress;
    uint8_t tcp_state;
    bool seen_reply, nat;
};

struct dppd_conntrack_status {
    bool enabled;
    uint32_t capacity, active, peak, timeout_s;
    uint64_t revision, created, expired, flushed, invalid, table_full, pool_full;
    struct dppd_snat_config snat;
};

struct dppd_connection_page {
    uint64_t revision, next_id;
    uint32_t count;
    bool more;
    struct dppd_connection_view entries[DPPD_CT_PAGE_SIZE];
};

/** begin 与 finish 必须成对，期间持有表锁，ACL/路由/报文分配失败都应以 accept=false 结束 */
struct dppd_connection_attempt {
    struct dppd_conntrack *owner;
    struct dppd_flow_tuple policy, rewrite;
    uint8_t state;
    bool reply, nat;
};

uint64_t dppd_monotonic_ms(void);
int dppd_conntrack_create(struct dppd_conntrack **result, uint32_t capacity, uint32_t timeout_s,
    const struct dppd_snat_config *snat);
/** 只在全部 worker 停止、管理入口关闭后销毁，不保存运行时会话 */
void dppd_conntrack_destroy(struct dppd_conntrack *table);
int dppd_conntrack_begin(struct dppd_conntrack *table, uint16_t ingress, const struct dppd_packet *packet,
    uint64_t now_ms, struct dppd_connection_attempt *attempt);
int dppd_conntrack_finish(struct dppd_connection_attempt *attempt, uint16_t egress, bool accept);
void dppd_conntrack_gc(struct dppd_conntrack *table, uint64_t now_ms, uint32_t budget);
void dppd_conntrack_status(struct dppd_conntrack *table, uint64_t now_ms, struct dppd_conntrack_status *status);
int dppd_conntrack_list(struct dppd_conntrack *table, uint64_t now_ms, uint64_t after_id, uint64_t expected_revision,
    struct dppd_connection_page *page);
int dppd_conntrack_flush(struct dppd_conntrack *table, uint64_t expected_revision);

#endif
