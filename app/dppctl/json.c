#include "json.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const char *boolean(bool value)
{
    return value ? "true" : "false";
}

/** 字段名称只来自源码常量，地址只来自 inet_ntop，不把任意终端文本拼进 JSON */
static void address(const char *field, int family, const void *bytes)
{
    char text[INET6_ADDRSTRLEN] = {0};
    (void)inet_ntop(family, bytes, text, sizeof(text));
    printf(",\"%s\":\"%s\"", field, text);
}

static void rule(const struct dppd_rule *value)
{
    static const char *domains[] = {"ingress", "egress", "transfer"};
    static const char *fallbacks[] = {"require", "prefer", "software"};
    static const char *matches[] = {"eth", "ipv4", "udp", "tcp", "represented_port", "udp_range", "tcp_range",
        "ip_protocol", "connection_state", "ipv6"};
    static const char *actions[] = {"drop", "queue", "mark", "count", "represented_port", "allow", "age",
        "shared_count"};
    printf("{\"id\":\"%" PRIu64 "\",\"generation\":\"%" PRIu64
           "\",\"port\":%u,\"domain\":\"%s\",\"fallback\":\"%s\",\"group\":%u,\"priority\":%u,\"matches\":[",
        value->id, value->generation, value->install_port_id,
        (unsigned int)value->domain < 3 ? domains[value->domain] : "unknown",
        (unsigned int)value->fallback < 3 ? fallbacks[value->fallback] : "unknown", value->group, value->priority);
    for (uint16_t i = 0; i < value->nb_matches && i < DPPD_RULE_MAX_ITEMS; ++i) {
        const struct dppd_match *item = &value->matches[i];
        printf("%s{\"type\":\"%s\"", i == 0 ? "" : ",",
            (unsigned int)item->type < 10 ? matches[item->type] : "unknown");
        switch (item->type) {
        case DPPD_MATCH_IPV4:
            address("source", AF_INET, &item->spec.ipv4.src_be);
            address("source_mask", AF_INET, &item->spec.ipv4.src_mask_be);
            address("destination", AF_INET, &item->spec.ipv4.dst_be);
            address("destination_mask", AF_INET, &item->spec.ipv4.dst_mask_be);
            break;
        case DPPD_MATCH_IPV6:
            address("source", AF_INET6, item->spec.ipv6.src);
            address("source_mask", AF_INET6, item->spec.ipv6.src_mask);
            address("destination", AF_INET6, item->spec.ipv6.dst);
            address("destination_mask", AF_INET6, item->spec.ipv6.dst_mask);
            break;
        case DPPD_MATCH_UDP:
        case DPPD_MATCH_TCP:
            printf(",\"source\":%u,\"source_mask\":%u,\"destination\":%u,\"destination_mask\":%u",
                ntohs(item->spec.l4.src_be), ntohs(item->spec.l4.src_mask_be), ntohs(item->spec.l4.dst_be),
                ntohs(item->spec.l4.dst_mask_be));
            break;
        case DPPD_MATCH_UDP_RANGE:
        case DPPD_MATCH_TCP_RANGE:
            printf(",\"source_first\":%u,\"source_last\":%u,\"destination_first\":%u,\"destination_last\":%u",
                ntohs(item->spec.range.src_first_be), ntohs(item->spec.range.src_last_be),
                ntohs(item->spec.range.dst_first_be), ntohs(item->spec.range.dst_last_be));
            break;
        case DPPD_MATCH_REPRESENTED_PORT:
            printf(",\"port\":%u", item->spec.ethdev_port_id);
            break;
        case DPPD_MATCH_IP_PROTOCOL:
            printf(",\"value\":%u,\"mask\":%u", item->spec.protocol.value, item->spec.protocol.mask);
            break;
        case DPPD_MATCH_CONNECTION_STATE:
            printf(",\"mask\":%u", item->spec.connection_states);
            break;
        default:
            break;
        }
        putchar('}');
    }
    printf("],\"actions\":[");
    for (uint16_t i = 0; i < value->nb_actions && i < DPPD_RULE_MAX_ACTIONS; ++i) {
        const struct dppd_action *action = &value->actions[i];
        printf("%s{\"type\":\"%s\"", i == 0 ? "" : ",",
            (unsigned int)action->type < 8 ? actions[action->type] : "unknown");
        switch (action->type) {
        case DPPD_ACTION_QUEUE:
            printf(",\"queue\":%u", action->conf.queue_id);
            break;
        case DPPD_ACTION_MARK:
            printf(",\"mark\":%u", action->conf.mark_id);
            break;
        case DPPD_ACTION_REPRESENTED_PORT:
            printf(",\"port\":%u", action->conf.ethdev_port_id);
            break;
        case DPPD_ACTION_AGE:
            printf(",\"seconds\":%u", action->conf.age_timeout_seconds);
            break;
        case DPPD_ACTION_SHARED_COUNT:
            printf(",\"id\":\"%" PRIu64 "\",\"generation\":\"%" PRIu64 "\"", action->conf.shared_count.id,
                action->conf.shared_count.generation);
            break;
        default:
            break;
        }
        putchar('}');
    }
    printf("]}");
}

static void shared(const struct dppd_shared_count_spec *spec)
{
    printf("\"id\":\"%" PRIu64 "\",\"generation\":\"%" PRIu64 "\",\"port\":%u,\"domain\":%u", spec->id,
        spec->generation, spec->install_port_id, (unsigned int)spec->domain);
}

/** 路由查询完整输出四张表，版本单独标明，调用方不能把缺少的另一地址族当作空表 */
static void routing(const struct dppd_ipv4_configuration *value)
{
    char prefix[INET6_ADDRSTRLEN];
    const uint8_t zero[16] = {0};
    printf("\"generation\":\"%" PRIu64 "\",\"routes\":[", value->generation);
    for (uint32_t i = 0; i < value->route_count; ++i) {
        const struct dppd_ipv4_route *entry = &value->routes[i];
        (void)inet_ntop(AF_INET, &entry->prefix_be, prefix, sizeof(prefix));
        printf("%s{\"prefix\":\"%s/%u\",\"port\":%u", i == 0 ? "" : ",", prefix, entry->depth, entry->port_id);
        if (entry->gateway_be == 0)
            printf(",\"next_hop\":\"direct\"");
        else
            address("next_hop", AF_INET, &entry->gateway_be);
        putchar('}');
    }
    printf("],\"neighbors\":[");
    for (uint32_t i = 0; i < value->neighbor_count; ++i) {
        const struct dppd_ipv4_neighbor *entry = &value->neighbors[i];
        printf("%s{\"port\":%u", i == 0 ? "" : ",", entry->port_id);
        address("address", AF_INET, &entry->address_be);
        printf(",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}", entry->mac[0], entry->mac[1], entry->mac[2],
            entry->mac[3], entry->mac[4], entry->mac[5]);
    }
    printf("],\"routes6\":[");
    for (uint32_t i = 0; i < value->route6_count; ++i) {
        const struct dppd_ipv6_route *entry = &value->routes6[i];
        (void)inet_ntop(AF_INET6, entry->prefix, prefix, sizeof(prefix));
        printf("%s{\"prefix\":\"%s/%u\",\"port\":%u", i == 0 ? "" : ",", prefix, entry->depth, entry->port_id);
        if (memcmp(entry->gateway, zero, sizeof(zero)) == 0)
            printf(",\"next_hop\":\"direct\"");
        else
            address("next_hop", AF_INET6, entry->gateway);
        putchar('}');
    }
    printf("],\"neighbors6\":[");
    for (uint32_t i = 0; i < value->neighbor6_count; ++i) {
        const struct dppd_ipv6_neighbor *entry = &value->neighbors6[i];
        printf("%s{\"port\":%u", i == 0 ? "" : ",", entry->port_id);
        address("address", AF_INET6, entry->address);
        printf(",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}", entry->mac[0], entry->mac[1], entry->mac[2],
            entry->mac[3], entry->mac[4], entry->mac[5]);
    }
    putchar(']');
}

/** 策略中的空地址池用 null 表示关闭，原样回传后可以作为下一次整策略替换的输入 */
static void snat_json(const struct dppd_snat_config *value)
{
    if (!value->enabled) {
        printf("null");
        return;
    }
    printf("{\"inside_port\":%u,\"outside_port\":%u,\"first_port\":%u,\"last_port\":%u", value->inside_port,
        value->outside_port, value->first_port, value->last_port);
    address("address", value->ipv6 ? AF_INET6 : AF_INET,
        value->ipv6 ? (const void *)value->address6 : (const void *)&value->address_be);
    putchar('}');
}

static void dnat_json(const struct dppd_dnat_rule *value)
{
    printf("{\"family\":\"%s\",\"protocol\":\"%s\",\"inside_port\":%u,\"outside_port\":%u,"
           "\"public_first\":%u,\"public_last\":%u,\"private_first\":%u",
        value->ipv6 ? "ipv6" : "ipv4", value->protocol == 6 ? "tcp" : "udp", value->inside_port, value->outside_port,
        value->public_first, value->public_last, value->private_first);
    address("public_address", value->ipv6 ? AF_INET6 : AF_INET,
        value->ipv6 ? (const void *)value->public_address6 : (const void *)&value->public_address_be);
    address("private_address", value->ipv6 ? AF_INET6 : AF_INET,
        value->ipv6 ? (const void *)value->private_address6 : (const void *)&value->private_address_be);
    putchar('}');
}

static void conntrack_policy_json(const struct dppd_conntrack_policy *value)
{
    printf("{\"capacity\":%u,\"timeout_s\":%u,\"snat\":", value->capacity, value->timeout_s);
    snat_json(&value->snat);
    printf(",\"snat6\":");
    snat_json(&value->snat6);
    printf(",\"dnat\":[");
    for (uint32_t i = 0; i < value->dnat_count; ++i) {
        if (i != 0)
            putchar(',');
        dnat_json(&value->dnat[i]);
    }
    printf("]}");
}

static void conntrack_status_json(const struct dppd_conntrack_status *value)
{
    printf("\"enabled\":%s,\"epoch\":\"%" PRIu64 "\",\"revision\":\"%" PRIu64
           "\",\"capacity\":%u,\"timeout_s\":%u,\"active\":%u,\"peak\":%u,\"created\":\"%" PRIu64
           "\",\"expired\":\"%" PRIu64 "\",\"flushed\":\"%" PRIu64 "\",\"invalid\":\"%" PRIu64
           "\",\"table_full\":\"%" PRIu64 "\",\"pool_full\":\"%" PRIu64 "\",\"sessions_persistent\":false,\"snat\":",
        boolean(value->enabled), value->epoch, value->revision, value->capacity, value->timeout_s, value->active,
        value->peak, value->created, value->expired, value->flushed, value->invalid, value->table_full, value->pool_full);
    snat_json(&value->snat);
    printf(",\"snat6\":");
    snat_json(&value->snat6);
    printf(",\"dnat_count\":%u", value->dnat_count);
}

static void tuple_json(const struct dppd_flow_tuple *value)
{
    printf("{\"family\":\"%s\",\"protocol\":%u,\"source_port\":%u,\"destination_port\":%u",
        value->ipv6 ? "ipv6" : "ipv4", value->protocol, ntohs(value->source_port_be), ntohs(value->destination_port_be));
    address("source", value->ipv6 ? AF_INET6 : AF_INET,
        value->ipv6 ? (const void *)value->source6 : (const void *)&value->source_be);
    address("destination", value->ipv6 ? AF_INET6 : AF_INET,
        value->ipv6 ? (const void *)value->destination6 : (const void *)&value->destination_be);
    putchar('}');
}

/** 成员集合由 epoch 和 revision 共同约束，流量计数仍是读取这一页时的即时值 */
static void connections_json(const struct dppd_connection_page *page)
{
    printf("\"epoch\":\"%" PRIu64 "\",\"revision\":\"%" PRIu64 "\",\"next_id\":\"%" PRIu64
           "\",\"count\":%u,\"more\":%s,\"entries\":[", page->epoch, page->revision, page->next_id, page->count,
        boolean(page->more));
    static const char *states[] = {"none", "syn", "syn-ack", "open", "fin", "closed"};
    for (uint32_t i = 0; i < page->count; ++i) {
        const struct dppd_connection_view *entry = &page->entries[i];
        printf("%s{\"id\":\"%" PRIu64 "\",\"ingress\":%u,\"egress\":%u,\"nat\":%s,\"seen_reply\":%s,"
               "\"dnat_rule_id\":%u,\"tcp_state\":\"%s\",\"last_seen_ms\":\"%" PRIu64
               "\",\"expires_ms\":\"%" PRIu64 "\",\"packets\":[\"%" PRIu64 "\",\"%" PRIu64
               "\"],\"bytes\":[\"%" PRIu64 "\",\"%" PRIu64 "\"],\"original\":",
            i == 0 ? "" : ",", entry->id, entry->ingress, entry->egress, boolean(entry->nat), boolean(entry->seen_reply),
            entry->dnat_rule_id, entry->tcp_state <= DPPD_TCP_CLOSED ? states[entry->tcp_state] : "unknown",
            entry->last_seen_ms, entry->expires_ms, entry->packets[0], entry->packets[1], entry->bytes[0], entry->bytes[1]);
        tuple_json(&entry->original);
        printf(",\"translated\":");
        tuple_json(&entry->translated);
        putchar('}');
    }
    putchar(']');
}

static void nd6_local_json(const struct dppd_nd6_local *local)
{
    printf("{\"port_id\":%u", local->port_id);
    address("address", AF_INET6, local->address);
    printf(",\"vlans\":[");
    for (uint8_t i = 0; i < local->vlan_depth; ++i)
        printf("%s%u", i == 0 ? "" : ",", local->vlan[i]);
    printf("]}");
}

/** 本机地址配置可直接回传给整策略接口，动态缓存和 DAD 结果单独查询 */
static void nd6_policy_json(const struct dppd_nd6_policy *value)
{
    printf("{\"reachable_ms\":%u,\"locals\":[", value->reachable_ms);
    for (uint32_t i = 0; i < value->local_count; ++i) {
        if (i != 0)
            putchar(',');
        nd6_local_json(&value->locals[i]);
    }
    printf("]}");
}

static void nd6_status_json(const struct dppd_nd6_status *value)
{
    printf("\"enabled\":%s,\"epoch\":\"%" PRIu64 "\",\"revision\":\"%" PRIu64
           "\",\"active\":%u,\"pending\":%u,\"local_count\":%u,\"reachable_ms\":%u,\"solicitations\":\"%" PRIu64
           "\",\"advertisements\":\"%" PRIu64 "\",\"invalid\":\"%" PRIu64 "\",\"tx_errors\":\"%" PRIu64
           "\",\"queue_drops\":\"%" PRIu64 "\",\"resolved\":\"%" PRIu64 "\",\"failed\":\"%" PRIu64
           "\",\"neighbors_persistent\":false,\"locals\":[",
        boolean(value->enabled), value->epoch, value->revision, value->active, value->pending, value->local_count,
        value->reachable_ms, value->solicitations, value->advertisements, value->invalid, value->tx_errors,
        value->queue_drops, value->resolved, value->failed);
    static const char *states[] = {"tentative", "preferred", "duplicate"};
    for (uint32_t i = 0; i < value->local_count; ++i) {
        printf("%s{\"state\":\"%s\",\"configuration\":", i == 0 ? "" : ",",
            value->locals[i].state <= DPPD_ND6_DUPLICATE ? states[value->locals[i].state] : "unknown");
        nd6_local_json(&value->locals[i].configuration);
        putchar('}');
    }
    putchar(']');
}

static void nd6_neighbors_json(const struct dppd_nd6_page *page)
{
    printf("\"epoch\":\"%" PRIu64 "\",\"revision\":\"%" PRIu64 "\",\"next_id\":\"%" PRIu64
           "\",\"count\":%u,\"more\":%s,\"entries\":[", page->epoch, page->revision, page->next_id, page->count,
        boolean(page->more));
    static const char *states[] = {"none", "incomplete", "reachable", "stale", "delay", "probe", "failed"};
    for (uint32_t i = 0; i < page->count; ++i) {
        const struct dppd_nd6_entry *entry = &page->entries[i];
        printf("%s{\"id\":\"%" PRIu64 "\",\"port_id\":%u", i == 0 ? "" : ",", entry->id, entry->port_id);
        address("address", AF_INET6, entry->address);
        printf(",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"state\":\"%s\",\"probes\":%u,"
               "\"changed_ms\":\"%" PRIu64 "\",\"due_ms\":\"%" PRIu64 "\",\"used_ms\":\"%" PRIu64 "\"}",
            entry->mac[0], entry->mac[1], entry->mac[2], entry->mac[3], entry->mac[4], entry->mac[5],
            entry->state <= DPPD_ND6_FAILED ? states[entry->state] : "unknown", entry->probes,
            entry->changed_ms, entry->due_ms, entry->used_ms);
    }
    putchar(']');
}

static void arp_local_json(const struct dppd_arp_local *local)
{
    printf("{\"port_id\":%u", local->port_id);
    address("address", AF_INET, local->address);
    printf(",\"vlans\":[");
    for (uint8_t i = 0; i < local->vlan_depth; ++i)
        printf("%s%u", i == 0 ? "" : ",", local->vlan[i]);
    printf("]}");
}

/** 本机地址配置可直接回传给整策略接口，动态缓存和 ACD 结果单独查询 */
static void arp_policy_json(const struct dppd_arp_policy *value)
{
    printf("{\"reachable_ms\":%u,\"locals\":[", value->reachable_ms);
    for (uint32_t i = 0; i < value->local_count; ++i) {
        if (i != 0)
            putchar(',');
        arp_local_json(&value->locals[i]);
    }
    printf("]}");
}

static void arp_status_json(const struct dppd_arp_status *value)
{
    printf("\"enabled\":%s,\"epoch\":\"%" PRIu64 "\",\"revision\":\"%" PRIu64
           "\",\"active\":%u,\"pending\":%u,\"local_count\":%u,\"reachable_ms\":%u,\"requests\":\"%" PRIu64
           "\",\"replies\":\"%" PRIu64 "\",\"invalid\":\"%" PRIu64 "\",\"tx_errors\":\"%" PRIu64
           "\",\"queue_drops\":\"%" PRIu64 "\",\"resolved\":\"%" PRIu64 "\",\"failed\":\"%" PRIu64
           "\",\"neighbors_persistent\":false,\"locals\":[",
        boolean(value->enabled), value->epoch, value->revision, value->active, value->pending, value->local_count,
        value->reachable_ms, value->requests, value->replies, value->invalid, value->tx_errors,
        value->queue_drops, value->resolved, value->failed);
    static const char *states[] = {"tentative", "preferred", "duplicate"};
    for (uint32_t i = 0; i < value->local_count; ++i) {
        printf("%s{\"state\":\"%s\",\"configuration\":", i == 0 ? "" : ",",
            value->locals[i].state <= DPPD_ARP_DUPLICATE ? states[value->locals[i].state] : "unknown");
        arp_local_json(&value->locals[i].configuration);
        putchar('}');
    }
    putchar(']');
}

static void arp_neighbors_json(const struct dppd_arp_page *page)
{
    printf("\"epoch\":\"%" PRIu64 "\",\"revision\":\"%" PRIu64 "\",\"next_id\":\"%" PRIu64
           "\",\"count\":%u,\"more\":%s,\"entries\":[", page->epoch, page->revision, page->next_id, page->count,
        boolean(page->more));
    static const char *states[] = {"none", "incomplete", "reachable", "stale", "delay", "probe", "failed"};
    for (uint32_t i = 0; i < page->count; ++i) {
        const struct dppd_arp_entry *entry = &page->entries[i];
        printf("%s{\"id\":\"%" PRIu64 "\",\"port_id\":%u", i == 0 ? "" : ",", entry->id, entry->port_id);
        address("address", AF_INET, entry->address);
        printf(",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"state\":\"%s\",\"probes\":%u,"
               "\"changed_ms\":\"%" PRIu64 "\",\"due_ms\":\"%" PRIu64 "\",\"used_ms\":\"%" PRIu64 "\"}",
            entry->mac[0], entry->mac[1], entry->mac[2], entry->mac[3], entry->mac[4], entry->mac[5],
            entry->state <= DPPD_ARP_FAILED ? states[entry->state] : "unknown", entry->probes,
            entry->changed_ms, entry->due_ms, entry->used_ms);
    }
    putchar(']');
}

bool dppctl_json_supported(enum dppd_management_operation operation)
{
    switch (operation) {
    case DPPD_MANAGEMENT_REQUEST_SUBMIT:
    case DPPD_MANAGEMENT_REQUEST_GET:
    case DPPD_MANAGEMENT_PING:
    case DPPD_MANAGEMENT_HEALTH_GET:
    case DPPD_MANAGEMENT_STATS_QUERY:
    case DPPD_MANAGEMENT_FLOW_STATUS:
    case DPPD_MANAGEMENT_FLOW_RETRY:
    case DPPD_MANAGEMENT_PERSISTENCE_STATUS:
    case DPPD_MANAGEMENT_PERSISTENCE_FLUSH:
    case DPPD_MANAGEMENT_RULE_GET:
    case DPPD_MANAGEMENT_RULE_LIST:
    case DPPD_MANAGEMENT_RULE_COUNT_QUERY:
    case DPPD_MANAGEMENT_SHARED_COUNT_GET:
    case DPPD_MANAGEMENT_SHARED_COUNT_QUERY:
    case DPPD_MANAGEMENT_FLOW_CONFIG_GET:
    case DPPD_MANAGEMENT_FLOW_CONFIG_APPLY:
    case DPPD_MANAGEMENT_ROUTING_GET:
    case DPPD_MANAGEMENT_ROUTING_STATUS:
    case DPPD_MANAGEMENT_CONNTRACK_POLICY_GET:
    case DPPD_MANAGEMENT_CONNTRACK_STATUS:
    case DPPD_MANAGEMENT_CONNTRACK_LIST:
    case DPPD_MANAGEMENT_CONNTRACK_FLUSH:
    case DPPD_MANAGEMENT_NAT_STATUS:
    case DPPD_MANAGEMENT_DNAT_LIST:
    case DPPD_MANAGEMENT_ND6_POLICY_GET:
    case DPPD_MANAGEMENT_ND6_STATUS:
    case DPPD_MANAGEMENT_ND6_LIST:
    case DPPD_MANAGEMENT_ND6_FLUSH:
    case DPPD_MANAGEMENT_ARP_POLICY_GET:
    case DPPD_MANAGEMENT_ARP_STATUS:
    case DPPD_MANAGEMENT_ARP_LIST:
    case DPPD_MANAGEMENT_ARP_FLUSH:
        return true;
    default:
        return false;
    }
}

static bool rule_bounds(const struct dppd_rule *value)
{
    return value->nb_matches <= DPPD_RULE_MAX_ITEMS && value->nb_actions <= DPPD_RULE_MAX_ACTIONS;
}

/** 长度错误必须整体拒绝，不能把截断后的列表当作完整的成功响应 */
static bool response_bounds(const struct dppd_management_response *value)
{
    if (value->operation == DPPD_MANAGEMENT_REQUEST_SUBMIT || value->operation == DPPD_MANAGEMENT_REQUEST_GET) {
        const struct dppd_flow_request_result *result = &value->payload.durable_result;
        return result->operation == DPPD_FLOW_REQUEST_ROUTING_REPLACE ||
                result->operation == DPPD_FLOW_REQUEST_CONNTRACK_REPLACE || result->operation == DPPD_FLOW_REQUEST_ND6_REPLACE || result->operation == DPPD_FLOW_REQUEST_ARP_REPLACE
            ? result->count == 1 : result->count <= 8;
    }
    if (value->operation == DPPD_MANAGEMENT_ND6_POLICY_GET) {
        const struct dppd_nd6_policy *policy = &value->payload.nd6_policy.policy;
        if (policy->local_count > DPPD_ND6_LOCAL_LIMIT)
            return false;
        for (uint32_t i = 0; i < policy->local_count; ++i)
            if (policy->locals[i].vlan_depth > 2)
                return false;
    }
    if (value->operation == DPPD_MANAGEMENT_ND6_STATUS || value->operation == DPPD_MANAGEMENT_ND6_FLUSH) {
        const struct dppd_nd6_status *status = &value->payload.nd6;
        if (status->local_count > DPPD_ND6_LOCAL_LIMIT)
            return false;
        for (uint32_t i = 0; i < status->local_count; ++i)
            if (status->locals[i].configuration.vlan_depth > 2)
                return false;
    }
    if (value->operation == DPPD_MANAGEMENT_ND6_LIST)
        return value->payload.dynamic_neighbors.count <= DPPD_ND6_PAGE_SIZE;
    if (value->operation == DPPD_MANAGEMENT_ARP_POLICY_GET) {
        const struct dppd_arp_policy *policy = &value->payload.arp_policy.policy;
        if (policy->local_count > DPPD_ARP_LOCAL_LIMIT)
            return false;
        for (uint32_t i = 0; i < policy->local_count; ++i)
            if (policy->locals[i].vlan_depth > 2)
                return false;
    }
    if (value->operation == DPPD_MANAGEMENT_ARP_STATUS || value->operation == DPPD_MANAGEMENT_ARP_FLUSH) {
        const struct dppd_arp_status *status = &value->payload.arp;
        if (status->local_count > DPPD_ARP_LOCAL_LIMIT)
            return false;
        for (uint32_t i = 0; i < status->local_count; ++i)
            if (status->locals[i].configuration.vlan_depth > 2)
                return false;
    }
    if (value->operation == DPPD_MANAGEMENT_ARP_LIST)
        return value->payload.arp_neighbors.count <= DPPD_ARP_PAGE_SIZE;
    if (value->operation == DPPD_MANAGEMENT_CONNTRACK_POLICY_GET)
        return value->payload.conntrack_policy.policy.dnat_count <= DPPD_MAX_DNAT_RULES;
    if (value->operation == DPPD_MANAGEMENT_CONNTRACK_LIST)
        return value->payload.connections.count <= DPPD_CT_PAGE_SIZE;
    if (value->operation == DPPD_MANAGEMENT_DNAT_LIST)
        return value->payload.dnat.count <= DPPD_CT_PAGE_SIZE;
    if (value->operation == DPPD_MANAGEMENT_ROUTING_GET) {
        const struct dppd_ipv4_configuration *configuration = &value->payload.flow_routing.configuration;
        return configuration->route_count <= DPPD_IPV4_ROUTE_LIMIT &&
            configuration->route6_count <= DPPD_IPV4_ROUTE_LIMIT &&
            configuration->neighbor_count <= DPPD_IPV4_NEIGHBOR_LIMIT &&
            configuration->neighbor6_count <= DPPD_IPV4_NEIGHBOR_LIMIT;
    }
    if (value->operation == DPPD_MANAGEMENT_RULE_GET)
        return rule_bounds(&value->payload.rule);
    if (value->operation == DPPD_MANAGEMENT_RULE_LIST)
        return value->payload.rule_page.count <= DPPD_MANAGEMENT_RULE_PAGE_SIZE;
    if (value->operation == DPPD_MANAGEMENT_FLOW_CONFIG_GET) {
        const struct dppd_flow_config_page *page = &value->payload.flow_configuration;
        if (page->tables_returned > 2 || page->counts_returned > 4)
            return false;
        for (uint32_t i = 0; i < page->tables_returned; ++i)
            if (!rule_bounds(&page->tables[i].spec.example))
                return false;
    }
    return true;
}

/** 配置与持久请求即使失败也可能已经应用，明确保存这些标志帮助调用方选择恢复操作 */
int dppctl_json_response(const struct dppd_management_request *request, const struct dppd_management_response *response)
{
    if (request == NULL || response == NULL)
        return -EINVAL;
    if (!dppctl_json_supported(response->operation) || !response_bounds(response))
        return -EPROTO;
    printf("{\"api_version\":\"v1\",\"status\":%d,\"data\":{", response->status);
    bool durable_result =
        (response->operation == DPPD_MANAGEMENT_REQUEST_SUBMIT || response->operation == DPPD_MANAGEMENT_REQUEST_GET) &&
        response->payload.durable_result.key.client != 0;
    if (response->status != 0 && response->operation != DPPD_MANAGEMENT_FLOW_CONFIG_APPLY && !durable_result)
        goto finish;
    switch (response->operation) {
    case DPPD_MANAGEMENT_REQUEST_SUBMIT:
    case DPPD_MANAGEMENT_REQUEST_GET: {
        const struct dppd_flow_request_result *value = &response->payload.durable_result;
        static const char *operations[] = {"unknown", "create", "update", "remove", "configure", "routing_replace",
            "conntrack_replace", "nd6_replace", "arp_replace"};
        printf("\"client\":\"%" PRIu64 "\",\"sequence\":\"%" PRIu64
               "\",\"operation\":\"%s\",\"state\":\"%s\",\"status\":%d,\"applied\":%s,\"durable\":%s,"
               "\"blocked\":%s,\"revision\":\"%" PRIu64 "\",\"rules\":[",
            value->key.client, value->key.sequence,
            (unsigned int)value->operation <= 8 ? operations[value->operation] : "unknown",
            value->phase == DPPD_FLOW_REQUEST_DONE ? "complete" : "pending", value->status, boolean(value->applied),
            boolean(value->durable), boolean(response->status == -EUCLEAN), value->revision);
        for (uint32_t i = 0; value->operation != DPPD_FLOW_REQUEST_ROUTING_REPLACE &&
             value->operation != DPPD_FLOW_REQUEST_CONNTRACK_REPLACE && value->operation != DPPD_FLOW_REQUEST_ND6_REPLACE && value->operation != DPPD_FLOW_REQUEST_ARP_REPLACE &&
             i < value->count && i < 8; ++i)
            printf("%s{\"id\":\"%" PRIu64 "\",\"generation\":\"%" PRIu64 "\"}", i == 0 ? "" : ",", value->ids[i],
                value->generations[i]);
        putchar(']');
        if (value->operation == DPPD_FLOW_REQUEST_ROUTING_REPLACE)
            printf(",\"routing_generation\":\"%" PRIu64 "\"", value->generations[0]);
        else if (value->operation == DPPD_FLOW_REQUEST_CONNTRACK_REPLACE)
            printf(",\"conntrack_generation\":\"%" PRIu64 "\"", value->generations[0]);
        else if (value->operation == DPPD_FLOW_REQUEST_ND6_REPLACE)
            printf(",\"nd6_generation\":\"%" PRIu64 "\"", value->generations[0]);
        else if (value->operation == DPPD_FLOW_REQUEST_ARP_REPLACE)
            printf(",\"arp_generation\":\"%" PRIu64 "\"", value->generations[0]);
        break;
    }
    case DPPD_MANAGEMENT_ND6_POLICY_GET: {
        const struct dppd_flow_nd6_view *value = &response->payload.nd6_policy;
        printf("\"generation\":\"%" PRIu64 "\",\"revision\":\"%" PRIu64 "\",\"durable_revision\":\"%" PRIu64
               "\",\"neighbor_epoch\":\"%" PRIu64 "\",\"neighbor_revision\":\"%" PRIu64 "\",\"policy\":",
            value->generation, value->revision, value->durable_revision, value->neighbor_epoch, value->neighbor_revision);
        nd6_policy_json(&value->policy);
        break;
    }
    case DPPD_MANAGEMENT_ND6_STATUS:
    case DPPD_MANAGEMENT_ND6_FLUSH:
        nd6_status_json(&response->payload.nd6);
        break;
    case DPPD_MANAGEMENT_ND6_LIST:
        nd6_neighbors_json(&response->payload.dynamic_neighbors);
        break;
    case DPPD_MANAGEMENT_ARP_POLICY_GET: {
        const struct dppd_flow_arp_view *value = &response->payload.arp_policy;
        printf("\"generation\":\"%" PRIu64 "\",\"revision\":\"%" PRIu64 "\",\"durable_revision\":\"%" PRIu64
               "\",\"neighbor_epoch\":\"%" PRIu64 "\",\"neighbor_revision\":\"%" PRIu64 "\",\"policy\":",
            value->generation, value->revision, value->durable_revision, value->neighbor_epoch, value->neighbor_revision);
        arp_policy_json(&value->policy);
        break;
    }
    case DPPD_MANAGEMENT_ARP_STATUS:
    case DPPD_MANAGEMENT_ARP_FLUSH:
        arp_status_json(&response->payload.arp);
        break;
    case DPPD_MANAGEMENT_ARP_LIST:
        arp_neighbors_json(&response->payload.arp_neighbors);
        break;
    case DPPD_MANAGEMENT_CONNTRACK_POLICY_GET: {
        const struct dppd_flow_conntrack_view *value = &response->payload.conntrack_policy;
        printf("\"generation\":\"%" PRIu64 "\",\"revision\":\"%" PRIu64 "\",\"durable_revision\":\"%" PRIu64
               "\",\"session_epoch\":\"%" PRIu64 "\",\"session_revision\":\"%" PRIu64 "\",\"policy\":",
            value->generation, value->revision, value->durable_revision, value->session_epoch, value->session_revision);
        conntrack_policy_json(&value->policy);
        break;
    }
    case DPPD_MANAGEMENT_CONNTRACK_STATUS:
    case DPPD_MANAGEMENT_CONNTRACK_FLUSH:
    case DPPD_MANAGEMENT_NAT_STATUS:
        conntrack_status_json(&response->payload.conntrack);
        break;
    case DPPD_MANAGEMENT_CONNTRACK_LIST:
        connections_json(&response->payload.connections);
        break;
    case DPPD_MANAGEMENT_DNAT_LIST: {
        const struct dppd_dnat_page *page = &response->payload.dnat;
        printf("\"next_id\":%u,\"more\":%s,\"entries\":[", page->next_id, boolean(page->more));
        for (uint32_t i = 0; i < page->count; ++i) {
            printf("%s{\"id\":%u,\"rule\":", i == 0 ? "" : ",", page->entries[i].id);
            dnat_json(&page->entries[i].rule);
            putchar('}');
        }
        putchar(']');
        break;
    }
    case DPPD_MANAGEMENT_ROUTING_GET:
        routing(&response->payload.flow_routing.configuration);
        printf(",\"managed\":%s,\"revision\":\"%" PRIu64 "\",\"durable_revision\":\"%" PRIu64 "\"",
            boolean(response->payload.flow_routing.managed), response->payload.flow_routing.revision,
            response->payload.flow_routing.durable_revision);
        break;
    case DPPD_MANAGEMENT_ROUTING_STATUS: {
        const struct dppd_routing_status *value = &response->payload.routing_status;
        printf("\"enabled\":%s,\"dirty\":%s,\"generation\":\"%" PRIu64 "\",\"persisted_generation\":\"%" PRIu64
               "\",\"routes\":%u,\"neighbors\":%u,\"routes6\":%u,\"neighbors6\":%u,\"last_error\":%d",
            boolean(value->enabled), boolean(value->dirty), value->generation, value->persisted_generation,
            value->route_count, value->neighbor_count, value->route6_count, value->neighbor6_count, value->last_error);
        break;
    }
    case DPPD_MANAGEMENT_PING:
        printf("\"revision\":\"%" PRIu64 "\",\"rules\":%u", response->payload.pong.repository_generation,
            response->payload.pong.rule_count);
        break;
    case DPPD_MANAGEMENT_HEALTH_GET: {
        const struct dppd_management_health *value = &response->payload.health;
        printf("\"live\":true,\"ready\":%s,\"blockers\":%u,\"workers_running\":%u,\"workers_expected\":%u,"
               "\"workers_failed\":%u,\"ports_started\":%u,\"ports_expected\":%u,\"links_up\":%u,\"links_down\":%u,"
               "\"links_unknown\":%u,\"links_unsupported\":%u,\"dirty\":%s,\"revision\":\"%" PRIu64
               "\",\"durable_revision\":\"%" PRIu64 "\",\"rules\":%u,\"routing_enabled\":%s,"
               "\"routing_generation\":\"%" PRIu64 "\"",
            boolean(value->ready), value->blockers, value->workers_running, value->workers_expected,
            value->workers_failed, value->ports_started, value->ports_expected, value->links_up, value->links_down,
            value->links_unknown, value->links_unsupported, boolean(value->persistence_dirty),
            value->repository_generation, value->persisted_generation, value->rule_count,
            boolean(value->routing.enabled), value->routing.generation);
        break;
    }
    case DPPD_MANAGEMENT_STATS_QUERY: {
        bool first = true;
#define FIELD(name)                                                                            \
    printf("%s\"" #name "\":\"%" PRIu64 "\"", first ? "" : ",", response->payload.stats.name); \
    first = false;
        DPPD_STATS_FIELDS(FIELD)
#undef FIELD
        break;
    }
    case DPPD_MANAGEMENT_FLOW_STATUS:
    case DPPD_MANAGEMENT_FLOW_RETRY: {
        const struct dppd_flow_store_status *value = &response->payload.flow_status.status;
        printf("\"session\":\"%" PRIu64 "\",\"ready\":%s,\"dirty\":%s,\"residual\":%s,\"closing\":%s,"
               "\"isolated\":%s,\"revision\":\"%" PRIu64 "\",\"durable_revision\":\"%" PRIu64
               "\",\"rules\":%u,\"jobs\":%u,\"last_error\":%d",
            response->payload.flow_status.session, boolean(value->ready), boolean(value->dirty),
            boolean(value->residual), boolean(value->closing), boolean(value->service.isolated),
            value->service.revision, value->durable_revision, value->service.rules, value->service.jobs,
            value->last_error);
        break;
    }
    case DPPD_MANAGEMENT_PERSISTENCE_STATUS:
    case DPPD_MANAGEMENT_PERSISTENCE_FLUSH: {
        const struct dppd_control_persistence_status *value = &response->payload.persistence;
        printf("\"enabled\":%s,\"dirty\":%s,\"revision\":\"%" PRIu64 "\",\"durable_revision\":\"%" PRIu64
               "\",\"last_error\":%d",
            boolean(value->enabled), boolean(value->dirty), value->current_generation, value->persisted_generation,
            value->last_error);
        break;
    }
    case DPPD_MANAGEMENT_RULE_GET:
        printf("\"rule\":");
        rule(&response->payload.rule);
        break;
    case DPPD_MANAGEMENT_RULE_LIST: {
        const struct dppd_management_rule_page *page = &response->payload.rule_page;
        printf("\"revision\":\"%" PRIu64 "\",\"next\":\"%" PRIu64 "\",\"total\":%u,\"more\":%s,\"rules\":[",
            page->repository_generation, page->next_after_rule_id, page->total_count, boolean(page->has_more));
        for (uint16_t i = 0; i < page->count && i < DPPD_MANAGEMENT_RULE_PAGE_SIZE; ++i) {
            const struct dppd_management_rule_summary *value = &page->rules[i];
            printf("%s{\"id\":\"%" PRIu64 "\",\"generation\":\"%" PRIu64
                   "\",\"port\":%u,\"priority\":%u,\"matches\":%u,\"actions\":%u}",
                i == 0 ? "" : ",", value->rule_id, value->generation, value->install_port_id, value->priority,
                value->match_mask, value->action_mask);
        }
        putchar(']');
        break;
    }
    case DPPD_MANAGEMENT_RULE_COUNT_QUERY:
        printf("\"id\":\"%" PRIu64 "\",\"generation\":\"%" PRIu64 "\",\"hits\":\"%" PRIu64 "\",\"bytes\":\"%" PRIu64
               "\"",
            response->payload.count.rule_id, response->payload.count.generation, response->payload.count.hits,
            response->payload.count.bytes);
        break;
    case DPPD_MANAGEMENT_SHARED_COUNT_GET:
        shared(&response->payload.shared_count.spec);
        printf(",\"references\":%u", response->payload.shared_count.references);
        break;
    case DPPD_MANAGEMENT_SHARED_COUNT_QUERY:
        shared(&response->payload.shared_count_result.object.spec);
        printf(",\"references\":%u,\"hits\":\"%" PRIu64 "\",\"bytes\":\"%" PRIu64 "\"",
            response->payload.shared_count_result.object.references, response->payload.shared_count_result.hits,
            response->payload.shared_count_result.bytes);
        break;
    case DPPD_MANAGEMENT_FLOW_CONFIG_GET: {
        const struct dppd_flow_config_page *page = &response->payload.flow_configuration;
        printf("\"revision\":\"%" PRIu64 "\",\"durable_revision\":\"%" PRIu64 "\",\"next_generation\":\"%" PRIu64
               "\",\"next_table\":\"%" PRIu64 "\",\"next_count\":\"%" PRIu64
               "\",\"more_tables\":%s,\"more_counts\":%s,\"dirty\":%s,\"residual\":%s,\"table_total\":%u,"
               "\"count_total\":%u,\"tables\":[",
            page->revision, page->durable_revision, page->shared_next_generation, page->next_table, page->next_count,
            boolean(page->more_tables), boolean(page->more_counts), boolean(page->dirty), boolean(page->residual),
            page->table_count, page->shared_count);
        for (uint32_t i = 0; i < page->tables_returned && i < 2; ++i) {
            const struct dppd_flow_state_table *table = &page->tables[i];
            printf("%s{\"key\":\"%" PRIu64 "\",\"capacity\":%u,\"dynamic_actions\":%u,\"example\":", i == 0 ? "" : ",",
                table->key, table->spec.capacity, table->spec.dynamic_actions);
            rule(&table->spec.example);
            putchar('}');
        }
        printf("],\"counts\":[");
        for (uint32_t i = 0; i < page->counts_returned && i < 4; ++i) {
            printf("%s{", i == 0 ? "" : ",");
            shared(&page->counts[i]);
            putchar('}');
        }
        putchar(']');
        break;
    }
    case DPPD_MANAGEMENT_FLOW_CONFIG_APPLY: {
        const struct dppd_flow_config_result *value = &response->payload.flow_config_result;
        printf("\"applied\":%s,\"durable\":%s,\"revision\":\"%" PRIu64 "\",\"durable_revision\":\"%" PRIu64
               "\",\"generations\":[",
            boolean(value->applied), boolean(value->durable), value->revision, value->durable_revision);
        for (uint32_t i = 0; i < 8; ++i)
            printf("%s\"%" PRIu64 "\"", i == 0 ? "" : ",", value->generations[i]);
        putchar(']');
        break;
    }
    default:
        break;
    }
finish:
    printf("}}\n");
    if (ferror(stdout))
        return -EIO;
    if (response->status == 0 && durable_result && response->payload.durable_result.phase == DPPD_FLOW_REQUEST_DONE)
        return response->payload.durable_result.status;
    return response->status;
}
