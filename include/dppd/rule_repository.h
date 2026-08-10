#ifndef DPPD_RULE_REPOSITORY_H
#define DPPD_RULE_REPOSITORY_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/rule.h"

#define DPPD_RULE_GENERATION_ANY UINT64_MAX

enum dppd_rule_apply_status {
    DPPD_RULE_CREATED = 0,
    DPPD_RULE_UPDATED,
    DPPD_RULE_UNCHANGED,
};

struct dppd_rule_apply_result {
    enum dppd_rule_apply_status status;
    uint64_t generation;
};

struct dppd_rule_record;

struct dppd_rule_repository {
    struct dppd_rule_record *records;
    uint32_t capacity;
    uint32_t count;
    uint64_t generation;
};

int dppd_rule_repository_init(struct dppd_rule_repository *repository,
                              uint32_t capacity);
void dppd_rule_repository_destroy(struct dppd_rule_repository *repository);
uint32_t dppd_rule_repository_count(const struct dppd_rule_repository *repository);
uint64_t dppd_rule_repository_generation(const struct dppd_rule_repository *repository);
int dppd_rule_repository_get(const struct dppd_rule_repository *repository,
                             uint64_t rule_id,
                             struct dppd_rule *rule);
/*
 * 按 rule id 升序返回 after_rule_id 之后的一页。
 *
 * expected_repository_generation 检查的是 repository 全局修订号，而不是某条
 * 规则的 generation。客户端第一页传 ANY，后续页回传第一页得到的 generation；
 * 分页期间发生增删改时返回 -ESTALE，避免拼接出跨版本混合快照。
 */
int dppd_rule_repository_list(
    const struct dppd_rule_repository *repository,
    uint64_t after_rule_id,
    uint64_t expected_repository_generation,
    struct dppd_rule *rules,
    uint32_t limit,
    uint32_t *count,
    bool *has_more,
    uint64_t *repository_generation);
int dppd_rule_repository_apply(struct dppd_rule_repository *repository,
                               const struct dppd_rule *rule,
                               uint64_t expected_generation,
                               struct dppd_rule_apply_result *result);
int dppd_rule_repository_remove(struct dppd_rule_repository *repository,
                                uint64_t rule_id,
                                uint64_t expected_generation,
                                bool *removed,
                                uint64_t *generation);
/*
 * 把已经通过持久化格式校验的完整快照一次性发布到空 repository。
 * 与普通 apply 不同，本接口保留每条规则和全局 repository generation；它只供
 * 启动恢复使用，不得用于绕过在线写入的乐观并发与 generation 分配。
 */
int dppd_rule_repository_restore(struct dppd_rule_repository *repository,
                                 const struct dppd_rule *rules,
                                 uint32_t count,
                                 uint64_t repository_generation);

#endif
