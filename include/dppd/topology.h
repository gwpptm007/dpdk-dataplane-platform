#ifndef DPPD_TOPOLOGY_H
#define DPPD_TOPOLOGY_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/config.h"

enum dppd_endpoint_kind {
    DPPD_ENDPOINT_ETHDEV = 0,
    DPPD_ENDPOINT_REPRESENTOR,
};

struct dppd_endpoint {
    uint16_t ethdev_port_id;
    enum dppd_endpoint_kind kind;
    bool has_switch_domain;
    uint16_t switch_domain_id;
    uint16_t switch_port_id;
    char switch_name[64];
    char driver_name[64];
};

struct dppd_topology {
    uint16_t nb_endpoints;
    struct dppd_endpoint endpoints[DPPD_MAX_PORTS];
};

int dppd_topology_discover(const struct dppd_config *cfg, struct dppd_topology *topology);
const struct dppd_endpoint *dppd_topology_find(const struct dppd_topology *topology,
                                               uint16_t ethdev_port_id);
void dppd_topology_dump(const struct dppd_topology *topology);

#endif

