#ifndef DPPD_EXACT_H
#define DPPD_EXACT_H

#include <stddef.h>
#include "dppd/packet.h"
#include "dppd/rule.h"

struct dppd_exact;

/** 精确匹配表与规则快照共同分配，发布后只读，查询不分配内存 */
size_t dppd_exact_size(uint32_t capacity);
struct dppd_exact *dppd_exact_init(void *memory, uint32_t capacity);
/** 只接收完整地址和端口的 TCP/UDP 五元组，其余合法条件返回 ENOTSUP 交给 ACL */
int dppd_exact_add(struct dppd_exact *table, const struct dppd_rule *rule, uint32_t tag);
/** 相同五元组按 group、priority、ID、tag 选出唯一结果，入口和地址族也属于查找键 */
uint32_t dppd_exact_classify(const struct dppd_exact *table, uint16_t ingress, const struct dppd_packet *packet);

#endif
