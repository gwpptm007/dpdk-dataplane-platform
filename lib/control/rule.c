#include "dppd/rule.h"

#include <stdbool.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

static int rule_error(char *error, uint32_t error_len, const char *message)
{
    if (error != NULL && error_len != 0)
        snprintf(error, error_len, "%s", message);
    return -1;
}

static bool match_equal(const struct dppd_match *left, const struct dppd_match *right)
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
    case DPPD_MATCH_IPV6:
        return memcmp(&left->spec.ipv6, &right->spec.ipv6, sizeof(left->spec.ipv6)) == 0;
    case DPPD_MATCH_UDP:
    case DPPD_MATCH_TCP:
        return left->spec.l4.src_be == right->spec.l4.src_be &&
            left->spec.l4.src_mask_be == right->spec.l4.src_mask_be && left->spec.l4.dst_be == right->spec.l4.dst_be &&
            left->spec.l4.dst_mask_be == right->spec.l4.dst_mask_be;
    case DPPD_MATCH_REPRESENTED_PORT:
        return left->spec.ethdev_port_id == right->spec.ethdev_port_id;
    case DPPD_MATCH_UDP_RANGE:
    case DPPD_MATCH_TCP_RANGE:
        return left->spec.range.src_first_be == right->spec.range.src_first_be &&
            left->spec.range.src_last_be == right->spec.range.src_last_be &&
            left->spec.range.dst_first_be == right->spec.range.dst_first_be &&
            left->spec.range.dst_last_be == right->spec.range.dst_last_be;
    case DPPD_MATCH_IP_PROTOCOL:
        return left->spec.protocol.value == right->spec.protocol.value &&
            left->spec.protocol.mask == right->spec.protocol.mask;
    case DPPD_MATCH_CONNECTION_STATE:
        return left->spec.connection_states == right->spec.connection_states;
    }
    return false;
}

static bool action_equal(const struct dppd_action *left, const struct dppd_action *right)
{
    if (left->type != right->type)
        return false;
    switch (left->type) {
    case DPPD_ACTION_DROP:
    case DPPD_ACTION_COUNT:
    case DPPD_ACTION_ALLOW:
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

    if (left == NULL || right == NULL || left->id != right->id || left->install_port_id != right->install_port_id ||
        left->domain != right->domain || left->fallback != right->fallback || left->group != right->group ||
        left->priority != right->priority || left->nb_matches != right->nb_matches ||
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
    bool seen_ipv6 = false;
    bool seen_l4 = false;
    uint8_t last_protocol_rank = 0;

    if (rule == NULL)
        return rule_error(error, error_len, "rule is null");
    if (rule->domain < DPPD_RULE_DOMAIN_INGRESS || rule->domain > DPPD_RULE_DOMAIN_TRANSFER)
        return rule_error(error, error_len, "invalid rule domain");
    if (rule->fallback < DPPD_FALLBACK_REQUIRE_HARDWARE || rule->fallback > DPPD_FALLBACK_SOFTWARE_ONLY)
        return rule_error(error, error_len, "invalid fallback policy");
    if (rule->nb_matches == 0 || rule->nb_matches > DPPD_RULE_MAX_ITEMS)
        return rule_error(error, error_len, "invalid match count");
    if (rule->nb_actions == 0 || rule->nb_actions > DPPD_RULE_MAX_ACTIONS)
        return rule_error(error, error_len, "invalid action count");

    for (i = 0; i < rule->nb_matches; ++i) {
        const enum dppd_match_type type = rule->matches[i].type;
        if (type < DPPD_MATCH_ETH || type > DPPD_MATCH_IPV6)
            return rule_error(error, error_len, "unsupported match type");
        if ((seen_matches & (1U << type)) != 0)
            return rule_error(error, error_len, "duplicate match type");
        seen_matches |= 1U << type;
        if (type == DPPD_MATCH_CONNECTION_STATE &&
            (rule->domain != DPPD_RULE_DOMAIN_INGRESS || rule->matches[i].spec.connection_states == 0 ||
                (rule->matches[i].spec.connection_states & ~15U) != 0))
            return rule_error(error, error_len, "invalid ingress connection state mask");
        if (type != DPPD_MATCH_REPRESENTED_PORT && type != DPPD_MATCH_CONNECTION_STATE) {
            const uint8_t rank = type == DPPD_MATCH_ETH              ? 1U
                : type == DPPD_MATCH_IPV4 || type == DPPD_MATCH_IPV6 ? 2U
                : type == DPPD_MATCH_IP_PROTOCOL                     ? 3U
                                                                     : 4U;
            if (rank < last_protocol_rank)
                return rule_error(error, error_len, "protocol matches are out of order");
            last_protocol_rank = rank;
        }
        if (type == DPPD_MATCH_REPRESENTED_PORT && rule->domain != DPPD_RULE_DOMAIN_TRANSFER)
            return rule_error(error, error_len, "represented-port matches require transfer domain");
        if (type == DPPD_MATCH_IPV4 || type == DPPD_MATCH_IPV6) {
            if (seen_l4)
                return rule_error(error, error_len, "IP must precede L4 matches");
            if (seen_ipv4 || seen_ipv6)
                return rule_error(error, error_len, "only one IP address family is allowed");
            seen_ipv4 = type == DPPD_MATCH_IPV4;
            seen_ipv6 = type == DPPD_MATCH_IPV6;
            if (seen_ipv6 && rule->domain != DPPD_RULE_DOMAIN_INGRESS)
                return rule_error(error, error_len, "IPv6 ACL requires ingress domain");
        }
        if (type == DPPD_MATCH_IP_PROTOCOL && !seen_ipv4 && !seen_ipv6)
            return rule_error(error, error_len, "IP protocol requires a preceding IP match");
        if (type == DPPD_MATCH_UDP_RANGE || type == DPPD_MATCH_TCP_RANGE) {
            const struct dppd_match_l4_range *range = &rule->matches[i].spec.range;
            if (ntohs(range->src_first_be) > ntohs(range->src_last_be) ||
                ntohs(range->dst_first_be) > ntohs(range->dst_last_be))
                return rule_error(error, error_len, "port range lower bound exceeds upper bound");
        }
        if (type == DPPD_MATCH_UDP || type == DPPD_MATCH_TCP || type == DPPD_MATCH_UDP_RANGE ||
            type == DPPD_MATCH_TCP_RANGE) {
            if (!seen_ipv4 && !seen_ipv6)
                return rule_error(error, error_len, "UDP/TCP requires a preceding IP match");
            if (seen_l4)
                return rule_error(error, error_len, "only one L4 match is allowed");
            seen_l4 = true;
        }
    }

    /** 当前会话表只处理 IPv4，IPv6 不能声明实际上不会产生的 NEW 或 ESTABLISHED 状态 */
    if (seen_ipv6)
        for (i = 0; i < rule->nb_matches; ++i)
            if (rule->matches[i].type == DPPD_MATCH_CONNECTION_STATE &&
                (rule->matches[i].spec.connection_states & ~1U) != 0)
                return rule_error(error, error_len, "IPv6 supports only untracked state");

    for (i = 0; i < rule->nb_actions; ++i) {
        const enum dppd_action_type type = rule->actions[i].type;
        if (type < DPPD_ACTION_DROP || type > DPPD_ACTION_ALLOW)
            return rule_error(error, error_len, "unsupported action type");
        if (type != DPPD_ACTION_DROP && (seen_actions & (1U << type)) != 0)
            return rule_error(error, error_len, "duplicate action type");
        seen_actions |= 1U << type;
        if (type == DPPD_ACTION_ALLOW && rule->domain != DPPD_RULE_DOMAIN_INGRESS)
            return rule_error(error, error_len, "allow action requires ingress domain");
        if (type == DPPD_ACTION_DROP || type == DPPD_ACTION_QUEUE || type == DPPD_ACTION_REPRESENTED_PORT ||
            type == DPPD_ACTION_ALLOW) {
            fate_actions++;
            if (i + 1U != rule->nb_actions)
                return rule_error(error, error_len, "fate action must be last");
        }
        if (type == DPPD_ACTION_COUNT)
            count_actions++;
        if (type == DPPD_ACTION_REPRESENTED_PORT && rule->domain != DPPD_RULE_DOMAIN_TRANSFER)
            return rule_error(error, error_len, "represented-port actions require transfer domain");
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
