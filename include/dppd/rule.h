#ifndef DPPD_RULE_H
#define DPPD_RULE_H

#include <stdbool.h>
#include <stdint.h>

#define DPPD_RULE_MAX_ITEMS 8U
#define DPPD_RULE_MAX_ACTIONS 8U

enum dppd_rule_domain {
    DPPD_RULE_DOMAIN_INGRESS = 0,
    DPPD_RULE_DOMAIN_EGRESS,
    DPPD_RULE_DOMAIN_TRANSFER,
};

enum dppd_fallback_policy {
    DPPD_FALLBACK_REQUIRE_HARDWARE = 0,
    DPPD_FALLBACK_PREFER_HARDWARE,
    DPPD_FALLBACK_SOFTWARE_ONLY,
};

enum dppd_match_type {
    DPPD_MATCH_ETH = 0,
    DPPD_MATCH_IPV4,
    DPPD_MATCH_UDP,
    DPPD_MATCH_TCP,
    DPPD_MATCH_REPRESENTED_PORT,
    DPPD_MATCH_UDP_RANGE,
    DPPD_MATCH_TCP_RANGE,
    DPPD_MATCH_IP_PROTOCOL,
    DPPD_MATCH_CONNECTION_STATE,
    DPPD_MATCH_IPV6,
};

struct dppd_match_ipv4 {
    uint32_t src_be;
    uint32_t src_mask_be;
    uint32_t dst_be;
    uint32_t dst_mask_be;
};

struct dppd_match_l4 {
    uint16_t src_be;
    uint16_t src_mask_be;
    uint16_t dst_be;
    uint16_t dst_mask_be;
};

/** 按字节保存地址和掩码，既支持 CIDR，也保留规则 IR 原有的任意掩码比较语义 */
struct dppd_match_ipv6 {
    uint8_t src[16], src_mask[16], dst[16], dst_mask[16];
};

/** 范围包含两个端点，四个端口数值都保存为网络字节序，不能把上界当作掩码 */
struct dppd_match_l4_range {
    uint16_t src_first_be, src_last_be, dst_first_be, dst_last_be;
};

struct dppd_match {
    enum dppd_match_type type;
    union {
        struct dppd_match_ipv4 ipv4;
        struct dppd_match_ipv6 ipv6;
        struct dppd_match_l4 l4;
        struct dppd_match_l4_range range;
        uint32_t connection_states;
        struct {
            uint8_t value, mask;
        } protocol;
        uint16_t ethdev_port_id;
    } spec;
};

enum dppd_action_type {
    DPPD_ACTION_DROP = 0,
    DPPD_ACTION_QUEUE,
    DPPD_ACTION_MARK,
    DPPD_ACTION_COUNT,
    DPPD_ACTION_REPRESENTED_PORT,
    /** 结束本次软件 ACL 匹配并继续后续转发，不承诺绕过路由或允许无效报文 */
    DPPD_ACTION_ALLOW,
    /** 超过指定秒数没有命中时报告老化，是否删除由控制面按精确版本决定 */
    DPPD_ACTION_AGE,
};

struct dppd_action {
    enum dppd_action_type type;
    union {
        uint16_t queue_id;
        uint32_t mark_id;
        uint16_t ethdev_port_id;
        uint32_t age_timeout_seconds;
    } conf;
};

struct dppd_rule {
    uint64_t id;
    uint64_t generation;
    /*
     * 规则安装目标属于 desired state，而不是临时的 API 参数。启动重放必须使用
     * 原始 ethdev port，不能在多端口环境中猜测或默认成 port 0
     */
    uint16_t install_port_id;
    enum dppd_rule_domain domain;
    enum dppd_fallback_policy fallback;
    uint32_t group;
    uint32_t priority;
    uint16_t nb_matches;
    uint16_t nb_actions;
    struct dppd_match matches[DPPD_RULE_MAX_ITEMS];
    struct dppd_action actions[DPPD_RULE_MAX_ACTIONS];
};

int dppd_rule_validate(const struct dppd_rule *rule, char *error, uint32_t error_len);
bool dppd_rule_equal(const struct dppd_rule *left, const struct dppd_rule *right);

#endif
