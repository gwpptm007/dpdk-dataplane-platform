#include "dppd/rule_repository.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/** 删除后槽位可以复用，是否有效由 occupied 决定，不能只看内存里是否仍有旧规则内容 */
struct dppd_rule_record {
    /** 此槽位是否保存有效规则，删除后清零，槽位可再次使用 */
    bool occupied;
    /** 控制面保存的完整规则副本，包含该规则最近一次创建或更新时的版本 */
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

/**
 * 将 rule 新增或更新到 records 数组，检查预期版本并分配新版本，不安装后端规则或写文件
 * expected_generation 是调用方预期的旧版本，0 用于期望新建，ANY 表示跳过版本冲突检查
 * 成功返回 0，result 给出 CREATED、UPDATED 或 UNCHANGED 及规则版本；失败返回负错误码
 * 调用方须串行访问这些规则记录，本函数不加锁
 *
 * 按 rule.id 查找记录
 *   已存在 → 内容相同 → UNCHANGED，返回已有版本，不检查 expected_generation
 *          → 内容不同 → 检查预期旧版本 → 覆盖内容并分配新版本 → UPDATED
 *   不存在 → 预期为 0 或 ANY → 分配槽位、写入内容及新版本 → CREATED
 *          → 预期为其他版本 → -ESTALE，调用方预期的记录已不存在
 */
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
        /** equal 比较规则内容，不比较规则自身的 generation；相同内容重复提交不推进版本 */
        if (dppd_rule_equal(&record->rule, rule)) {
            result->status = DPPD_RULE_UNCHANGED;
            result->generation = record->rule.generation;
            return 0;
        }
        /**
         * 例：调用方读到 R1 版本 7，其他请求已将其改成版本 9
         * 本次携带 expected_generation = 7 且内容不同 → -ESTALE，拒绝覆盖版本 9
         * 比较的是 R1 当前版本，不是整个规则集合的 generation；ANY 明确跳过此检查
         */
        if (expected_generation != DPPD_RULE_GENERATION_ANY && expected_generation != record->rule.generation)
            return -ESTALE;
        if (repository->generation >= UINT64_MAX - 1U)
            return -EOVERFLOW;
        record->rule = *rule;
        /**
         * 新版本取整个规则集合的递增序列，不采用输入 rule->generation，也不是旧规则版本加 1
         * 例：集合版本 12，R1 版本 7 → 用预期版本 7 更新成功 → 集合和 R1 均变为 13
         * 其他规则保留各自的版本，更新已有记录不增加 count
         */
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
    /**
     * 索引负责按 ID 查槽位，records 数组保存规则内容
     * 例：索引 (ID 20, 0) → 槽位 3 → records[3].rule
     * 索引键中的 0 固定占位，真正的规则版本保存在 record->rule.generation
     */
    int rc = dppd_slot_index_insert(&repository->index, rule->id, 0, &slot);
    if (rc != 0)
        return rc;
    record = &repository->records[slot];
    record->occupied = true;
    record->rule = *rule;
    /**
     * 新建规则：集合版本 12 → 递增为 13 → 将 13 写入这条规则
     * control_apply_impl 已用 12 + 1 安装候选规则；串行执行期间集合版本未变，两处版本因此一致
     * 这里才推进 repository->generation，其他已有规则的版本不变
     */
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
static int remove_batch(struct dppd_rule_repository *repository, const struct dppd_rule *rules, uint32_t count,
    bool advance)
{
    uint32_t slots[64];
    if (repository == NULL || repository->records == NULL || rules == NULL || count == 0 || count > 64)
        return -EINVAL;
    if (advance && repository->generation >= UINT64_MAX - count)
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
    if (advance)
        repository->generation += count;
    return 0;
}

int dppd_rule_repository_remove_batch(struct dppd_rule_repository *repository, const struct dppd_rule *rules,
    uint32_t count)
{
    return remove_batch(repository, rules, count, true);
}

int dppd_rule_repository_forget_batch(struct dppd_rule_repository *repository, const struct dppd_rule *rules,
    uint32_t count)
{
    return remove_batch(repository, rules, count, false);
}

/** 替换前完整校验，版本由同一控制线程连续分配，预检完成后不再分配内存 */
int dppd_rule_repository_replay_batch(struct dppd_rule_repository *repository, const struct dppd_rule *rules,
    uint32_t count)
{
    char error[128];
    if (repository == NULL || repository->records == NULL || rules == NULL || count == 0 || count > 64 ||
        count > repository->capacity - repository->count || repository->generation == UINT64_MAX)
        return -EINVAL;
    for (uint32_t i = 0; i < count; ++i) {
        if (rules[i].id == 0 || rules[i].id == UINT64_MAX || rules[i].generation == 0 ||
            rules[i].generation > repository->generation || dppd_rule_validate(&rules[i], error, sizeof(error)) != 0)
            return -EINVAL;
        if (dppd_slot_index_find(&repository->index, rules[i].id, 0) != DPPD_SLOT_NONE)
            return -EEXIST;
        for (uint32_t j = 0; j < i; ++j)
            if (rules[i].id == rules[j].id)
                return -EEXIST;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t slot;
        int rc = dppd_slot_index_insert(&repository->index, rules[i].id, 0, &slot);
        if (rc != 0) {
            for (uint32_t j = 0; j < i; ++j)
                dppd_slot_index_erase(&repository->index, rules[j].id, 0);
            return -EUCLEAN;
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t slot = dppd_slot_index_find(&repository->index, rules[i].id, 0);
        repository->records[slot].occupied = true;
        repository->records[slot].rule = rules[i];
    }
    repository->count += count;
    return 0;
}

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

/**
 * 删除内存账本中的规则，校验预期版本后撤销索引、清空记录并推进仓库修订号
 * 成功返回 0，removed 表示是否实际删除，generation 返回操作后的仓库版本；失败返回负错误码
 * 后端规则须由控制层先删除，本函数不操作 worker 快照或硬件对象
 * rule_id → 找到 record → slot_index_erase 归还索引槽位 → 清空 record → count 减 1
 */
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
