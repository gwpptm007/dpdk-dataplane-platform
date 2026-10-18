#ifndef DPPD_TEST_FLOW_STATE_FIXTURE_H
#define DPPD_TEST_FLOW_STATE_FIXTURE_H

#include <assert.h>
#include <stdlib.h>
#include "dppd/flow_state.h"

/** 规则版本故意留空洞，恢复后要保留删除历史，不能按当前条数重新编号 */
static inline struct dppd_flow_state *flow_state_fixture(uint32_t count)
{
    struct dppd_flow_state *s = calloc(1, sizeof(*s));
    assert(s != NULL);
    uint32_t capacity = count < 4 ? 4 : count;
    s->config = (struct dppd_flow_state_config){.mode = DPPD_FLOW_STATE_SOFTWARE,
        .port_id = 7,
        .rule_capacity = capacity,
        .queue_count = 2,
        .depth = 64,
        .queue_capacity = {capacity + 64, capacity + 64},
        .timeout_ns = UINT64_C(1000000000),
        .template_leases = 2U * capacity + 256,
        .shared_leases = 65536};
    s->config.admission.ticket_capacity = 2U * capacity + 256;
    s->config.admission.port_count = 1;
    s->config.admission.ports[0].port_id = 7;
    for (unsigned int k = 0; k < DPPD_RESOURCE_KIND_COUNT; ++k)
        s->config.admission.limits[k] = s->config.admission.ports[0].limits[k] = capacity + 128;
    s->table_count = 1;
    s->tables[0].key = 100;
    s->tables[0].spec = (struct dppd_flow_template_spec){.capacity = capacity + 64, .dynamic_actions = 1};
    struct dppd_rule *example = &s->tables[0].spec.example;
    *example = (struct dppd_rule){.install_port_id = 7,
        .fallback = DPPD_FALLBACK_SOFTWARE_ONLY,
        .nb_matches = 1,
        .nb_actions = 3};
    example->matches[0].type = DPPD_MATCH_ETH;
    example->actions[0] = (struct dppd_action){.type = DPPD_ACTION_MARK, .conf.mark_id = 42};
    example->actions[1].type = DPPD_ACTION_COUNT;
    example->actions[2].type = DPPD_ACTION_DROP;
    s->shared_next_generation = 50;
    s->revision = (uint64_t)count * 10U + 91;
    s->rule_count = count;
    if (count != 0) {
        s->rules = calloc(count, sizeof(*s->rules));
        assert(s->rules != NULL);
    }
    for (uint32_t i = 0; i < count; ++i) {
        s->rules[i] = (struct dppd_flow_state_rule){.table_key = 100, .queue = i % 2U, .rule = *example};
        s->rules[i].rule.id = i + 1U;
        s->rules[i].rule.generation = i * 10U + 3;
    }
    return s;
}

#endif
