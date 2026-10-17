#ifndef DPPD_ACL_H
#define DPPD_ACL_H

#include <stddef.h>
#include "dppd/packet.h"
#include "dppd/rule.h"

struct dppd_acl;

/** ACL 与所属规则快照一起分配，编译过程不额外申请内存，也不依赖 EAL */
size_t dppd_acl_size(uint32_t capacity);
struct dppd_acl *dppd_acl_init(void *memory, uint32_t capacity);
/** 只向尚未发布的表添加规则，tag 非零并用于定位所属快照中的动作和计数器 */
int dppd_acl_add(struct dppd_acl *acl, const struct dppd_rule *rule, uint32_t tag);
/** 读者只查询完整不可变表，先定位入口端口，再按 group、priority、ID、tag 选择首条匹配 */
uint32_t dppd_acl_classify(const struct dppd_acl *acl, uint16_t ingress, const struct dppd_packet *packet);

#endif
