#ifndef DPPD_CAPABILITY_H
#define DPPD_CAPABILITY_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/offload.h"

/** 缓存固定占用小块控制面内存，不随用户提交的规则数量无限增长 */
#define DPPD_FLOW_PROBE_CACHE_CAPACITY 64U
/** 探测结果最多保留五秒，不能把某次驱动答复当成永久能力保证 */
#define DPPD_FLOW_PROBE_CACHE_TTL_NS UINT64_C(5000000000)
#define DPPD_CAPABILITY_NAME_SIZE 128U
#define DPPD_CAPABILITY_VERSION_SIZE 64U

/**
 * 启动配置时保存的设备身份和描述符信息，全部是值，不向管理客户端暴露驱动指针
 * 固件未知与版本为空不同，查询失败或缓冲区不够时明确保留错误，不妨碍原有端口配置
 */
struct dppd_capability_identity {
    /** 设备实例、固件及 DPDK 版本共同说明本次观察的环境，不能只按厂商判断能力 */
    char device_name[DPPD_CAPABILITY_NAME_SIZE];
    char firmware_version[DPPD_CAPABILITY_NAME_SIZE];
    char dpdk_version[DPPD_CAPABILITY_VERSION_SIZE];
    /** 查询失败时字符串不作为有效结果使用，固件错误保留供用户辨别未知原因 */
    bool device_name_known;
    bool firmware_known;
    int32_t firmware_error;
    /** min/max 是驱动允许的范围，align 是描述符数量需要满足的对齐要求 */
    uint16_t rx_desc_min;
    uint16_t rx_desc_max;
    uint16_t rx_desc_align;
    uint16_t tx_desc_min;
    uint16_t tx_desc_max;
    uint16_t tx_desc_align;
    /** configured 数值记录实际调整后的配置，与驱动声明的最大能力分别保存 */
    uint16_t configured_rx_desc;
    uint16_t configured_tx_desc;
    uint16_t configured_queues;
};

/** 将不支持、规则被拒绝和设备暂时无法校验分开，不能都解释成“不支持硬件” */
enum dppd_flow_probe_status {
    DPPD_FLOW_PROBE_SUPPORTED = 0,
    DPPD_FLOW_PROBE_UNSUPPORTED,
    DPPD_FLOW_PROBE_REJECTED,
    DPPD_FLOW_PROBE_UNAVAILABLE,
};

/**
 * 一条完整规则的驱动校验答复，不表示已经创建对象，也不预留驱动资源
 * cached 为真时 age_ns 表示答复已经过去多久，正式安装仍必须重新调用驱动校验
 */
struct dppd_flow_probe_result {
    uint64_t rule_id;
    uint16_t install_port_id;
    enum dppd_flow_probe_status status;
    int32_t validation_code;
    bool cached;
    bool age_available;
    bool software_equivalent;
    uint64_t age_ns;
    uint64_t epoch;
    char message[256];
};

/**
 * 每个端口自进程启动以来的校验记录，查询它只读取内存，不会重新访问驱动
 * observed 掩码只说明成功校验的组合里出现过这些元素，不能推断任意组合都支持
 * cache_entries 表示仍占用的结果槽位，过期项会在下次探测时清理，不是网卡规则容量
 */
struct dppd_flow_probe_statistics {
    /** epoch 是该端口答复的失效版本，不是规则账本的 generation */
    uint64_t epoch;
    /** validations 包含实际探测和正式安装校验，后三项按实际驱动返回码分类累计 */
    uint64_t validations;
    uint64_t supported;
    uint64_t unsupported;
    uint64_t failed;
    /** hits/misses 只统计诊断查询，清缓存不把历史统计清零 */
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t invalidations;
    /** 六十四个槽位由全部端口共享，每端口的 entries 仅计数属于它的记录 */
    uint32_t cache_entries;
    uint32_t cache_capacity;
    /** 位编号对应规则 IR 的枚举值，只记录完整规则曾校验成功时出现的元素 */
    uint32_t observed_domains;
    uint32_t observed_items;
    uint32_t observed_actions;
};

struct dppd_flow_probe_cache;

/** 缓存只由串行管理线程使用，不给逐包路径增加锁、分配或时钟读取 */
int dppd_flow_probe_cache_init(struct dppd_flow_probe_cache **cache);
void dppd_flow_probe_cache_destroy(struct dppd_flow_probe_cache *cache);
int dppd_flow_probe_cache_register(struct dppd_flow_probe_cache *cache, uint16_t port_id);
/** 显式刷新端口的结果，累计校验统计保留，epoch 推进以标识旧答复已经失效 */
int dppd_flow_probe_cache_clear(struct dppd_flow_probe_cache *cache, uint16_t port_id);
/** 创建或删除可能改变共享资源，任何一次尝试都保守清空全部端口的结果 */
void dppd_flow_probe_cache_invalidate_all(struct dppd_flow_probe_cache *cache);
/** 正式校验只累计事实，不缓存答复，也不读取时钟 */
void dppd_flow_probe_cache_observe(struct dppd_flow_probe_cache *cache, uint16_t port_id,
                                  const struct dppd_rule *rule, int validation_code);
int dppd_flow_probe_cache_statistics(const struct dppd_flow_probe_cache *cache,
                                     uint16_t port_id,
                                     struct dppd_flow_probe_statistics *statistics);
/** refresh 为真时绕过旧结果，只做驱动校验，不创建或删除规则 */
int dppd_flow_probe_cache_query(struct dppd_flow_probe_cache *cache, uint16_t port_id,
    const struct dppd_rule *rule, bool refresh,
    int (*validate)(uint16_t, const struct dppd_rule *, struct dppd_flow_error *),
    struct dppd_flow_probe_result *result);

#endif
