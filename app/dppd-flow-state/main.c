#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dppd/flow_state.h"

static int number(const char *text, uint32_t maximum, uint32_t *output)
{
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (text[0] == '\0' || text[0] == '-' || *end != '\0' || errno != 0 || value > maximum)
        return -EINVAL;
    *output = (uint32_t)value;
    return 0;
}

/** 初始化只定义一个可复用的 Ethernet DROP 模板，不在配置检查工具里启动网卡或 worker */
static int initialize(int argc, char **argv)
{
    uint32_t port, capacity, queues, depth;
    if (argc != 7 || number(argv[3], UINT16_MAX - 1U, &port) != 0 || number(argv[4], 16384, &capacity) != 0 ||
        number(argv[5], 8, &queues) != 0 || number(argv[6], 256, &depth) != 0 || capacity == 0 || queues == 0 ||
        depth == 0)
        return -EINVAL;
    struct dppd_flow_state *state = calloc(1, sizeof(*state));
    if (state == NULL)
        return -ENOMEM;
    state->config = (struct dppd_flow_state_config){.mode = DPPD_FLOW_STATE_SOFTWARE,
        .port_id = (uint16_t)port,
        .rule_capacity = capacity,
        .queue_count = queues,
        .depth = depth,
        .timeout_ns = UINT64_C(5000000000),
        .template_leases = 2U * capacity + 128U,
        .shared_leases = 3U * capacity + 128U};
    state->config.admission.ticket_capacity = 2U * capacity + 256U;
    state->config.admission.port_count = 1;
    state->config.admission.ports[0].port_id = (uint16_t)port;
    for (uint32_t k = 0; k < DPPD_RESOURCE_KIND_COUNT; ++k)
        state->config.admission.limits[k] = state->config.admission.ports[0].limits[k] = capacity + 256U;
    for (uint32_t q = 0; q < queues; ++q)
        state->config.queue_capacity[q] = capacity + 64U;
    state->table_count = 1;
    state->shared_next_generation = 1;
    state->tables[0].key = 1;
    state->tables[0].spec.capacity = capacity + 64U;
    struct dppd_rule *example = &state->tables[0].spec.example;
    *example = (struct dppd_rule){.install_port_id = (uint16_t)port,
        .fallback = DPPD_FALLBACK_SOFTWARE_ONLY,
        .nb_matches = 1,
        .nb_actions = 1};
    example->matches[0].type = DPPD_MATCH_ETH;
    example->actions[0].type = DPPD_ACTION_DROP;
    int rc = dppd_flow_state_initialize(argv[2], state);
    dppd_flow_state_free(&state);
    return rc;
}

int main(int argc, char **argv)
{
    int rc = -EINVAL;
    if (argc >= 2 && strcmp(argv[1], "init-software") == 0)
        rc = initialize(argc, argv);
    else if (argc == 3 && (strcmp(argv[1], "check") == 0 || strcmp(argv[1], "restore-check") == 0)) {
        struct dppd_flow_state *state = NULL;
        rc = dppd_flow_state_load(argv[2], &state);
        if (rc == 0 && strcmp(argv[1], "restore-check") == 0) {
            /** 工具没有独立硬件清空证明，只允许恢复进程退出即消失的软件实际对象 */
            if (state->config.mode != DPPD_FLOW_STATE_SOFTWARE)
                rc = -ENOTSUP;
            else {
                struct dppd_flow_store *store = NULL;
                rc = dppd_flow_store_open(argv[2], NULL, &store);
                if (rc == 0)
                    rc = dppd_flow_store_close(&store);
                if (store != NULL)
                    rc = -EUCLEAN;
            }
        }
        if (rc == 0)
            printf("{\"mode\":%u,\"port\":%u,\"queues\":%u,\"depth\":%u,\"templates\":%u,\"shared_counts\":%u,"
                   "\"rules\":%u,\"revision\":%" PRIu64 ",\"restored\":%s,\"hardware_acceptance\":\"not_run\"}\n",
                (unsigned int)state->config.mode, state->config.port_id, state->config.queue_count, state->config.depth,
                state->table_count, state->shared_count, state->rule_count, state->revision,
                strcmp(argv[1], "restore-check") == 0 ? "true" : "false");
        dppd_flow_state_free(&state);
    }
    if (rc != 0) {
        fprintf(stderr, "flow-state: %s (%d)\n", strerror(-rc), rc);
        fprintf(stderr,
            "usage: dppd-flow-state init-software PATH PORT CAPACITY QUEUES DEPTH | check PATH | restore-check PATH\n");
    }
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
