#include "dppd/rule.h"

#include <stdbool.h>
#include <stdio.h>

static int rule_error(char *error, uint32_t error_len, const char *message)
{
    if (error != NULL && error_len != 0)
        snprintf(error, error_len, "%s", message);
    return -1;
}

static bool match_equal(const struct dppd_match *left,
                        const struct dppd_match *right)
{
    if (left->type != right->type)
        return false;
    switch (left->type) {
    case DPPD_MATCH_ETH:
        return true;
    case DPPD_MATCH_IPV4:
        return left->spec.ipv4.src_be == right->spec.ipv4.src_be &&
               left->spec.ipv4.src_mask_be == right->spec.ipv4.src_mask_be &&
               left->spec.ipv4.dst_be == right->spec.ipv4.dst_be &&
               left->spec.ipv4.dst_mask_be == right->spec.ipv4.dst_mask_be;
    case DPPD_MATCH_UDP:
    case DPPD_MATCH_TCP:
        return left->spec.l4.src_be == right->spec.l4.src_be &&
               left->spec.l4.src_mask_be == right->spec.l4.src_mask_be &&
               left->spec.l4.dst_be == right->spec.l4.dst_be &&
               left->spec.l4.dst_mask_be == right->spec.l4.dst_mask_be;
    case DPPD_MATCH_REPRESENTED_PORT:
        return left->spec.ethdev_port_id == right->spec.ethdev_port_id;
    }
    return false;
}

static bool action_equal(const struct dppd_action *left,
                         const struct dppd_action *right)
{
    if (left->type != right->type)
        return false;
    switch (left->type) {
    case DPPD_ACTION_DROP:
    case DPPD_ACTION_COUNT:
        return true;
    case DPPD_ACTION_QUEUE:
        return left->conf.queue_id == right->conf.queue_id;
    case DPPD_ACTION_MARK:
        return left->conf.mark_id == right->conf.mark_id;
    case DPPD_ACTION_REPRESENTED_PORT:
        return left->conf.ethdev_port_id == right->conf.ethdev_port_id;
    }
    return false;
}

bool dppd_rule_equal(const struct dppd_rule *left, const struct dppd_rule *right)
{
    uint16_t i;

    if (left == NULL || right == NULL ||
        left->id != right->id ||
        left->install_port_id != right->install_port_id ||
        left->domain != right->domain ||
        left->fallback != right->fallback || left->group != right->group ||
        left->priority != right->priority ||
        left->nb_matches != right->nb_matches ||
        left->nb_actions != right->nb_actions)
        return false;
    for (i = 0; i < left->nb_matches; ++i) {
        if (!match_equal(&left->matches[i], &right->matches[i]))
            return false;
    }
    for (i = 0; i < left->nb_actions; ++i) {
        if (!action_equal(&left->actions[i], &right->actions[i]))
            return false;
    }
    return true;
}

int dppd_rule_validate(const struct dppd_rule *rule, char *error, uint32_t error_len)
{
    uint16_t i;
    uint16_t fate_actions = 0;
    uint16_t count_actions = 0;
    uint32_t seen_matches = 0;
    uint32_t seen_actions = 0;
    bool seen_ipv4 = false;
    bool seen_l4 = false;
    uint8_t last_protocol_rank = 0;

    if (rule == NULL)
        return rule_error(error, error_len, "rule is null");
    if (rule->domain < DPPD_RULE_DOMAIN_INGRESS ||
        rule->domain > DPPD_RULE_DOMAIN_TRANSFER)
        return rule_error(error, error_len, "invalid rule domain");
    if (rule->fallback < DPPD_FALLBACK_REQUIRE_HARDWARE ||
        rule->fallback > DPPD_FALLBACK_SOFTWARE_ONLY)
        return rule_error(error, error_len, "invalid fallback policy");
    if (rule->nb_matches == 0 || rule->nb_matches > DPPD_RULE_MAX_ITEMS)
        return rule_error(error, error_len, "invalid match count");
    if (rule->nb_actions == 0 || rule->nb_actions > DPPD_RULE_MAX_ACTIONS)
        return rule_error(error, error_len, "invalid action count");

    for (i = 0; i < rule->nb_matches; ++i) {
        const enum dppd_match_type type = rule->matches[i].type;
        if (type < DPPD_MATCH_ETH || type > DPPD_MATCH_REPRESENTED_PORT)
            return rule_error(error, error_len, "unsupported match type");
        if ((seen_matches & (1U << type)) != 0)
            return rule_error(error, error_len, "duplicate match type");
        seen_matches |= 1U << type;
        if (type != DPPD_MATCH_REPRESENTED_PORT) {
            const uint8_t rank = type == DPPD_MATCH_ETH ? 1U :
                                 type == DPPD_MATCH_IPV4 ? 2U : 3U;
            if (rank < last_protocol_rank)
                return rule_error(error, error_len, "protocol matches are out of order");
            last_protocol_rank = rank;
        }
        if (type == DPPD_MATCH_REPRESENTED_PORT &&
            rule->domain != DPPD_RULE_DOMAIN_TRANSFER)
            return rule_error(error, error_len,
                              "represented-port matches require transfer domain");
        if (type == DPPD_MATCH_IPV4) {
            if (seen_l4)
                return rule_error(error, error_len, "IPv4 must precede L4 matches");
            seen_ipv4 = true;
        }
        if (type == DPPD_MATCH_UDP || type == DPPD_MATCH_TCP) {
            if (!seen_ipv4)
                return rule_error(error, error_len, "UDP/TCP requires a preceding IPv4 match");
            if (seen_l4)
                return rule_error(error, error_len, "only one L4 match is allowed");
            seen_l4 = true;
        }
    }

    for (i = 0; i < rule->nb_actions; ++i) {
        const enum dppd_action_type type = rule->actions[i].type;
        if (type < DPPD_ACTION_DROP || type > DPPD_ACTION_REPRESENTED_PORT)
            return rule_error(error, error_len, "unsupported action type");
        if (type != DPPD_ACTION_DROP && (seen_actions & (1U << type)) != 0)
            return rule_error(error, error_len, "duplicate action type");
        seen_actions |= 1U << type;
        if (type == DPPD_ACTION_DROP || type == DPPD_ACTION_QUEUE ||
            type == DPPD_ACTION_REPRESENTED_PORT) {
            fate_actions++;
            if (i + 1U != rule->nb_actions)
                return rule_error(error, error_len, "fate action must be last");
        }
        if (type == DPPD_ACTION_COUNT)
            count_actions++;
        if (type == DPPD_ACTION_REPRESENTED_PORT &&
            rule->domain != DPPD_RULE_DOMAIN_TRANSFER)
            return rule_error(error, error_len,
                              "represented-port actions require transfer domain");
        if (type == DPPD_ACTION_QUEUE && rule->domain != DPPD_RULE_DOMAIN_INGRESS)
            return rule_error(error, error_len, "queue action requires ingress domain");
    }

    if (fate_actions != 1)
        return rule_error(error, error_len, "rule must contain exactly one fate action");
    if (count_actions > 1)
        return rule_error(error, error_len, "only one count action is supported");
    if (error != NULL && error_len != 0)
        error[0] = '\0';
    return 0;
}
