#include "json.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>

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
    if (value->operation == DPPD_MANAGEMENT_REQUEST_SUBMIT || value->operation == DPPD_MANAGEMENT_REQUEST_GET)
        return value->payload.durable_result.count <= 8;
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
        static const char *operations[] = {"unknown", "create", "update", "remove", "configure"};
        printf("\"client\":\"%" PRIu64 "\",\"sequence\":\"%" PRIu64
               "\",\"operation\":\"%s\",\"state\":\"%s\",\"status\":%d,\"applied\":%s,\"durable\":%s,"
               "\"blocked\":%s,\"revision\":\"%" PRIu64 "\",\"rules\":[",
            value->key.client, value->key.sequence,
            (unsigned int)value->operation <= 4 ? operations[value->operation] : "unknown",
            value->phase == DPPD_FLOW_REQUEST_DONE ? "complete" : "pending", value->status, boolean(value->applied),
            boolean(value->durable), boolean(response->status == -EUCLEAN), value->revision);
        for (uint32_t i = 0; i < value->count && i < 8; ++i)
            printf("%s{\"id\":\"%" PRIu64 "\",\"generation\":\"%" PRIu64 "\"}", i == 0 ? "" : ",", value->ids[i],
                value->generations[i]);
        putchar(']');
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
               "\",\"durable_revision\":\"%" PRIu64 "\",\"rules\":%u",
            boolean(value->ready), value->blockers, value->workers_running, value->workers_expected,
            value->workers_failed, value->ports_started, value->ports_expected, value->links_up, value->links_down,
            value->links_unknown, value->links_unsupported, boolean(value->persistence_dirty),
            value->repository_generation, value->persisted_generation, value->rule_count);
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
