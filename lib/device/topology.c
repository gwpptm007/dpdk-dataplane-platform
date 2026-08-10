#include "dppd/topology.h"

#include <stdio.h>
#include <string.h>
#include <rte_ethdev.h>

int dppd_topology_discover(const struct dppd_config *cfg, struct dppd_topology *topology)
{
    uint16_t i;

    if (cfg == NULL || topology == NULL)
        return -1;
    memset(topology, 0, sizeof(*topology));

    for (i = 0; i < cfg->nb_ports; ++i) {
        struct rte_eth_dev_info info;
        struct dppd_endpoint *endpoint = &topology->endpoints[topology->nb_endpoints];
        int rc;

        memset(&info, 0, sizeof(info));
        rc = rte_eth_dev_info_get(cfg->ports[i], &info);
        if (rc != 0)
            return rc;

        endpoint->ethdev_port_id = cfg->ports[i];
        endpoint->kind = DPPD_ENDPOINT_ETHDEV;
        if (info.dev_flags != NULL && ((*info.dev_flags & RTE_ETH_DEV_REPRESENTOR) != 0))
            endpoint->kind = DPPD_ENDPOINT_REPRESENTOR;
        endpoint->has_switch_domain =
            info.switch_info.domain_id != RTE_ETH_DEV_SWITCH_DOMAIN_ID_INVALID;
        endpoint->switch_domain_id = info.switch_info.domain_id;
        endpoint->switch_port_id = info.switch_info.port_id;
        snprintf(endpoint->switch_name, sizeof(endpoint->switch_name), "%s",
                 info.switch_info.name != NULL ? info.switch_info.name : "-");
        snprintf(endpoint->driver_name, sizeof(endpoint->driver_name), "%s",
                 info.driver_name != NULL ? info.driver_name : "unknown");
        topology->nb_endpoints++;
    }
    return 0;
}

const struct dppd_endpoint *dppd_topology_find(const struct dppd_topology *topology,
                                               uint16_t ethdev_port_id)
{
    uint16_t i;

    if (topology == NULL)
        return NULL;
    for (i = 0; i < topology->nb_endpoints; ++i) {
        if (topology->endpoints[i].ethdev_port_id == ethdev_port_id)
            return &topology->endpoints[i];
    }
    return NULL;
}

void dppd_topology_dump(const struct dppd_topology *topology)
{
    uint16_t i;

    for (i = 0; i < topology->nb_endpoints; ++i) {
        const struct dppd_endpoint *endpoint = &topology->endpoints[i];
        printf("[dppd] endpoint port=%u kind=%s driver=%s switch-domain=%s",
               endpoint->ethdev_port_id,
               endpoint->kind == DPPD_ENDPOINT_REPRESENTOR ? "representor" : "ethdev",
               endpoint->driver_name,
               endpoint->has_switch_domain ? "yes" : "no");
        if (endpoint->has_switch_domain)
            printf(" domain=%u switch-port=%u switch=%s",
                   endpoint->switch_domain_id,
                   endpoint->switch_port_id,
                   endpoint->switch_name);
        printf("\n");
    }
}

