#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "dppd/software_backend.h"

static struct dppd_rule make_rule(uint64_t id, uint32_t priority,
                                  uint32_t mark_id, bool with_count)
{
    struct dppd_rule rule;

    memset(&rule, 0, sizeof(rule));
    rule.id = id;
    rule.generation = id;
    rule.install_port_id = 5;
    rule.domain = DPPD_RULE_DOMAIN_INGRESS;
    rule.fallback = DPPD_FALLBACK_SOFTWARE_ONLY;
    rule.priority = priority;
    rule.nb_matches = 3;
    rule.matches[0].type = DPPD_MATCH_ETH;
    rule.matches[1].type = DPPD_MATCH_IPV4;
    rule.matches[1].spec.ipv4.src_be = 0x0101010aU;
    rule.matches[1].spec.ipv4.src_mask_be = UINT32_MAX;
    rule.matches[1].spec.ipv4.dst_be = 0x0202020aU;
    rule.matches[1].spec.ipv4.dst_mask_be = UINT32_MAX;
    rule.matches[2].type = DPPD_MATCH_TCP;
    rule.matches[2].spec.l4.src_be = 0xd204U;
    rule.matches[2].spec.l4.src_mask_be = UINT16_MAX;
    rule.matches[2].spec.l4.dst_be = 0xe110U;
    rule.matches[2].spec.l4.dst_mask_be = UINT16_MAX;
    rule.nb_actions = with_count ? 3 : 2;
    rule.actions[0].type = DPPD_ACTION_MARK;
    rule.actions[0].conf.mark_id = mark_id;
    if (with_count)
        rule.actions[1].type = DPPD_ACTION_COUNT;
    rule.actions[with_count ? 2 : 1].type = DPPD_ACTION_DROP;
    return rule;
}

static void install(struct dppd_software_backend *backend,
                    const struct dppd_rule *rule)
{
    struct dppd_transaction_item item;
    struct dppd_transaction transaction;
    struct dppd_transaction_backends backends;

    /*
     * 测试也经由正式 transaction 路径安装规则，而不直接构造 snapshot。这样能够同时
     * 覆盖 prepare token、不可变发布和 QSBR 回收前提；plan 中的 ID/generation 必须与
     * rule 一致，否则 transaction 在 validate 阶段应拒绝该测试输入。
     */
    memset(&item, 0, sizeof(item));
    memset(&backends, 0, sizeof(backends));
    item.rule = *rule;
    item.plan.rule_id = rule->id;
    item.plan.rule_generation = rule->generation;
    item.plan.install_port_id = rule->install_port_id;
    item.plan.backend = DPPD_PLAN_BACKEND_SOFTWARE;
    backends.software = dppd_software_transaction_backend(backend);
    assert(dppd_transaction_init(&transaction, rule->id, &item, 1) == 0);
    assert(dppd_transaction_run(&transaction, &backends) == 0);
}

int main(void)
{
    struct dppd_software_backend backend;
    struct dppd_software_decision decision;
    struct dppd_packet packet;
    struct dppd_rule counted;
    struct dppd_rule preferred;
    struct dppd_rule unsupported;
    uint64_t hits;
    uint64_t bytes;

    memset(&packet, 0, sizeof(packet));
    packet.packet_len = 128;
    packet.l3_type = DPPD_L3_IPV4;
    packet.l4_type = DPPD_L4_TCP;
    packet.ipv4_src_be = 0x0101010aU;
    packet.ipv4_dst_be = 0x0202020aU;
    packet.l4_src_port_be = 0xd204U;
    packet.l4_dst_port_be = 0xe110U;
    assert(dppd_software_backend_init(&backend, 4) == 0);
    /*
     * 模拟真实 worker：发布后的旧 snapshot 必须等到该静默点以后才可回收。每次 decide
     * 后显式报告静默点，既验证正常运行的调用顺序，也让后续发布可以回收不再被本测试
     * reader 引用的旧版本。
     */
    assert(dppd_software_backend_worker_register(&backend, 0) == 0);

    counted = make_rule(1, 20, 7, true);
    assert(dppd_software_backend_rule_supported(&counted));
    install(&backend, &counted);
    dppd_software_backend_decide(&backend, 5, &packet, &decision);
    dppd_software_backend_worker_quiescent(&backend, 0);
    assert(decision.matched && decision.drop && decision.has_mark);
    assert(decision.mark_id == 7);
    assert(dppd_software_backend_query_count(&backend, 1, 1,
                                              &hits, &bytes) == 0);
    assert(hits == 1 && bytes == 128);

    /*
     * 更小的 priority 优先，验证规则物理槽位顺序不会改变 classifier 语义。同时首条
     * counted 规则的 COUNT 不应因 snapshot 克隆而归零，因此仍断言 hits/bytes 保持在
     * 首次命中后的值。
     */
    preferred = make_rule(2, 10, 9, false);
    install(&backend, &preferred);
    dppd_software_backend_decide(&backend, 5, &packet, &decision);
    dppd_software_backend_worker_quiescent(&backend, 0);
    assert(decision.matched && decision.drop && decision.has_mark);
    assert(decision.mark_id == 9);
    assert(dppd_software_backend_query_count(&backend, 1, 1,
                                              &hits, &bytes) == 0);
    assert(hits == 1 && bytes == 128);

    assert(dppd_software_backend_remove_version(&backend, 2, 2) == 0);
    dppd_software_backend_decide(&backend, 5, &packet, &decision);
    dppd_software_backend_worker_quiescent(&backend, 0);
    assert(decision.mark_id == 7);
    assert(dppd_software_backend_query_count(&backend, 1, 1,
                                              &hits, &bytes) == 0);
    assert(hits == 2 && bytes == 256);

    unsupported = counted;
    unsupported.id = 3;
    unsupported.generation = 3;
    unsupported.actions[2].type = DPPD_ACTION_QUEUE;
    assert(!dppd_software_backend_rule_supported(&unsupported));
    assert(dppd_software_backend_remove_version(&backend, 1, 1) == 0);
    dppd_software_backend_worker_unregister(&backend, 0);
    dppd_software_backend_fini(&backend);
    return 0;
}
