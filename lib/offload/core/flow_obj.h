#ifndef DPPD_FLOW_OBJ_H
#define DPPD_FLOW_OBJ_H

#include <stdbool.h>
#include <stdint.h>
#include "match.h"
#include "action.h"

enum dppd_offload_backend {
    DPPD_BACKEND_SOFT = 0,
    DPPD_BACKEND_RTE_FLOW,
    DPPD_BACKEND_FDIR,
};

enum dppd_traffic_direction {
    DPPD_DIR_INGRESS = 0,
    DPPD_DIR_EGRESS,
    DPPD_DIR_TRANSFER,
};

struct dppd_flow_obj {
    enum dppd_offload_backend backend;
    enum dppd_traffic_direction direction;
    struct dppd_match match;
    struct dppd_action action;
    uint16_t dst_port_id;
    uint16_t priority;
    bool transfer;
};

#endif
