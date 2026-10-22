#ifndef DPPD_RULE_REPOSITORY_H
#define DPPD_RULE_REPOSITORY_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/rule.h"
#include "dppd/slot_index.h"

/** 不限制预期版本，调用方明确跳过版本冲突检查；该值不分配给实际规则版本 */
#define DPPD_RULE_GENERATION_ANY UINT64_MAX

enum dppd_rule_apply_status {
    DPPD_RULE_CREATED = 0,
    DPPD_RULE_UPDATED,
    DPPD_RULE_UNCHANGED,
};

struct dppd_rule_apply_result {
    /** 本次操作新增、更新或内容未变，函数返回 0 后读取 */
    enum dppd_rule_apply_status status;
    /** 操作完成后该条规则的版本，内容未变时返回原版本 */
    uint64_t generation;
};

struct dppd_rule_record;

struct dppd_rule_repository {
    /** 保存完整规则的槽位数组，容量为 capacity，空槽位可复用 */
    struct dppd_rule_record *records;
    /** 只移动紧凑槽位编号，查找和分页不再反复扫描完整规则数组 */
    struct dppd_slot_index index;
    /** 可容纳的最大规则数 */
    uint32_t capacity;
    /** 当前有效规则数，新增时增加，更新已有规则时不变 */
    uint32_t count;
    /** 仓库全局修订号，修改后推进；各规则保留各自最后一次创建或更新时分配的版本 */
    uint64_t generation;
};

int dppd_rule_repository_init(struct dppd_rule_repository *repository, uint32_t capacity);
void dppd_rule_repository_destroy(struct dppd_rule_repository *repository);
uint32_t dppd_rule_repository_count(const struct dppd_rule_repository *repository);
uint64_t dppd_rule_repository_generation(const struct dppd_rule_repository *repository);
int dppd_rule_repository_get(const struct dppd_rule_repository *repository, uint64_t rule_id, struct dppd_rule *rule);
/*
 * 按 rule id 升序返回 after_rule_id 之后的一页
 *
 * expected_repository_generation 检查的是 repository 全局修订号，而不是某条
 * 规则的 generation。客户端第一页传 ANY，后续页回传第一页得到的 generation；
 * 分页期间发生增删改时返回 -ESTALE，避免拼接出跨版本混合快照
 */
int dppd_rule_repository_list(const struct dppd_rule_repository *repository, uint64_t after_rule_id,
    uint64_t expected_repository_generation, struct dppd_rule *rules, uint32_t limit, uint32_t *count, bool *has_more,
    uint64_t *repository_generation);
int dppd_rule_repository_apply(struct dppd_rule_repository *repository, const struct dppd_rule *rule,
    uint64_t expected_generation, struct dppd_rule_apply_result *result);
/** M4 预先分配连续版本，全部校验并建立槽位索引后才发布整批，调用者须串行访问 */
int dppd_rule_repository_create_batch(struct dppd_rule_repository *repository, const struct dppd_rule *rules,
    uint32_t count);
/** 启动恢复逐批装入已校验的旧版本，修订号由外层先固定，运行期不得用它绕过版本分配 */
int dppd_rule_repository_replay_batch(struct dppd_rule_repository *repository, const struct dppd_rule *rules,
    uint32_t count);
/** 输入为精确旧规则身份，全部预检后一次推进修订号，不调用后端也不分配内存 */
int dppd_rule_repository_remove_batch(struct dppd_rule_repository *repository, const struct dppd_rule *rules,
    uint32_t count);
/** 只供停机回收实际对象，不推进配置修订号，版本空间耗尽也必须能够退出 */
int dppd_rule_repository_forget_batch(struct dppd_rule_repository *repository, const struct dppd_rule *rules,
    uint32_t count);
/**
 * 由单个控制线程整批更新规则账本，rules 与 expected_generations 按下标一一对应
 * 先确认全批规则合法且旧版本匹配，再执行不分配内存的替换，因此输入错误不产生部分写入
 * 相同内容也分配连续新版本；这个接口只修改账本，调用方必须先完成实际后端的切换
 */
int dppd_rule_repository_update_batch(struct dppd_rule_repository *repository, const struct dppd_rule *rules,
    const uint64_t *expected_generations, uint32_t count);
int dppd_rule_repository_remove(struct dppd_rule_repository *repository, uint64_t rule_id, uint64_t expected_generation,
    bool *removed, uint64_t *generation);
/*
 * 把已经通过持久化格式校验的完整快照一次性发布到空 repository
 * 与普通 apply 不同，本接口保留每条规则和全局 repository generation；它只供
 * 启动恢复使用，不得用于绕过在线写入的乐观并发与 generation 分配
 */
int dppd_rule_repository_restore(struct dppd_rule_repository *repository, const struct dppd_rule *rules, uint32_t count,
    uint64_t repository_generation);

#endif
