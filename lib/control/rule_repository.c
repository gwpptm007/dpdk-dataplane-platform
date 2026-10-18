#include "dppd/rule_repository.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/** 删除后槽位可以复用，是否有效由 occupied 决定，不能只看内存里是否仍有旧规则内容 */
struct dppd_rule_record {
    bool occupied;
    struct dppd_rule rule;
};

static struct dppd_rule_record *find_record(struct dppd_rule_repository *repository, uint64_t rule_id)
{
    uint32_t slot = dppd_slot_index_find(&repository->index, rule_id, 0);
    return slot == DPPD_SLOT_NONE ? NULL : &repository->records[slot];
}

static const struct dppd_rule_record *find_record_const(const struct dppd_rule_repository *repository, uint64_t rule_id)
{
    uint32_t slot = dppd_slot_index_find(&repository->index, rule_id, 0);
    return slot == DPPD_SLOT_NONE ? NULL : &repository->records[slot];
}

int dppd_rule_repository_init(struct dppd_rule_repository *repository, uint32_t capacity)
{
    if (repository == NULL || capacity == 0)
        return -EINVAL;
    memset(repository, 0, sizeof(*repository));
    repository->records = calloc(capacity, sizeof(*repository->records));
    if (repository->records == NULL)
        return -ENOMEM;
    int rc = dppd_slot_index_init(&repository->index, capacity);
    if (rc != 0) {
        free(repository->records);
        memset(repository, 0, sizeof(*repository));
        return rc;
    }
    repository->capacity = capacity;
    return 0;
}

void dppd_rule_repository_destroy(struct dppd_rule_repository *repository)
{
    if (repository == NULL)
        return;
    free(repository->records);
    dppd_slot_index_fini(&repository->index);
    memset(repository, 0, sizeof(*repository));
}

uint32_t dppd_rule_repository_count(const struct dppd_rule_repository *repository)
{
    return repository == NULL ? 0 : repository->count;
}

uint64_t dppd_rule_repository_generation(const struct dppd_rule_repository *repository)
{
    return repository == NULL ? 0 : repository->generation;
}

int dppd_rule_repository_get(const struct dppd_rule_repository *repository, uint64_t rule_id, struct dppd_rule *rule)
{
    const struct dppd_rule_record *record;

    if (repository == NULL || repository->records == NULL || rule_id == 0 || rule == NULL)
        return -EINVAL;
    record = find_record_const(repository, rule_id);
    if (record == NULL)
        return -ENOENT;
    /** 返回独立副本，调用方修改查询结果不会直接改动已经发布的规则账本 */
    *rule = record->rule;
    return 0;
}

int dppd_rule_repository_list(const struct dppd_rule_repository *repository, uint64_t after_rule_id,
    uint64_t expected_repository_generation, struct dppd_rule *rules, uint32_t limit, uint32_t *count, bool *has_more,
    uint64_t *repository_generation)
{
    if (repository == NULL || repository->records == NULL || rules == NULL || limit == 0 || count == NULL ||
        has_more == NULL || repository_generation == NULL)
        return -EINVAL;
    if (expected_repository_generation != DPPD_RULE_GENERATION_ANY &&
        expected_repository_generation != repository->generation)
        return -ESTALE;

    /** 对外仍按规则编号分页，内部只二分一次游标，再顺序复制本页对象 */
    uint32_t position = dppd_slot_index_upper_bound(&repository->index, after_rule_id, UINT64_MAX);
    uint32_t available = repository->index.count - position;
    uint32_t output_count = limit < available ? limit : available;
    for (uint32_t i = 0; i < output_count; ++i) {
        uint32_t slot = dppd_slot_index_at(&repository->index, position + i);
        rules[i] = repository->records[slot].rule;
    }
    *has_more = output_count < available;
    *count = output_count;
    *repository_generation = repository->generation;
    return 0;
}

int dppd_rule_repository_apply(struct dppd_rule_repository *repository, const struct dppd_rule *rule,
    uint64_t expected_generation, struct dppd_rule_apply_result *result)
{
    struct dppd_rule_record *record;
    char validation_error[128];

    if (repository == NULL || repository->records == NULL || rule == NULL || rule->id == 0 || result == NULL)
        return -EINVAL;
    if (dppd_rule_validate(rule, validation_error, sizeof(validation_error)) != 0)
        return -EINVAL;

    record = find_record(repository, rule->id);
    if (record != NULL) {
        if (dppd_rule_equal(&record->rule, rule)) {
            result->status = DPPD_RULE_UNCHANGED;
            result->generation = record->rule.generation;
            return 0;
        }
        if (expected_generation != DPPD_RULE_GENERATION_ANY && expected_generation != record->rule.generation)
            return -ESTALE;
        if (repository->generation >= UINT64_MAX - 1U)
            return -EOVERFLOW;
        record->rule = *rule;
        /* generation 只由 repository 分配，调用者携带的值不会直接发布 */
        record->rule.generation = ++repository->generation;
        result->status = DPPD_RULE_UPDATED;
        result->generation = record->rule.generation;
        return 0;
    }

    if (expected_generation != DPPD_RULE_GENERATION_ANY && expected_generation != 0)
        return -ESTALE;
    if (repository->generation >= UINT64_MAX - 1U)
        return -EOVERFLOW;
    uint32_t slot;
    int rc = dppd_slot_index_insert(&repository->index, rule->id, 0, &slot);
    if (rc != 0)
        return rc;
    record = &repository->records[slot];
    record->occupied = true;
    record->rule = *rule;
    /* create 与 update 共用同一条全局 desired-state 修订序列 */
    record->rule.generation = ++repository->generation;
    repository->count++;
    result->status = DPPD_RULE_CREATED;
    result->generation = record->rule.generation;
    return 0;
}

/**
 * 整批创建尚不存在的规则账本记录，不执行任何硬件或软件安装操作
 *
 * 第一轮确认规则合法、ID 不重复且版本与预分配序列相同，再一次建立全部索引
 * 索引全部成功后才写入记录，所以后一条输入或容量错误不会留下已发布前缀
 * 该保证依赖调用方串行访问仓库，不等于提供了多线程并发写入保护
 */
int dppd_rule_repository_create_batch(struct dppd_rule_repository *repository, const struct dppd_rule *rules,
    uint32_t count)
{
    uint32_t slots[64], inserted = 0;
    char error[128];
    int rc;
    if (repository == NULL || repository->records == NULL || rules == NULL || count == 0 || count > 64)
        return -EINVAL;
    if (count > repository->capacity - repository->count)
        return -ENOSPC;
    if (repository->generation >= UINT64_MAX - count)
        return -EOVERFLOW;
    for (uint32_t i = 0; i < count; ++i) {
        if (rules[i].id == 0 || rules[i].id == UINT64_MAX || rules[i].generation != repository->generation + i + 1U ||
            dppd_rule_validate(&rules[i], error, sizeof(error)) != 0)
            return -EINVAL;
        if (find_record(repository, rules[i].id) != NULL)
            return -EEXIST;
        for (uint32_t j = 0; j < i; ++j)
            if (rules[j].id == rules[i].id)
                return -EEXIST;
    }
    for (; inserted < count; ++inserted) {
        rc = dppd_slot_index_insert(&repository->index, rules[inserted].id, 0, &slots[inserted]);
        if (rc != 0) {
            while (inserted != 0)
                dppd_slot_index_erase(&repository->index, rules[--inserted].id, 0);
            return -EUCLEAN;
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        repository->records[slots[i]].rule = rules[i];
        repository->records[slots[i]].occupied = true;
    }
    repository->count += count;
    repository->generation += count;
    return 0;
}

/** 精确版本、重复编号及版本空间先整批检查，正常删除阶段没有分配或外部调用 */
int dppd_rule_repository_remove_batch(struct dppd_rule_repository *repository, const struct dppd_rule *rules,
    uint32_t count)
{
    uint32_t slots[64];
    if (repository == NULL || repository->records == NULL || rules == NULL || count == 0 || count > 64)
        return -EINVAL;
    if (repository->generation >= UINT64_MAX - count)
        return -EOVERFLOW;
    for (uint32_t i = 0; i < count; ++i) {
        slots[i] = dppd_slot_index_find(&repository->index, rules[i].id, 0);
        if (slots[i] == DPPD_SLOT_NONE || rules[i].generation == 0 || rules[i].generation == UINT64_MAX ||
            repository->records[slots[i]].rule.generation != rules[i].generation)
            return -ESTALE;
        for (uint32_t j = 0; j < i; ++j)
            if (rules[j].id == rules[i].id)
                return -EEXIST;
    }
    for (uint32_t i = 0; i < count; ++i)
        if (dppd_slot_index_erase(&repository->index, rules[i].id, 0) != 0)
            return -EUCLEAN;
    for (uint32_t i = 0; i < count; ++i)
        memset(&repository->records[slots[i]], 0, sizeof(repository->records[slots[i]]));
    repository->count -= count;
    repository->generation += count;
    return 0;
}

/** 替换前完整校验，版本由同一控制线程连续分配，预检完成后不再分配内存 */
int dppd_rule_repository_update_batch(struct dppd_rule_repository *repository, const struct dppd_rule *rules,
    const uint64_t *expected_generations, uint32_t count)
{
    uint32_t i;

    if (repository == NULL || repository->records == NULL || rules == NULL || expected_generations == NULL ||
        count == 0)
        return -EINVAL;
    if (count > repository->count)
        return -ENOENT;
    /**
     * UINT64_MAX 被用来表示“不检查版本”的 ANY 条件，不能分配给真实规则
     * 这里一次检查整批需要的版本空间，避免写到中途才发现版本号用尽
     */
    if (repository->generation >= UINT64_MAX - count)
        return -EOVERFLOW;
    for (i = 0; i < count; ++i) {
        struct dppd_rule_record *record;
        char error[128];
        uint32_t j;

        if (rules[i].id == 0 || dppd_rule_validate(&rules[i], error, sizeof(error)) != 0)
            return -EINVAL;
        for (j = 0; j < i; ++j) {
            if (rules[j].id == rules[i].id)
                return -EEXIST;
        }
        record = find_record(repository, rules[i].id);
        if (record == NULL)
            return -ENOENT;
        if (expected_generations[i] == 0 || expected_generations[i] == DPPD_RULE_GENERATION_ANY ||
            expected_generations[i] != record->rule.generation)
            return -ESTALE;
    }
    /**
     * 全部检查通过后才开始写入，这一阶段不申请内存、不调用后端，也不删除记录
     * 单条 apply 遇到相同内容会返回 UNCHANGED，无法满足批量更新连续分配新版本的约定
     * 因此这里直接替换完整规则内容，并按请求顺序推进仓库的全局版本号
     */
    for (i = 0; i < count; ++i) {
        struct dppd_rule_record *record = find_record(repository, rules[i].id);

        /** 同一控制线程中没有增删记录，上面已确认存在的 ID 在这里一定能找到 */
        record->rule = rules[i];
        record->rule.generation = ++repository->generation;
    }
    return 0;
}

int dppd_rule_repository_remove(struct dppd_rule_repository *repository, uint64_t rule_id, uint64_t expected_generation,
    bool *removed, uint64_t *generation)
{
    struct dppd_rule_record *record;

    if (repository == NULL || repository->records == NULL || rule_id == 0 || removed == NULL || generation == NULL)
        return -EINVAL;
    record = find_record(repository, rule_id);
    if (record == NULL) {
        if (expected_generation != DPPD_RULE_GENERATION_ANY && expected_generation != 0)
            return -ESTALE;
        *removed = false;
        *generation = repository->generation;
        return 0;
    }
    if (expected_generation != DPPD_RULE_GENERATION_ANY && expected_generation != record->rule.generation)
        return -ESTALE;

    /** 删除同样需要一个新的有效版本，不能先清空记录再发现版本号已耗尽 */
    if (repository->generation >= UINT64_MAX - 1U)
        return -EOVERFLOW;
    if (dppd_slot_index_erase(&repository->index, rule_id, 0) != 0)
        return -EUCLEAN;
    memset(record, 0, sizeof(*record));
    repository->count--;
    *removed = true;
    /* 删除同样推进 generation，避免重放旧版本时出现 ABA 问题 */
    *generation = ++repository->generation;
    return 0;
}

int dppd_rule_repository_restore(struct dppd_rule_repository *repository, const struct dppd_rule *rules, uint32_t count,
    uint64_t repository_generation)
{
    uint32_t i;
    char validation_error[128];

    if (repository == NULL || repository->records == NULL || (count != 0 && rules == NULL) ||
        count > repository->capacity)
        return -EINVAL;
    /* 启动恢复只允许发布到全新空仓库，避免覆盖仍有硬件对象对应的在线状态 */
    if (repository->count != 0 || repository->generation != 0 || repository->index.count != 0)
        return -EBUSY;

    for (i = 0; i < count; ++i) {
        if (rules[i].id == 0 || rules[i].generation == 0 || rules[i].generation > repository_generation ||
            (i != 0 && rules[i - 1U].id >= rules[i].id) ||
            dppd_rule_validate(&rules[i], validation_error, sizeof(validation_error)) != 0)
            return -EINVAL;
    }

    /*
     * records 已在 init 时一次性分配；完成上面的全量预检后，下面的发布路径不再
     * 分配内存也不调用外部组件，因此不会出现只写入部分规则的失败点
     */
    for (i = 0; i < count; ++i) {
        uint32_t slot;
        if (dppd_slot_index_insert(&repository->index, rules[i].id, 0, &slot) != 0) {
            dppd_slot_index_clear(&repository->index);
            return -EUCLEAN;
        }
    }
    for (i = 0; i < count; ++i) {
        uint32_t slot = dppd_slot_index_find(&repository->index, rules[i].id, 0);
        repository->records[slot].occupied = true;
        repository->records[slot].rule = rules[i];
    }
    repository->count = count;
    repository->generation = repository_generation;
    return 0;
}
