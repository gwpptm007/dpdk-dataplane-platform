#include <assert.h>
#include <string.h>
#include "dppd/rule.h"

static struct dppd_rule valid_ingress_rule(void)
{
    struct dppd_rule rule;

    memset(&rule, 0, sizeof(rule));
    rule.id = 1;
    rule.domain = DPPD_RULE_DOMAIN_INGRESS;
    rule.fallback = DPPD_FALLBACK_PREFER_HARDWARE;
    rule.nb_matches = 3;
    rule.matches[0].type = DPPD_MATCH_ETH;
    rule.matches[1].type = DPPD_MATCH_IPV4;
    rule.matches[2].type = DPPD_MATCH_UDP;
    rule.nb_actions = 2;
    rule.actions[0].type = DPPD_ACTION_COUNT;
    rule.actions[1].type = DPPD_ACTION_QUEUE;
    rule.actions[1].conf.queue_id = 0;
    return rule;
}

int main(void)
{
    struct dppd_rule rule = valid_ingress_rule();
    char error[128];

    assert(dppd_rule_validate(&rule, error, sizeof(error)) == 0);

    rule.matches[1].type = DPPD_MATCH_UDP;
    assert(dppd_rule_validate(&rule, error, sizeof(error)) != 0);

    rule = valid_ingress_rule();
    rule.matches[0].type = DPPD_MATCH_IPV4;
    rule.matches[1].type = DPPD_MATCH_ETH;
    assert(dppd_rule_validate(&rule, error, sizeof(error)) != 0);

    rule = valid_ingress_rule();
    rule.actions[1].type = DPPD_ACTION_DROP;
    assert(dppd_rule_validate(&rule, error, sizeof(error)) == 0);

    rule.actions[0].type = DPPD_ACTION_DROP;
    rule.actions[1].type = DPPD_ACTION_COUNT;
    assert(dppd_rule_validate(&rule, error, sizeof(error)) != 0);

    memset(&rule, 0, sizeof(rule));
    rule.domain = DPPD_RULE_DOMAIN_TRANSFER;
    rule.fallback = DPPD_FALLBACK_REQUIRE_HARDWARE;
    rule.nb_matches = 1;
    rule.matches[0].type = DPPD_MATCH_REPRESENTED_PORT;
    rule.matches[0].spec.ethdev_port_id = 2;
    rule.nb_actions = 1;
    rule.actions[0].type = DPPD_ACTION_REPRESENTED_PORT;
    rule.actions[0].conf.ethdev_port_id = 3;
    assert(dppd_rule_validate(&rule, error, sizeof(error)) == 0);

    rule.actions[0].type = DPPD_ACTION_QUEUE;
    assert(dppd_rule_validate(&rule, error, sizeof(error)) != 0);
    return 0;
}
