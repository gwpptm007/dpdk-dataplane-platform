#ifndef DPPD_DEVICE_H
#define DPPD_DEVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <rte_config.h>
#include <rte_ether.h>
#include "dppd/config.h"
#include "dppd/topology.h"

struct rte_mempool;

struct dppd_port_capabilities {
    uint16_t max_rx_queues;
    uint16_t max_tx_queues;
    uint16_t reta_size;
    uint64_t rss_offloads;
    uint64_t rx_offloads;
    uint64_t tx_offloads;
    uint64_t device_capabilities;
};

struct dppd_port {
    uint16_t port_id;
    uint16_t peer_port_id;
    int socket_id;
    struct dppd_port_capabilities capabilities;
    uint64_t configured_rss_hf;
    uint64_t configured_tx_offloads;
    struct rte_ether_addr mac;
    bool configured;
    bool started;
};

struct dppd_device_set {
    uint16_t nb_ports;
    struct dppd_port ports[DPPD_MAX_PORTS];
    struct rte_mempool *pools[RTE_MAX_NUMA_NODES];
    struct dppd_topology topology;
};

int dppd_devices_init(struct dppd_device_set *devices, const struct dppd_config *cfg);
void dppd_devices_stop(struct dppd_device_set *devices);
const struct dppd_port *dppd_devices_find(const struct dppd_device_set *devices,
                                          uint16_t port_id);

#endif
