#define _GNU_SOURCE
#include "config_cli.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int number(const char *text, uint64_t maximum, uint64_t *output)
{
    char *end;
    if (text == NULL || text[0] < '0' || text[0] > '9')
        return -EINVAL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || value > maximum)
        return -EINVAL;
    *output = value;
    return 0;
}

static int change(int argc, char **argv, struct dppd_flow_config_change *output,
    int (*rule)(int, char **, struct dppd_management_request *))
{
    if (argc < 2 || number(argv[1], UINT64_MAX - 1U, &output->key) != 0 || output->key == 0)
        return -EINVAL;
    if (argc == 2 && strcmp(argv[0], "template-delete") == 0) {
        output->operation = DPPD_FLOW_CONFIG_TEMPLATE_DELETE;
        return 0;
    }
    if (argc == 3 && strcmp(argv[0], "count-delete") == 0) {
        output->operation = DPPD_FLOW_CONFIG_COUNT_DELETE;
        return number(argv[2], UINT64_MAX - 1U, &output->expected_generation) == 0 && output->expected_generation != 0
            ? 0
            : -EINVAL;
    }
    if (argc == 4 && strcmp(argv[0], "count-add") == 0) {
        uint64_t port;
        if (number(argv[2], UINT16_MAX - 1U, &port) != 0)
            return -EINVAL;
        output->operation = DPPD_FLOW_CONFIG_COUNT_ADD;
        output->spec.count.port_id = (uint16_t)port;
        output->spec.count.domain = strcmp(argv[3], "ingress") == 0 ? DPPD_RULE_DOMAIN_INGRESS
            : strcmp(argv[3], "egress") == 0                        ? DPPD_RULE_DOMAIN_EGRESS
                                                                    : DPPD_RULE_DOMAIN_TRANSFER;
        return output->spec.count.domain != DPPD_RULE_DOMAIN_TRANSFER || strcmp(argv[3], "transfer") == 0 ? 0 : -EINVAL;
    }
    if (argc >= 8 && strcmp(argv[0], "template-add") == 0) {
        uint64_t capacity, dynamic;
        struct dppd_management_request built;
        if (number(argv[2], 65536, &capacity) != 0 || capacity == 0 || number(argv[3], UINT32_MAX, &dynamic) != 0 ||
            rule(argc - 4, argv + 4, &built) != 0 || built.payload.apply.expected_generation != 0)
            return -EINVAL;
        output->operation = DPPD_FLOW_CONFIG_TEMPLATE_ADD;
        output->spec.table.capacity = (uint32_t)capacity;
        output->spec.table.dynamic_actions = (uint32_t)dynamic;
        output->spec.table.example = built.payload.apply.rule;
        output->spec.table.example.id = output->spec.table.example.generation = 0;
        return 0;
    }
    return -EINVAL;
}

int dppctl_config_build(int argc, char **argv, struct dppd_management_request *request,
    int (*rule)(int, char **, struct dppd_management_request *),
    void (*initialize)(struct dppd_management_request *, enum dppd_management_operation))
{
    if (argc == 0 || strncmp(argv[0], "flow-config-", 12) != 0)
        return -ENOSYS;
    if ((argc == 1 || argc == 2 || argc == 4) && strcmp(argv[0], "flow-config-get") == 0) {
        initialize(request, DPPD_MANAGEMENT_FLOW_CONFIG_GET);
        request->payload.flow_config_get.expected = UINT64_MAX;
        if (argc >= 2 && number(argv[1], UINT64_MAX - 1U, &request->payload.flow_config_get.expected) != 0)
            return -EINVAL;
        if (argc == 4 &&
            (number(argv[2], UINT64_MAX - 1U, &request->payload.flow_config_get.after_table) != 0 ||
                number(argv[3], UINT64_MAX - 1U, &request->payload.flow_config_get.after_count) != 0))
            return -EINVAL;
        return 0;
    }
    if (argc != 3 || strcmp(argv[0], "flow-config-apply") != 0)
        return -EINVAL;
    initialize(request, DPPD_MANAGEMENT_FLOW_CONFIG_APPLY);
    if (number(argv[1], UINT64_MAX - 2U, &request->payload.flow_config_apply.expected) != 0)
        return -EINVAL;
    FILE *file = fopen(argv[2], "r");
    if (file == NULL)
        return -errno;
    char *line = NULL;
    size_t size = 0, total = 0;
    ssize_t bytes;
    int rc = 0;
    /** 整个文件先完整解析，限制行长、总长度和条数，不把错误行之前的前缀发送出去 */
    while ((bytes = getline(&line, &size, file)) >= 0) {
        total += (size_t)bytes;
        if (bytes > 4096 || total > 65536 || memchr(line, '\0', (size_t)bytes) != NULL) {
            rc = -EINVAL;
            break;
        }
        char *words[40], *save = NULL;
        int count = 0;
        for (char *word = strtok_r(line, " \t\r\n", &save); word != NULL; word = strtok_r(NULL, " \t\r\n", &save)) {
            if (word[0] == '#')
                break;
            if (count == 40) {
                rc = -EINVAL;
                break;
            }
            words[count++] = word;
        }
        if (rc != 0)
            break;
        if (count == 0)
            continue;
        uint32_t slot = request->payload.flow_config_apply.count;
        if (slot == DPPD_FLOW_CONFIG_OPERATIONS) {
            rc = -E2BIG;
            break;
        }
        rc = change(count, words, &request->payload.flow_config_apply.changes[slot], rule);
        if (rc != 0)
            break;
        ++request->payload.flow_config_apply.count;
    }
    if (ferror(file))
        rc = -EIO;
    free(line);
    fclose(file);
    return rc != 0 ? rc : request->payload.flow_config_apply.count == 0 ? -EINVAL : 0;
}

int dppctl_config_print(const struct dppd_management_response *response)
{
    if (response->operation == DPPD_MANAGEMENT_FLOW_CONFIG_APPLY) {
        const struct dppd_flow_config_result *result = &response->payload.flow_config_result;
        printf("configuration applied=%s durable=%s revision=%" PRIu64 " durable-revision=%" PRIu64 "\n",
            result->applied ? "yes" : "no", result->durable ? "yes" : "no", result->revision, result->durable_revision);
        for (uint32_t i = 0; i < 8; ++i)
            if (result->generations[i] != 0)
                printf("operation=%u allocated-generation=%" PRIu64 "\n", i, result->generations[i]);
        return 1;
    }
    if (response->operation != DPPD_MANAGEMENT_FLOW_CONFIG_GET)
        return 0;
    const struct dppd_flow_config_page *page = &response->payload.flow_configuration;
    if (page->tables_returned > 2 || page->counts_returned > 4)
        return -EPROTO;
    printf("configuration revision=%" PRIu64 " durable-revision=%" PRIu64
           " templates=%u counts=%u next-generation=%" PRIu64 " dirty=%s residual=%s\n",
        page->revision, page->durable_revision, page->table_count, page->shared_count, page->shared_next_generation,
        page->dirty ? "yes" : "no", page->residual ? "yes" : "no");
    for (uint32_t i = 0; i < page->tables_returned; ++i)
        printf("template key=%" PRIu64 " capacity=%u dynamic=%u matches=%u actions=%u\n", page->tables[i].key,
            page->tables[i].spec.capacity, page->tables[i].spec.dynamic_actions,
            page->tables[i].spec.example.nb_matches, page->tables[i].spec.example.nb_actions);
    for (uint32_t i = 0; i < page->counts_returned; ++i)
        printf("shared id=%" PRIu64 " generation=%" PRIu64 " port=%u domain=%u\n", page->counts[i].id,
            page->counts[i].generation, page->counts[i].install_port_id, (unsigned int)page->counts[i].domain);
    if (page->more_tables || page->more_counts)
        printf("next-command: flow-config-get %" PRIu64 " %" PRIu64 " %" PRIu64 "\n", page->revision, page->next_table,
            page->next_count);
    return 1;
}
