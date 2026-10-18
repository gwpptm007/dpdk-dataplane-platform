#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include "dppd/rule_repository.h"
#include "dppd/slot_index.h"
#include "dppd/control.h"

static bool fail_allocation;

void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size);

void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_allocation) {
        fail_allocation = false;
        return NULL;
    }
    return __real_calloc(count, size);
}

/** 编号与版本共同排序，插入新的排序项不能搬走旧事务仍然引用的槽位 */
static void test_versions_and_reuse(void)
{
    struct dppd_slot_index index = {0};
    const uint64_t ids[] = {9, 2, 2, UINT64_MAX, 9, 1};
    const uint64_t generations[] = {1, 8, 3, 1, 2, 0};
    uint32_t slots[6], untouched = 77;
    assert(dppd_slot_index_init(&index, 6) == 0);
    for (uint32_t i = 0; i < 6; ++i) {
        assert(dppd_slot_index_insert(&index, ids[i], generations[i], &slots[i]) == 0);
        for (uint32_t j = 0; j <= i; ++j)
            assert(dppd_slot_index_find(&index, ids[j], generations[j]) == slots[j]);
    }
    const uint32_t order[] = {5, 2, 1, 0, 4, 3};
    for (uint32_t i = 0; i < 6; ++i)
        assert(dppd_slot_index_at(&index, i) == slots[order[i]]);
    assert(dppd_slot_index_latest(&index, 2) == slots[1]);
    assert(dppd_slot_index_latest(&index, UINT64_MAX) == slots[3]);
    assert(dppd_slot_index_latest(&index, 7) == DPPD_SLOT_NONE);
    assert(dppd_slot_index_insert(&index, 9, 1, &untouched) == -EEXIST && untouched == 77);
    assert(dppd_slot_index_insert(&index, 3, 1, &untouched) == -ENOSPC && untouched == 77);
    assert(dppd_slot_index_erase(&index, 2, 2) == -ENOENT && index.count == 6);
    assert(dppd_slot_index_upper_bound(&index, 2, 3) == 2);
    assert(dppd_slot_index_upper_bound(&index, 2, UINT64_MAX) == 3);
    assert(dppd_slot_index_upper_bound(&index, UINT64_MAX, UINT64_MAX) == 6);
    assert(dppd_slot_index_erase(&index, 9, 2) == 0);
    assert(dppd_slot_index_latest(&index, 9) == slots[0]);
    assert(dppd_slot_index_insert(&index, 3, 1, &untouched) == 0 && untouched == slots[4]);
    assert(dppd_slot_index_find(&index, 9, 2) == DPPD_SLOT_NONE);
    dppd_slot_index_clear(&index);
    assert(index.count == 0 && dppd_slot_index_at(&index, 0) == DPPD_SLOT_NONE);
    assert(dppd_slot_index_find(&index, 3, 1) == DPPD_SLOT_NONE);
    assert(dppd_slot_index_insert(&index, 3, 1, &untouched) == 0 && untouched == 0);
    dppd_slot_index_fini(&index);
    assert(index.storage == NULL && index.capacity == 0);
}

/** 独立小数组作为真值，随机插入、删除和复用后逐项核对所有键及有序遍历 */
static void test_randomized_reference(void)
{
    struct dppd_slot_index index = {0};
    uint32_t expected[256], random = 0x81c9e375U;
    for (unsigned int i = 0; i < 256; ++i)
        expected[i] = DPPD_SLOT_NONE;
    assert(dppd_slot_index_init(&index, 32) == 0);
    for (unsigned int operation = 0; operation < 4096; ++operation) {
        random = random * 1664525U + 1013904223U;
        uint32_t key = (random >> 16) % 256, slot = 91;
        uint64_t id = key / 4U + 1U, generation = key % 4U;
        if ((random & 1U) != 0) {
            int rc = dppd_slot_index_insert(&index, id, generation, &slot);
            if (expected[key] != DPPD_SLOT_NONE)
                assert(rc == -EEXIST && slot == 91);
            else if (index.count == 32 && rc != 0)
                assert(rc == -ENOSPC && slot == 91);
            else {
                assert(rc == 0);
                expected[key] = slot;
            }
        } else {
            int rc = dppd_slot_index_erase(&index, id, generation);
            assert(rc == (expected[key] == DPPD_SLOT_NONE ? -ENOENT : 0));
            expected[key] = DPPD_SLOT_NONE;
        }
        uint32_t count = 0;
        for (uint32_t i = 0; i < 256; ++i) {
            assert(dppd_slot_index_find(&index, i / 4U + 1U, i % 4U) == expected[i]);
            if (expected[i] != DPPD_SLOT_NONE)
                assert(dppd_slot_index_at(&index, count++) == expected[i]);
        }
        assert(index.count == count && dppd_slot_index_at(&index, count) == DPPD_SLOT_NONE);
    }
    dppd_slot_index_fini(&index);
}

/** 达到固定规模后仍保留原槽位，不依赖规则编号恰好递增这一种有利输入 */
static void test_reverse_scale(void)
{
    struct dppd_slot_index index = {0};
    const uint32_t count = 16384;
    assert(dppd_slot_index_init(&index, count) == 0);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t slot;
        assert(dppd_slot_index_insert(&index, count - i, i, &slot) == 0 && slot == i);
    }
    for (uint32_t i = 0; i < count; ++i) {
        assert(dppd_slot_index_find(&index, count - i, i) == i);
        assert(dppd_slot_index_at(&index, i) == count - i - 1U);
    }
    for (uint32_t i = 0; i < count; ++i)
        assert(dppd_slot_index_erase(&index, count - i, i) == 0);
    assert(index.count == 0);
    dppd_slot_index_fini(&index);
}

/** 单条创建和替换也不能分配 ANY 保留版本，失败不得消耗一个槽位 */
static void test_repository_generation_limit(void)
{
    struct dppd_rule_repository repository;
    struct dppd_rule rule = {.id = 1,
        .nb_matches = 1,
        .matches = {{.type = DPPD_MATCH_ETH}},
        .nb_actions = 1,
        .actions = {{.type = DPPD_ACTION_DROP}}};
    struct dppd_rule_apply_result result;
    assert(dppd_rule_repository_init(&repository, 2) == 0);
    repository.generation = UINT64_MAX - 2U;
    assert(dppd_rule_repository_apply(&repository, &rule, 0, &result) == 0 && result.generation == UINT64_MAX - 1U);
    rule.priority = 7;
    assert(dppd_rule_repository_apply(&repository, &rule, UINT64_MAX - 1U, &result) == -EOVERFLOW);
    rule.id = 2;
    assert(dppd_rule_repository_apply(&repository, &rule, 0, &result) == -EOVERFLOW);
    assert(repository.count == 1 && repository.index.count == 1 && repository.generation == UINT64_MAX - 1U);
    rule.id = 1;
    rule.priority = 0;
    assert(dppd_rule_repository_apply(&repository, &rule, UINT64_MAX - 1U, &result) == 0);
    assert(result.status == DPPD_RULE_UNCHANGED);
    dppd_rule_repository_destroy(&repository);
}

static void test_initialization_failure(void)
{
    struct dppd_slot_index index = {.capacity = 91};
    assert(dppd_slot_index_init(NULL, 1) == -EINVAL);
    assert(dppd_slot_index_init(&index, 0) == -EINVAL && index.capacity == 91);
    fail_allocation = true;
    assert(dppd_slot_index_init(&index, 16) == -ENOMEM && index.storage == NULL && index.capacity == 91);
    assert(dppd_slot_index_init(&index, 16) == 0);
    uint32_t slot = 92;
    assert(dppd_slot_index_insert(&index, 0, 1, &slot) == -EINVAL && slot == 92);
    assert(dppd_slot_index_find(&index, 0, 1) == DPPD_SLOT_NONE);
    assert(dppd_slot_index_erase(&index, 0, 1) == -EINVAL);
    dppd_slot_index_fini(&index);
}

static unsigned int driver_calls;

static int memory_validate(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_error *error)
{
    (void)port;
    (void)rule;
    (void)error;
    ++driver_calls;
    return 0;
}

static int memory_create(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_handle *handle,
    struct dppd_flow_error *error)
{
    (void)error;
    ++driver_calls;
    *handle = (struct dppd_flow_handle){.rule_id = rule->id,
        .rule_generation = rule->generation,
        .port_id = port,
        .flow = (struct rte_flow *)(uintptr_t)(rule->id + 1U)};
    return 0;
}

static int memory_remove(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)handle;
    (void)error;
    ++driver_calls;
    return 0;
}

/** 全局版本耗尽时，正式控制入口必须在任何驱动访问前拒绝创建或替换，幂等读取仍可用 */
static void test_control_generation_preflight(void)
{
    const struct dppd_topology topology = {.nb_endpoints = 1, .endpoints = {{.ethdev_port_id = 5}}};
    const struct dppd_flow_api api = {.validate = memory_validate, .create = memory_create, .remove = memory_remove};
    struct dppd_control_service control;
    struct dppd_control_apply_result result;
    struct dppd_rule rule = {.id = 1,
        .nb_matches = 1,
        .matches = {{.type = DPPD_MATCH_ETH}},
        .nb_actions = 1,
        .actions = {{.type = DPPD_ACTION_DROP}}};
    assert(dppd_control_init(&control, &topology, 2, &api) == 0);
    assert(dppd_control_apply(&control, 5, &rule, 0, &result) == 0);
    unsigned int calls = driver_calls;
    control.rules.generation = UINT64_MAX - 1U;
    rule.priority = 7;
    assert(dppd_control_apply(&control, 5, &rule, 1, &result) == -EOVERFLOW && driver_calls == calls);
    rule.id = 2;
    assert(dppd_control_apply(&control, 5, &rule, 0, &result) == -EOVERFLOW && driver_calls == calls);
    assert(control.rules.count == 1 && control.rte_flow.count == 1);
    rule.id = 1;
    rule.priority = 0;
    assert(dppd_control_apply(&control, 5, &rule, 1, &result) == 0 && result.status == DPPD_RULE_UNCHANGED);
    assert(driver_calls == calls && dppd_control_fini(&control) == 0);
}

int main(void)
{
    test_initialization_failure();
    test_versions_and_reuse();
    test_randomized_reference();
    test_reverse_scale();
    test_repository_generation_limit();
    test_control_generation_preflight();
    return 0;
}
