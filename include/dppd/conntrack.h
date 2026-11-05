#ifndef DPPD_CONNTRACK_H
#define DPPD_CONNTRACK_H

#include "dppd/config.h"
#include "dppd/packet.h"

#define DPPD_CT_PAGE_SIZE 4U

struct dppd_conntrack;

/** 容量为零表示关闭跟踪，此时其余字段全部清零，避免隐藏尚未生效的 NAT 策略 */
struct dppd_conntrack_policy {
    uint32_t capacity, timeout_s;
    struct dppd_snat_config snat, snat6;
    uint32_t dnat_count;
    struct dppd_dnat_rule dnat[DPPD_MAX_DNAT_RULES];
};

/**
 * 描述一条流的五元组：源 IP、目的 IP、源端口、目的端口、传输层协议
 * 正向示例：10.0.0.2:53000 → 192.0.2.10:9000，UDP
 * 反向示例：192.0.2.10:9000 → 10.0.0.2:53000，UDP（未启用 NAT 时）
 * 会话查找还会检查地址族和入口网口，正反向索引可指向同一条会话记录
 */
struct dppd_flow_tuple {
    /** IPv4 源地址，网络字节序 */
    uint32_t source_be;
    /** IPv4 目的地址，网络字节序 */
    uint32_t destination_be;
    /** 源端口，网络字节序 */
    uint16_t source_port_be;
    /** 目的端口，网络字节序 */
    uint16_t destination_port_be;
    /** IP 协议编号，TCP 为 6，UDP 为 17 */
    uint8_t protocol;
    /** true 使用下面的 IPv6 地址，false 使用上面的 IPv4 地址，地址族也参与匹配 */
    bool ipv6;
    /** IPv6 源地址的 16 个网络字节 */
    uint8_t source6[16];
    /** IPv6 目的地址的 16 个网络字节 */
    uint8_t destination6[16];
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
    uint32_t dnat_rule_id;
    uint8_t tcp_state;
    bool seen_reply, nat;
};

struct dppd_conntrack_status {
    bool enabled;
    uint32_t capacity, active, peak, timeout_s;
    /** epoch 标识本次会话容器，重启后改变，防止旧请求误清理新进程的会话 */
    uint64_t epoch, revision, created, expired, flushed, invalid, table_full, pool_full;
    struct dppd_snat_config snat;
    struct dppd_snat_config snat6;
    uint32_t dnat_count;
};

/** 静态规则编号按当前策略顺序分配，跨页读取还应核对控制面的配置修订号 */
struct dppd_dnat_page {
    uint32_t count, next_id;
    bool more;
    struct {
        uint32_t id;
        struct dppd_dnat_rule rule;
    } entries[DPPD_CT_PAGE_SIZE];
};

struct dppd_connection_page {
    uint64_t epoch, revision, next_id;
    uint32_t count;
    bool more;
    struct dppd_connection_view entries[DPPD_CT_PAGE_SIZE];
};

/** begin 与 finish 必须成对，期间持有表锁，ACL/路由/报文分配失败都应以 accept=false 结束 */
struct dppd_connection_attempt {
    struct dppd_conntrack *owner;
    struct dppd_flow_tuple policy, rewrite;
    uint8_t state;
    /** tracking 记录此包是否进入启用的跟踪路径，关闭容器时保持普通转发行为 */
    bool reply, nat, tracking;
};

uint64_t dppd_monotonic_ms(void);
/** 只检查字段和冲突，不分配会话数组，供文件恢复和在线请求共用 */
int dppd_conntrack_policy_validate(const struct dppd_conntrack_policy *policy);
int dppd_conntrack_create_policy(struct dppd_conntrack **result, const struct dppd_conntrack_policy *policy);
/**
 * 预分配候选后再锁表，精确核对 epoch 与会话修订号，有活跃会话时必须明确允许清空
 * publish 成功后切换内部存储，表地址和 epoch 不变，失败时保留原策略和全部会话
 * publish 在表锁内执行，不能再次调用任何会话表接口
 */
int dppd_conntrack_replace(struct dppd_conntrack *table, const struct dppd_conntrack_policy *policy,
    bool reset_sessions, uint64_t expected_epoch, uint64_t expected_revision, int (*publish)(void *), void *context);
int dppd_conntrack_create(struct dppd_conntrack **result, uint32_t capacity, uint32_t timeout_s,
    const struct dppd_snat_config *snat, const struct dppd_dnat_rule *dnat, uint32_t dnat_count);
/** 双栈共用容量与会话修订号，转换策略和端口分配按地址族独立 */
int dppd_conntrack_create_dual(struct dppd_conntrack **result, uint32_t capacity, uint32_t timeout_s,
    const struct dppd_snat_config *snat, const struct dppd_snat_config *snat6, const struct dppd_dnat_rule *dnat,
    uint32_t dnat_count);
/** 只在全部 worker 停止、管理入口关闭后销毁，不保存运行时会话 */
void dppd_conntrack_destroy(struct dppd_conntrack *table);
int dppd_conntrack_begin(struct dppd_conntrack *table, uint16_t ingress, const struct dppd_packet *packet,
    uint64_t now_ms, struct dppd_connection_attempt *attempt);
int dppd_conntrack_finish(struct dppd_connection_attempt *attempt, uint16_t egress, bool accept);
void dppd_conntrack_gc(struct dppd_conntrack *table, uint64_t now_ms, uint32_t budget);
void dppd_conntrack_status(struct dppd_conntrack *table, uint64_t now_ms, struct dppd_conntrack_status *status);
int dppd_conntrack_list(struct dppd_conntrack *table, uint64_t now_ms, uint64_t after_id, uint64_t expected_revision,
    struct dppd_connection_page *page);
/** 首次读取同时传零，后续分页同时传回 epoch 与 revision，重启或集合变化都拒绝旧游标 */
int dppd_conntrack_list_guarded(struct dppd_conntrack *table, uint64_t now_ms, uint64_t after_id,
    uint64_t expected_epoch, uint64_t expected_revision, struct dppd_connection_page *page);
int dppd_conntrack_flush(struct dppd_conntrack *table, uint64_t expected_revision);
int dppd_conntrack_flush_guarded(struct dppd_conntrack *table, uint64_t expected_epoch, uint64_t expected_revision);
int dppd_conntrack_dnat_list(struct dppd_conntrack *table, uint64_t after_id, struct dppd_dnat_page *page);

#endif
