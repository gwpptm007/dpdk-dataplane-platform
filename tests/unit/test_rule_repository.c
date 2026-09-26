#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include "dppd/rule_repository.h"

static struct dppd_rule make_rule(uint64_t id)
{
    struct dppd_rule rule;

    memset(&rule, 0, sizeof(rule));
    rule.id = id;
    rule.domain = DPPD_RULE_DOMAIN_INGRESS;
    rule.fallback = DPPD_FALLBACK_PREFER_HARDWARE;
    rule.nb_matches = 2;
    rule.matches[0].type = DPPD_MATCH_ETH;
    rule.matches[1].type = DPPD_MATCH_IPV4;
    rule.nb_actions = 2;
    rule.actions[0].type = DPPD_ACTION_COUNT;
    rule.actions[1].type = DPPD_ACTION_QUEUE;
    rule.actions[1].conf.queue_id = 0;
    return rule;
}

int main(void)
{
    struct dppd_rule_repository repository;
    struct dppd_rule_apply_result result;
    struct dppd_rule page[2];
    struct dppd_rule stored;
    struct dppd_rule rule = make_rule(1001);
    uint64_t deletion_generation;
    uint64_t page_generation;
    uint32_t page_count;
    bool has_more;
    bool removed;

    assert(dppd_rule_repository_init(NULL, 1) == -EINVAL);
    assert(dppd_rule_repository_init(&repository, 0) == -EINVAL);
    assert(dppd_rule_repository_init(&repository, 1) == 0);
    assert(dppd_rule_repository_count(&repository) == 0);
    assert(dppd_rule_repository_generation(&repository) == 0);

    assert(dppd_rule_repository_apply(&repository, &rule, 0, &result) == 0);
    assert(result.status == DPPD_RULE_CREATED);
    assert(result.generation == 1);
    assert(dppd_rule_repository_count(&repository) == 1);
    assert(dppd_rule_repository_get(&repository, rule.id, &stored) == 0);
    assert(stored.id == rule.id && stored.generation == 1);

    assert(dppd_rule_repository_apply(&repository, &rule, 0, &result) == 0);
    assert(result.status == DPPD_RULE_UNCHANGED);
    assert(result.generation == 1);

    rule.priority = 10;
    assert(dppd_rule_repository_apply(&repository, &rule, 0, &result) == -ESTALE);
    assert(dppd_rule_repository_generation(&repository) == 1);
    assert(dppd_rule_repository_apply(&repository, &rule, 1, &result) == 0);
    assert(result.status == DPPD_RULE_UPDATED);
    assert(result.generation == 2);

    rule = make_rule(1002);
    assert(dppd_rule_repository_apply(&repository, &rule, 0, &result) == -ENOSPC);
    assert(dppd_rule_repository_generation(&repository) == 2);

    assert(dppd_rule_repository_remove(&repository, 1001, 1, &removed,
                                       &deletion_generation) == -ESTALE);
    assert(dppd_rule_repository_remove(&repository, 1001, 2, &removed,
                                       &deletion_generation) == 0);
    assert(removed && deletion_generation == 3);
    assert(dppd_rule_repository_count(&repository) == 0);
    assert(dppd_rule_repository_get(&repository, 1001, &stored) == -ENOENT);

    assert(dppd_rule_repository_remove(&repository, 1001, 0, &removed,
                                       &deletion_generation) == 0);
    assert(!removed && deletion_generation == 3);

    rule = make_rule(0);
    assert(dppd_rule_repository_apply(&repository, &rule, 0, &result) == -EINVAL);
    dppd_rule_repository_destroy(&repository);
    assert(repository.records == NULL);
    assert(repository.capacity == 0);

    /*
     * 列表顺序按 rule id 而不是内部槽位。特意用 30、10、20 的写入顺序验证
     * cursor 稳定性，并模拟翻页间发生写入时 generation 冲突。
     */
    assert(dppd_rule_repository_init(&repository, 5) == 0);
    rule = make_rule(30);
    assert(dppd_rule_repository_apply(&repository, &rule, 0, &result) == 0);
    rule = make_rule(10);
    assert(dppd_rule_repository_apply(&repository, &rule, 0, &result) == 0);
    rule = make_rule(20);
    assert(dppd_rule_repository_apply(&repository, &rule, 0, &result) == 0);

    assert(dppd_rule_repository_list(
               &repository, 0, DPPD_RULE_GENERATION_ANY, page, 2,
               &page_count, &has_more, &page_generation) == 0);
    assert(page_generation == 3);
    assert(page_count == 2 && has_more);
    assert(page[0].id == 10 && page[1].id == 20);

    assert(dppd_rule_repository_list(
               &repository, 20, page_generation, page, 2,
               &page_count, &has_more, &page_generation) == 0);
    assert(page_count == 1 && !has_more && page[0].id == 30);

    rule = make_rule(40);
    assert(dppd_rule_repository_apply(&repository, &rule, 0, &result) == 0);
    assert(dppd_rule_repository_list(
               &repository, 20, 3, page, 2, &page_count,
               &has_more, &page_generation) == -ESTALE);
    /**
     * 故意让第二条的旧版本或规则内容出错，确认第一条优先级没有提前写入
     * 全部条件正确后，第二条即使内容不变也必须取得连续的新版本
     * 最后把全局版本放到边界附近，确认整批无法分配版本时不会改动已有记录
     */
    {
        struct dppd_rule updates[2] = {make_rule(10), make_rule(20)};
        uint64_t expected[2] = {2, 99};

        updates[0].priority = 42;
        assert(dppd_rule_repository_update_batch(&repository, updates,
                                                  expected, 2) == -ESTALE);
        assert(repository.generation == 4);
        assert(dppd_rule_repository_get(&repository, 10, &stored) == 0);
        assert(stored.generation == 2 && stored.priority == 0);
        expected[1] = 3;
        updates[1].id = 10;
        assert(dppd_rule_repository_update_batch(&repository, updates,
                                                  expected, 2) == -EEXIST);
        updates[1].id = 20;
        updates[1].nb_actions = 0;
        assert(dppd_rule_repository_update_batch(&repository, updates,
                                                  expected, 2) == -EINVAL);
        assert(repository.generation == 4);
        updates[1] = make_rule(20);
        assert(dppd_rule_repository_update_batch(&repository, updates, expected, 2) == 0);
        assert(repository.generation == 6);
        assert(dppd_rule_repository_get(&repository, 20, &stored) == 0);
        assert(stored.generation == 6);
        expected[0] = 5;
        expected[1] = 6;
        repository.generation = UINT64_MAX - 2;
        assert(dppd_rule_repository_update_batch(&repository, updates,
                                                  expected, 2) == -EOVERFLOW);
        assert(dppd_rule_repository_get(&repository, 10, &stored) == 0);
        assert(stored.generation == 5);
    }
    dppd_rule_repository_destroy(&repository);
    return 0;
}
