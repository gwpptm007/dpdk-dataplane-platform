#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "dppd/software_backend.h"

static struct dppd_rule make_rule(unsigned int index)
{
    struct dppd_rule rule = {.id = index + 1,
        .generation = 1,
        .install_port_id = 0,
        .domain = DPPD_RULE_DOMAIN_INGRESS,
        .fallback = DPPD_FALLBACK_SOFTWARE_ONLY,
        .nb_matches = 3,
        .nb_actions = 2};
    rule.matches[0].type = DPPD_MATCH_ETH;
    rule.matches[1].type = DPPD_MATCH_IPV4;
    rule.matches[1].spec.ipv4 = (struct dppd_match_ipv4){1, UINT32_MAX, 2, UINT32_MAX};
    rule.matches[2].type = DPPD_MATCH_TCP;
    rule.matches[2].spec.l4 = (struct dppd_match_l4){7, UINT16_MAX, (uint16_t)index, UINT16_MAX};
    rule.actions[0].type = DPPD_ACTION_COUNT;
    rule.actions[1].type = DPPD_ACTION_DROP;
    return rule;
}

static void install(struct dppd_software_backend *backend, const struct dppd_rule *rule)
{
    struct dppd_transaction_item item = {.rule = *rule};
    item.plan.rule_id = rule->id;
    item.plan.rule_generation = rule->generation;
    item.plan.install_port_id = rule->install_port_id;
    item.plan.backend = DPPD_PLAN_BACKEND_SOFTWARE;
    struct dppd_transaction transaction;
    struct dppd_transaction_backends backends = {.software = dppd_software_transaction_backend(backend)};
    assert(dppd_transaction_init(&transaction, rule->id, &item, 1) == 0);
    int rc = dppd_transaction_run(&transaction, &backends);
    if (rc != 0)
        fprintf(stderr, "install id=%llu rc=%d stage=%d\n", (unsigned long long)rule->id, rc,
            transaction.failure_stage);
    assert(rc == 0);
    assert(dppd_transaction_finalize(&transaction, &backends) == 0);
}

static void expect_count(struct dppd_software_backend *backend, uint64_t id, uint64_t generation,
    uint64_t expected_hits, uint64_t expected_bytes)
{
    uint64_t hits, bytes;
    assert(dppd_software_backend_query_count(backend, id, generation, &hits, &bytes) == 0);
    assert(hits == expected_hits && bytes == expected_bytes);
}

static struct dppd_packet packet(unsigned int index)
{
    return (struct dppd_packet){.packet_len = 100 + index,
        .l3_type = DPPD_L3_IPV4,
        .l4_type = DPPD_L4_TCP,
        .ipv4_src_be = 1,
        .ipv4_dst_be = 2,
        .l4_src_port_be = 7,
        .l4_dst_port_be = (uint16_t)index};
}

struct reader {
    struct dppd_software_backend *backend;
    unsigned int id;
};

/** 两个真实线程分别归并再提交，核对共享计数器没有因批次覆盖而丢失增量 */
static void *read_packets(void *context)
{
    struct reader *reader = context;
    assert(dppd_software_backend_worker_register(reader->backend, reader->id) == 0);
    struct dppd_packet input = packet(2);
    struct dppd_software_count_batch batch = {0};
    for (unsigned int i = 0; i < 10000; ++i) {
        struct dppd_software_decision decision;
        dppd_software_backend_decide_counted(reader->backend, 0, &input, &decision, &batch);
        assert(decision.matched && decision.drop);
        if (i % 123 == 0) {
            dppd_software_count_batch_flush(&batch);
            dppd_software_backend_worker_quiescent(reader->backend, reader->id);
        }
    }
    dppd_software_count_batch_flush(&batch);
    dppd_software_backend_worker_unregister(reader->backend, reader->id);
    return NULL;
}

int main(void)
{
    enum { RULES = 40 };
    struct dppd_software_backend backend;
    assert(dppd_software_backend_init(&backend, RULES) == 0);
    assert(dppd_software_backend_worker_register(&backend, 0) == 0);
    for (unsigned int i = 0; i < RULES; ++i) {
        struct dppd_rule rule = make_rule(i);
        install(&backend, &rule);
    }
    struct dppd_software_count_batch batch = {0};
    struct dppd_software_decision decision;
    struct dppd_packet input = packet(0);
    for (unsigned int i = 0; i < 256; ++i)
        dppd_software_backend_decide_counted(&backend, 0, &input, &decision, &batch);
    assert(decision.drop && batch.count == 1);
    expect_count(&backend, 1, 1, 0, 0);
    input.neighbor_retry = true;
    dppd_software_backend_decide_counted(&backend, 0, &input, &decision, &batch);
    dppd_software_count_batch_flush(&batch);
    dppd_software_count_batch_flush(&batch);
    expect_count(&backend, 1, 1, 256, 25600);

    /** 超过批次数量上限仍逐条保留 DROP 命中，字节数不能使用批次平均长度 */
    for (unsigned int i = 0; i < RULES; ++i) {
        input = packet(i);
        dppd_software_backend_decide_counted(&backend, 0, &input, &decision, &batch);
    }
    dppd_software_count_batch_flush(&batch);
    for (unsigned int i = 0; i < RULES; ++i)
        expect_count(&backend, i + 1, 1, i == 0 ? 257 : 1, i == 0 ? 25700 : 100 + i);

    /** 批次仍引用旧版本时替换快照，新版本从零开始，旧指针在静默前仍可安全提交 */
    input = packet(0);
    dppd_software_backend_decide_counted(&backend, 0, &input, &decision, &batch);
    struct dppd_rule replacement = make_rule(0);
    replacement.generation = 2;
    uint64_t expected = 1;
    assert(dppd_software_backend_update_batch(&backend, &replacement, &expected, 1) == 0);
    expect_count(&backend, 1, 2, 0, 0);
    dppd_software_backend_decide_counted(&backend, 0, &input, &decision, &batch);
    assert(batch.count == 2);
    dppd_software_count_batch_flush(&batch);
    dppd_software_backend_worker_quiescent(&backend, 0);
    expect_count(&backend, 1, 2, 1, 100);

    pthread_t threads[2];
    struct reader readers[2] = {{&backend, 1}, {&backend, 2}};
    for (unsigned int i = 0; i < 2; ++i)
        assert(pthread_create(&threads[i], NULL, read_packets, &readers[i]) == 0);
    for (unsigned int i = 0; i < 2; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    expect_count(&backend, 3, 1, 20001, 20001 * 102);
    dppd_software_backend_worker_unregister(&backend, 0);
    dppd_software_backend_fini(&backend);
    return 0;
}
