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

/**
 * IPv4 地址匹配，_be 表示网络字节序，即高位字节在前
 * 掩码中 1 对应的位参与比较，0 对应的位忽略；例如 /24 掩码为 255.255.255.0
 */
struct dppd_match_ipv4 {
    /** 要匹配的源 IPv4 地址 */
    uint32_t src_be;
    /** 源地址掩码，0 表示不限源地址 */
    uint32_t src_mask_be;
    /** 要匹配的目的 IPv4 地址 */
    uint32_t dst_be;
    /** 目的地址掩码，全 1 表示精确匹配该地址 */
    uint32_t dst_mask_be;
};

/** UDP/TCP 端口匹配，端口和掩码均为网络字节序；掩码为 0 表示不限，为 0xffff 表示精确匹配 */
struct dppd_match_l4 {
    /** 源端口值 */
    uint16_t src_be;
    /** 源端口中哪些位参与匹配 */
    uint16_t src_mask_be;
    /** 目的端口值，例如 htons(9000) */
    uint16_t dst_be;
    /** 目的端口中哪些位参与匹配 */
    uint16_t dst_mask_be;
};

/** IPv6 地址和掩码按网络顺序保存为 16 字节数组；例如 /64 掩码为前 8 字节全 ff、后 8 字节全 00 */
struct dppd_match_ipv6 {
    /** 要匹配的源 IPv6 地址 */
    uint8_t src[16];
    /** 源地址掩码，全零表示不限；也支持非连续的掩码 */
    uint8_t src_mask[16];
    /** 要匹配的目的 IPv6 地址 */
    uint8_t dst[16];
    /** 目的地址掩码，1 对应的位参与比较 */
    uint8_t dst_mask[16];
};

/** UDP/TCP 端口范围，含上下界且均为网络字节序；[9000, 9000] 匹配单个端口，[0, 65535] 表示不限 */
struct dppd_match_l4_range {
    /** 源端口下界 */
    uint16_t src_first_be;
    /** 源端口上界 */
    uint16_t src_last_be;
    /** 目的端口下界 */
    uint16_t dst_first_be;
    /** 目的端口上界 */
    uint16_t dst_last_be;
};

/** 一项匹配条件，type 决定使用 spec 中哪个成员；ETH 只要求以太网层，不使用 spec */
struct dppd_match {
    /** 匹配类型，例如 IPV4、UDP_RANGE 或 CONNECTION_STATE */
    enum dppd_match_type type;
    /** 各成员共用存储空间，一项条件只使用其中一种 */
    union {
        /** IPV4：源、目的地址及掩码 */
        struct dppd_match_ipv4 ipv4;
        /** IPV6：源、目的地址及掩码 */
        struct dppd_match_ipv6 ipv6;
        /** UDP/TCP：端口值及位掩码 */
        struct dppd_match_l4 l4;
        /** UDP_RANGE/TCP_RANGE：端口上下界 */
        struct dppd_match_l4_range range;
        /** 连接状态位组合：UNTRACKED=1、NEW=2、ESTABLISHED=4、INVALID=8，可按位或组合 */
        uint32_t connection_states;
        /** IP_PROTOCOL：按 (报文协议号 & mask) == (value & mask) 匹配 */
        struct {
            /** IP 协议号，例如 TCP 为 6、UDP 为 17 */
            uint8_t value;
            /** 协议号掩码，0xff 表示精确匹配，0 表示不限 */
            uint8_t mask;
        } protocol;
        /** REPRESENTED_PORT：匹配的 DPDK 端口 ID，用于 transfer 域 */
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
    /** 引用共享计数对象的精确身份，禁止把进程内驱动指针放进规则或快照 */
    DPPD_ACTION_SHARED_COUNT,
};

/** 命中规则后执行的一项动作，type 决定使用 conf 中哪个参数 */
struct dppd_action {
    /** 动作类型，例如 COUNT、MARK 或 DROP */
    enum dppd_action_type type;
    /** 动作参数共用存储空间；DROP、COUNT、ALLOW 无需填写参数 */
    union {
        /** QUEUE：将报文送入的网卡接收队列编号 */
        uint16_t queue_id;
        /** MARK：写入报文元数据的标记值，供后续处理识别 */
        uint32_t mark_id;
        /** REPRESENTED_PORT：转发目标的 DPDK 端口 ID，用于 transfer 域 */
        uint16_t ethdev_port_id;
        /** AGE：连续多少秒未命中后报告老化，不会在此处自动删除规则 */
        uint32_t age_timeout_seconds;
        /** SHARED_COUNT：引用已经创建的共享计数对象 */
        struct {
            /** 共享计数对象的 ID */
            uint64_t id;
            /** 该计数对象的版本，防止误用同 ID 重建的新对象 */
            uint64_t generation;
        } shared_count;
    } conf;
};

/** 一条完整规则：身份和版本 + 安装位置 + 匹配条件 + 命中后的动作 */
struct dppd_rule {
    /** 规则的唯一 ID，用于查找、更新和删除，例如 20 */
    uint64_t id;
    /** 本条规则当前版本，由控制面分配；不同于调用方传入的预期旧版本 */
    uint64_t generation;
    /** 安装到哪个 DPDK 端口，保存配置时一并记录，重启恢复时沿用 */
    uint16_t install_port_id;
    /** 生效位置：INGRESS 入口、EGRESS 出口或 TRANSFER 端口间转发 */
    enum dppd_rule_domain domain;
    /** 执行要求：必须硬件、优先硬件或仅软件 */
    enum dppd_fallback_policy fallback;
    /** 规则组编号；软件匹配先比较 group，数值小的优先 */
    uint32_t group;
    /** 同组内的优先级，数值越小越优先 */
    uint32_t priority;
    /** matches 中有效条件的数量，例如 ETH、IPv4、UDP_RANGE 共 3 项 */
    uint16_t nb_matches;
    /** actions 中有效动作的数量，例如 COUNT、DROP 共 2 项 */
    uint16_t nb_actions;
    /** 匹配条件数组，各项需同时满足，只读取前 nb_matches 项 */
    struct dppd_match matches[DPPD_RULE_MAX_ITEMS];
    /** 动作数组，只读取前 nb_actions 项；决定报文去向的动作放在最后 */
    struct dppd_action actions[DPPD_RULE_MAX_ACTIONS];
};

int dppd_rule_validate(const struct dppd_rule *rule, char *error, uint32_t error_len);
bool dppd_rule_equal(const struct dppd_rule *left, const struct dppd_rule *right);

#endif
