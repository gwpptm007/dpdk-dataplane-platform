#include "dppd/rule_repository.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct dppd_rule_record {
    bool occupied;
    struct dppd_rule rule;
};

static struct dppd_rule_record *find_record(struct dppd_rule_repository *repository,
                                            uint64_t rule_id)
{
    uint32_t i;

    for (i = 0; i < repository->capacity; ++i) {
        if (repository->records[i].occupied &&
            repository->records[i].rule.id == rule_id)
            return &repository->records[i];
    }
    return NULL;
}

static const struct dppd_rule_record *find_record_const(
    const struct dppd_rule_repository *repository, uint64_t rule_id)
{
    uint32_t i;

    for (i = 0; i < repository->capacity; ++i) {
        if (repository->records[i].occupied &&
            repository->records[i].rule.id == rule_id)
            return &repository->records[i];
    }
    return NULL;
}

static struct dppd_rule_record *find_empty_record(
    struct dppd_rule_repository *repository)
{
    uint32_t i;

    for (i = 0; i < repository->capacity; ++i) {
        if (!repository->records[i].occupied)
            return &repository->records[i];
    }
    return NULL;
}

int dppd_rule_repository_init(struct dppd_rule_repository *repository,
                              uint32_t capacity)
{
    if (repository == NULL || capacity == 0)
        return -EINVAL;
    memset(repository, 0, sizeof(*repository));
    repository->records = calloc(capacity, sizeof(*repository->records));
    if (repository->records == NULL)
        return -ENOMEM;
    repository->capacity = capacity;
    return 0;
}

void dppd_rule_repository_destroy(struct dppd_rule_repository *repository)
{
    if (repository == NULL)
        return;
    free(repository->records);
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

int dppd_rule_repository_get(const struct dppd_rule_repository *repository,
                             uint64_t rule_id,
                             struct dppd_rule *rule)
{
    const struct dppd_rule_record *record;

    if (repository == NULL || repository->records == NULL ||
        rule_id == 0 || rule == NULL)
        return -EINVAL;
    record = find_record_const(repository, rule_id);
    if (record == NULL)
        return -ENOENT;
    *rule = record->rule;
    return 0;
}

int dppd_rule_repository_list(
    const struct dppd_rule_repository *repository,
    uint64_t after_rule_id,
    uint64_t expected_repository_generation,
    struct dppd_rule *rules,
    uint32_t limit,
    uint32_t *count,
    bool *has_more,
    uint64_t *repository_generation)
{
    uint64_t cursor = after_rule_id;
    uint32_t output_count = 0;
    uint32_t i;

    if (repository == NULL || repository->records == NULL || rules == NULL ||
        limit == 0 || count == NULL || has_more == NULL ||
        repository_generation == NULL)
        return -EINVAL;
    if (expected_repository_generation != DPPD_RULE_GENERATION_ANY &&
        expected_repository_generation != repository->generation)
        return -ESTALE;

    /*
     * record 槽位会因删除和复用而变化，不能把槽位下标暴露成分页 cursor。
     * 每次选择“大于 cursor 的最小 rule id”，得到与内部存储布局无关的稳定顺序。
     * 当前 repository 是小容量 baseline，O(capacity × page_size) 可接受；规模化后
     * 应替换为有序索引，同时保持本接口的 cursor/generation 语义。
     */
    while (output_count < limit) {
        const struct dppd_rule_record *next = NULL;

        for (i = 0; i < repository->capacity; ++i) {
            const struct dppd_rule_record *record = &repository->records[i];

            if (!record->occupied || record->rule.id <= cursor)
                continue;
            if (next == NULL || record->rule.id < next->rule.id)
                next = record;
        }
        if (next == NULL)
            break;
        rules[output_count++] = next->rule;
        cursor = next->rule.id;
    }

    *has_more = false;
    for (i = 0; i < repository->capacity; ++i) {
        if (repository->records[i].occupied &&
            repository->records[i].rule.id > cursor) {
            *has_more = true;
            break;
        }
    }
    *count = output_count;
    *repository_generation = repository->generation;
    return 0;
}

int dppd_rule_repository_apply(struct dppd_rule_repository *repository,
                               const struct dppd_rule *rule,
                               uint64_t expected_generation,
                               struct dppd_rule_apply_result *result)
{
    struct dppd_rule_record *record;
    char validation_error[128];

    if (repository == NULL || repository->records == NULL ||
        rule == NULL || rule->id == 0 || result == NULL)
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
        if (expected_generation != DPPD_RULE_GENERATION_ANY &&
            expected_generation != record->rule.generation)
            return -ESTALE;
        record->rule = *rule;
        /* generation 只由 repository 分配，调用者携带的值不会直接发布。 */
        record->rule.generation = ++repository->generation;
        result->status = DPPD_RULE_UPDATED;
        result->generation = record->rule.generation;
        return 0;
    }

    if (expected_generation != DPPD_RULE_GENERATION_ANY &&
        expected_generation != 0)
        return -ESTALE;
    record = find_empty_record(repository);
    if (record == NULL)
        return -ENOSPC;
    record->occupied = true;
    record->rule = *rule;
    /* create 与 update 共用同一条全局 desired-state 修订序列。 */
    record->rule.generation = ++repository->generation;
    repository->count++;
    result->status = DPPD_RULE_CREATED;
    result->generation = record->rule.generation;
    return 0;
}

int dppd_rule_repository_remove(struct dppd_rule_repository *repository,
                                uint64_t rule_id,
                                uint64_t expected_generation,
                                bool *removed,
                                uint64_t *generation)
{
    struct dppd_rule_record *record;

    if (repository == NULL || repository->records == NULL || rule_id == 0 ||
        removed == NULL || generation == NULL)
        return -EINVAL;
    record = find_record(repository, rule_id);
    if (record == NULL) {
        if (expected_generation != DPPD_RULE_GENERATION_ANY &&
            expected_generation != 0)
            return -ESTALE;
        *removed = false;
        *generation = repository->generation;
        return 0;
    }
    if (expected_generation != DPPD_RULE_GENERATION_ANY &&
        expected_generation != record->rule.generation)
        return -ESTALE;

    memset(record, 0, sizeof(*record));
    repository->count--;
    *removed = true;
    /* 删除同样推进 generation，避免重放旧版本时出现 ABA 问题。 */
    *generation = ++repository->generation;
    return 0;
}

int dppd_rule_repository_restore(struct dppd_rule_repository *repository,
                                 const struct dppd_rule *rules,
                                 uint32_t count,
                                 uint64_t repository_generation)
{
    uint32_t i;
    char validation_error[128];

    if (repository == NULL || repository->records == NULL ||
        (count != 0 && rules == NULL) || count > repository->capacity)
        return -EINVAL;
    /* 启动恢复只允许发布到全新空仓库，避免覆盖仍有硬件对象对应的在线状态。 */
    if (repository->count != 0 || repository->generation != 0)
        return -EBUSY;

    for (i = 0; i < count; ++i) {
        if (rules[i].id == 0 || rules[i].generation == 0 ||
            rules[i].generation > repository_generation ||
            (i != 0 && rules[i - 1U].id >= rules[i].id) ||
            dppd_rule_validate(&rules[i], validation_error,
                               sizeof(validation_error)) != 0)
            return -EINVAL;
    }

    /*
     * records 已在 init 时一次性分配；完成上面的全量预检后，下面的发布路径不再
     * 分配内存也不调用外部组件，因此不会出现只写入部分规则的失败点。
     */
    for (i = 0; i < count; ++i) {
        repository->records[i].occupied = true;
        repository->records[i].rule = rules[i];
    }
    repository->count = count;
    repository->generation = repository_generation;
    return 0;
}
