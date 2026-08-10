#ifndef DPPD_OFFLOAD_H
#define DPPD_OFFLOAD_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/rule.h"

struct rte_flow;

struct dppd_flow_handle {
    uint64_t rule_id;
    uint64_t rule_generation;
    uint16_t port_id;
    struct rte_flow *flow;
    bool has_count;
};

struct dppd_flow_error {
    int code;
    char message[256];
};

int dppd_flow_validate(uint16_t port_id,
                       const struct dppd_rule *rule,
                       struct dppd_flow_error *error);
int dppd_flow_create(uint16_t port_id,
                     const struct dppd_rule *rule,
                     struct dppd_flow_handle *handle,
                     struct dppd_flow_error *error);
int dppd_flow_install(uint16_t port_id,
                      const struct dppd_rule *rule,
                      struct dppd_flow_handle *handle,
                      struct dppd_flow_error *error);
int dppd_flow_remove(struct dppd_flow_handle *handle, struct dppd_flow_error *error);
int dppd_flow_query_count(const struct dppd_flow_handle *handle,
                          uint64_t *hits,
                          uint64_t *bytes,
                          struct dppd_flow_error *error);
int dppd_flow_flush(uint16_t port_id, struct dppd_flow_error *error);

#endif
