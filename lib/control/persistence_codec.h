#ifndef DPPD_PERSISTENCE_CODEC_H
#define DPPD_PERSISTENCE_CODEC_H

#include "dppd/rule.h"

/** M4 配置复用 v5 的规则编码，不直接把含有填充字节的 C 结构写进文件 */
#define DPPD_PERSISTENCE_RULE_BYTES (40U + DPPD_RULE_MAX_ITEMS * 68U + DPPD_RULE_MAX_ACTIONS * 24U)
int dppd_persistence_rule_encode(const struct dppd_rule *rule, uint8_t *output);
int dppd_persistence_rule_decode(const uint8_t *input, struct dppd_rule *rule);

#endif
