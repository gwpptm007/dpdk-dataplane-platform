#include "transfer_cli.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int number(const char *text, uint64_t maximum, uint64_t *output)
{
    if (text == NULL || *text == '\0')
        return -EINVAL;
    for (const char *p = text; *p != '\0'; ++p)
        if (*p < '0' || *p > '9')
            return -EINVAL;
    errno = 0;
    char *end;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || value > maximum)
        return -EINVAL;
    *output = value;
    return 0;
}

void dppctl_transfer_usage(const char *program)
{
    fprintf(stderr,
        "  %s [--socket PATH] transfer-plan ID INSTALL SRC DST TEMPLATE oneway|bidir [count] [priority:N] [age:SECONDS]\n",
        program);
    fprintf(stderr,
        "  %s [--socket PATH] transfer-apply ID INSTALL SRC DST TEMPLATE oneway|bidir EXPECTED"
        " [count] [priority:N] [age:SECONDS]\n",
        program);
    fprintf(stderr,
        "  TEMPLATE: pair|uplink-vf|uplink-sf|pf-vf|pf-sf|vf-vf|sf-sf; count may be replaced with shared-count:ID:GEN\n");
}

int dppctl_transfer_build(int argc, char **argv, struct dppd_management_request *request,
    void (*initialize)(struct dppd_management_request *, enum dppd_management_operation))
{
    if (argc == 0 || (strcmp(argv[0], "transfer-plan") != 0 && strcmp(argv[0], "transfer-apply") != 0))
        return -ENOSYS;
    bool apply = strcmp(argv[0], "transfer-apply") == 0;
    int base = apply ? 8 : 7;
    if (argc < base || argc > base + 3)
        return -EINVAL;
    initialize(request, apply ? DPPD_MANAGEMENT_TRANSFER_APPLY : DPPD_MANAGEMENT_TRANSFER_PLAN);
    struct dppd_transfer_spec *spec = &request->payload.transfer;
    uint64_t value;
    if (number(argv[1], UINT64_MAX - 1, &spec->first_rule_id) != 0 || spec->first_rule_id == 0 ||
        number(argv[2], UINT16_MAX - 1, &value) != 0)
        return -EINVAL;
    spec->install_port_id = (uint16_t)value;
    if (number(argv[3], UINT16_MAX - 1, &value) != 0)
        return -EINVAL;
    spec->source_port_id = (uint16_t)value;
    if (number(argv[4], UINT16_MAX - 1, &value) != 0)
        return -EINVAL;
    spec->destination_port_id = (uint16_t)value;
    spec->scenario = DPPD_TRANSFER_TEMPLATE_COUNT;
    for (unsigned int scenario = 0; scenario < DPPD_TRANSFER_TEMPLATE_COUNT; ++scenario)
        if (strcmp(argv[5], dppd_transfer_template_name(scenario)) == 0)
            spec->scenario = (uint8_t)scenario;
    if (spec->scenario == DPPD_TRANSFER_TEMPLATE_COUNT ||
        (strcmp(argv[6], "oneway") != 0 && strcmp(argv[6], "bidir") != 0))
        return -EINVAL;
    spec->bidirectional = strcmp(argv[6], "bidir") == 0;
    if (spec->source_port_id == spec->destination_port_id ||
        (spec->bidirectional && spec->first_rule_id == UINT64_MAX - 1))
        return -EINVAL;
    spec->expected_repository_generation = DPPD_RULE_GENERATION_ANY;
    if (apply && number(argv[7], UINT64_MAX - 1, &spec->expected_repository_generation) != 0)
        return -EINVAL;
    bool priority = false, age = false;
    for (int i = base; i < argc; ++i) {
        if (strcmp(argv[i], "count") == 0 && !spec->with_count && spec->shared_count_id == 0)
            spec->with_count = 1;
        else if (strncmp(argv[i], "shared-count:", 13) == 0 && !spec->with_count && spec->shared_count_id == 0) {
            /** 不修改原始参数，拆开的两段都必须是完整十进制身份 */
            const char *id = argv[i] + 13, *colon = strchr(id, ':');
            char digits[21];
            if (colon == NULL || colon == id || (size_t)(colon - id) >= sizeof(digits))
                return -EINVAL;
            memcpy(digits, id, (size_t)(colon - id));
            digits[colon - id] = '\0';
            if (number(digits, UINT64_MAX - 1U, &spec->shared_count_id) != 0 || spec->shared_count_id == 0 ||
                number(colon + 1, UINT64_MAX - 1U, &spec->shared_count_generation) != 0 ||
                spec->shared_count_generation == 0)
                return -EINVAL;
        } else if (strncmp(argv[i], "priority:", 9) == 0 && !priority && number(argv[i] + 9, UINT32_MAX, &value) == 0) {
            spec->priority = (uint32_t)value;
            priority = true;
        } else if (strncmp(argv[i], "age:", 4) == 0 && !age && number(argv[i] + 4, 0xffffffU, &value) == 0 &&
            value != 0) {
            spec->age_timeout_seconds = (uint32_t)value;
            age = true;
        } else
            return -EINVAL;
    }
    return 0;
}

/** 先核对整张图再读取数组，损坏的后一条边也不能让前面的成功摘要提前输出 */
static bool graph_valid(const struct dppd_transfer_graph *graph)
{
    if (graph->count == 0 || graph->count > DPPD_TRANSFER_MAX_EDGES)
        return false;
    for (uint16_t i = 0; i < graph->count; ++i) {
        const struct dppd_rule *rule = &graph->rules[i];
        if (rule->nb_matches == 0 || rule->nb_matches > DPPD_RULE_MAX_ITEMS || rule->nb_actions == 0 ||
            rule->nb_actions > DPPD_RULE_MAX_ACTIONS)
            return false;
        if (rule->nb_matches != 2 || rule->nb_actions > 3)
            return false;
        if (rule->matches[0].type != DPPD_MATCH_REPRESENTED_PORT || rule->matches[1].type != DPPD_MATCH_ETH ||
            rule->actions[rule->nb_actions - 1U].type != DPPD_ACTION_REPRESENTED_PORT)
            return false;
        bool count = false, age = false;
        for (uint16_t item = 0; item + 1U < rule->nb_actions; ++item) {
            const struct dppd_action *action = &rule->actions[item];
            if (action->type == DPPD_ACTION_COUNT && !count && !age)
                count = true;
            else if (action->type == DPPD_ACTION_SHARED_COUNT && !count && !age && action->conf.shared_count.id != 0 &&
                action->conf.shared_count.id != UINT64_MAX && action->conf.shared_count.generation != 0 &&
                action->conf.shared_count.generation != UINT64_MAX)
                count = true;
            else if (action->type == DPPD_ACTION_AGE && !age && action->conf.age_timeout_seconds != 0 &&
                action->conf.age_timeout_seconds <= 0xffffffU)
                age = true;
            else
                return false;
        }
    }
    return true;
}

int dppctl_transfer_print(const struct dppd_management_response *response)
{
    if (response->operation != DPPD_MANAGEMENT_TRANSFER_PLAN && response->operation != DPPD_MANAGEMENT_TRANSFER_APPLY)
        return 0;
    const struct dppd_transfer_graph *graph = &response->payload.transfer.graph;
    if (!graph_valid(graph)) {
        fprintf(stderr, "dppctl: malformed transfer response\n");
        return -EPROTO;
    }
    printf("transfer=%s repository-generation=%" PRIu64 " domain=%u edges=%u hardware-acceptance=not-run\n",
        response->operation == DPPD_MANAGEMENT_TRANSFER_PLAN ? "planned" : "created", graph->repository_generation,
        graph->switch_domain_id, graph->count);
    for (uint16_t i = 0; i < graph->count; ++i) {
        const struct dppd_rule *rule = &graph->rules[i];
        bool count = false;
        uint32_t timeout = 0;
        for (uint16_t item = 0; item < rule->nb_actions; ++item) {
            count |= rule->actions[item].type == DPPD_ACTION_COUNT;
            if (rule->actions[item].type == DPPD_ACTION_AGE)
                timeout = rule->actions[item].conf.age_timeout_seconds;
        }
        printf("edge=%u id=%" PRIu64 " generation=%" PRIu64
               " install=%u source=%u destination=%u priority=%u count=%s age-seconds=%u",
            i, rule->id, rule->generation, rule->install_port_id, rule->matches[0].spec.ethdev_port_id,
            rule->actions[rule->nb_actions - 1].conf.ethdev_port_id, rule->priority, count ? "yes" : "no", timeout);
        for (uint16_t item = 0; item < rule->nb_actions; ++item)
            if (rule->actions[item].type == DPPD_ACTION_SHARED_COUNT)
                printf(" shared-count=%" PRIu64 " shared-generation=%" PRIu64, rule->actions[item].conf.shared_count.id,
                    rule->actions[item].conf.shared_count.generation);
        printf("\n");
    }
    printf("%.*s\n", DPPD_TRANSFER_MESSAGE_SIZE, response->payload.transfer.message);
    return 1;
}
