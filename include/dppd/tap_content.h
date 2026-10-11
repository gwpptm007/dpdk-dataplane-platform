#ifndef DPPD_TAP_CONTENT_H
#define DPPD_TAP_CONTENT_H

#include "dppd/rule.h"

#define DPPD_TAP_CONTENT_SIZE 64U

/**
 * 保存本地 TAP 规则真正参与匹配的位以及单个终止动作
 * 地址与端口使用网络字节序，其余整数使用主机字节序
 * version 为零表示旧记录没有预期内容，不能从当前内核对象反推原始意图
 */
struct dppd_tap_content {
    uint32_t version;
    uint16_t eth_type, queue;
    uint8_t ip_proto, action;
    struct dppd_match_ipv4 ipv4;
    struct dppd_match_l4 l4;
    uint32_t parent, chain;
    uint16_t priority, protocol;
};

/** 根据 DPDK 21.11 TAP 的映射计算预期值，超出可完整核验的范围立即拒绝 */
int dppd_tap_content_from_rule(const struct dppd_rule *rule, uint16_t max_queues, struct dppd_tap_content *content);
/** 固定编码不包含 C 结构体填充，预留字节统一为零 */
void dppd_tap_content_encode(const struct dppd_tap_content *content, uint8_t bytes[DPPD_TAP_CONTENT_SIZE]);
/** 同时检查字段组合和规范编码，未知版本或保留位不允许静默通过 */
int dppd_tap_content_decode(const uint8_t bytes[DPPD_TAP_CONTENT_SIZE], struct dppd_tap_content *content);
/** 检查预期内容是否完整，零版本只允许所有字段均为零 */
bool dppd_tap_content_valid(const struct dppd_tap_content *content);

#endif
