#include "dppd/device.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#define DPPD_DEFAULT_RX_DESC 1024U
#define DPPD_DEFAULT_TX_DESC 1024U

static int port_socket(uint16_t port_id)
{
    int socket_id = rte_eth_dev_socket_id(port_id);

    if (socket_id < 0 || socket_id >= RTE_MAX_NUMA_NODES)
        socket_id = 0;
    return socket_id;
}

static int create_socket_pools(struct dppd_device_set *devices, const struct dppd_config *cfg)
{
    uint16_t i;

    if (cfg->mbuf_cache > RTE_MEMPOOL_CACHE_MAX_SIZE) {
        fprintf(stderr, "[dppd] mbuf cache %u exceeds DPDK maximum %u\n",
                cfg->mbuf_cache, RTE_MEMPOOL_CACHE_MAX_SIZE);
        return -EINVAL;
    }

    for (i = 0; i < cfg->nb_ports; ++i) {
        const int socket_id = port_socket(cfg->ports[i]);
        char name[64];

        if (devices->pools[socket_id] != NULL)
            continue;
        snprintf(name, sizeof(name), "dppd_mbuf_s%d", socket_id);
        devices->pools[socket_id] = rte_pktmbuf_pool_create(name,
                                                            cfg->mbufs_per_socket,
                                                            cfg->mbuf_cache,
                                                            0,
                                                            RTE_MBUF_DEFAULT_BUF_SIZE,
                                                            socket_id);
        if (devices->pools[socket_id] == NULL) {
            fprintf(stderr, "[dppd] cannot create mbuf pool on socket %d: %s\n",
                    socket_id, rte_strerror(rte_errno));
            return -rte_errno;
        }
    }
    return 0;
}

static int configure_port(struct dppd_port *port,
                          const struct dppd_config *cfg,
                          struct rte_mempool *pool)
{
    struct rte_eth_dev_info info;
    struct rte_eth_conf port_conf;
    struct rte_eth_rxconf rx_conf;
    struct rte_eth_txconf tx_conf;
    uint16_t rx_desc = DPPD_DEFAULT_RX_DESC;
    uint16_t tx_desc = DPPD_DEFAULT_TX_DESC;
    uint16_t queue;
    uint64_t requested_rss;
    int rc;

    memset(&info, 0, sizeof(info));
    memset(&port_conf, 0, sizeof(port_conf));
    rc = rte_eth_dev_info_get(port->port_id, &info);
    if (rc != 0)
        return rc;
    port->capabilities.max_rx_queues = info.max_rx_queues;
    port->capabilities.max_tx_queues = info.max_tx_queues;
    port->capabilities.reta_size = info.reta_size;
    port->capabilities.rss_offloads = info.flow_type_rss_offloads;
    port->capabilities.rx_offloads = info.rx_offload_capa;
    port->capabilities.tx_offloads = info.tx_offload_capa;
    port->capabilities.device_capabilities = info.dev_capa;
    if (cfg->nb_queues > port->capabilities.max_rx_queues ||
        cfg->nb_queues > port->capabilities.max_tx_queues) {
        fprintf(stderr,
                "[dppd] port %u supports at most rx=%u tx=%u queues, requested=%u\n",
                port->port_id, info.max_rx_queues, info.max_tx_queues, cfg->nb_queues);
        return -ENOTSUP;
    }

    requested_rss = RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP | RTE_ETH_RSS_TCP;
    port->configured_rss_hf = cfg->nb_queues > 1U ?
        requested_rss & port->capabilities.rss_offloads : 0;
    if (cfg->nb_queues > 1U && port->configured_rss_hf == 0) {
        fprintf(stderr, "[dppd] port %u cannot provide RSS for multiple queues\n",
                port->port_id);
        return -ENOTSUP;
    }
    if (cfg->nb_queues > 1U) {
        port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
        port_conf.rx_adv_conf.rss_conf.rss_hf = port->configured_rss_hf;
    } else {
        port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
    }

    port->configured_tx_offloads =
        port->capabilities.tx_offloads & RTE_ETH_TX_OFFLOAD_MULTI_SEGS;
    port_conf.txmode.offloads = port->configured_tx_offloads;

    rc = rte_eth_dev_configure(port->port_id, cfg->nb_queues, cfg->nb_queues, &port_conf);
    if (rc != 0)
        return rc;
    port->configured = true;

    rc = rte_eth_dev_adjust_nb_rx_tx_desc(port->port_id, &rx_desc, &tx_desc);
    if (rc != 0)
        return rc;

    rx_conf = info.default_rxconf;
    rx_conf.offloads = port_conf.rxmode.offloads;
    tx_conf = info.default_txconf;
    tx_conf.offloads = port_conf.txmode.offloads;
    for (queue = 0; queue < cfg->nb_queues; ++queue) {
        rc = rte_eth_rx_queue_setup(port->port_id,
                                     queue,
                                     rx_desc,
                                     port->socket_id,
                                     &rx_conf,
                                     pool);
        if (rc != 0)
            return rc;
        rc = rte_eth_tx_queue_setup(port->port_id,
                                     queue,
                                     tx_desc,
                                     port->socket_id,
                                     &tx_conf);
        if (rc != 0)
            return rc;
    }

    rc = rte_eth_dev_start(port->port_id);
    if (rc != 0)
        return rc;
    port->started = true;

    if (cfg->promiscuous) {
        rc = rte_eth_promiscuous_enable(port->port_id);
        if (rc != 0)
            return rc;
    }
    rc = rte_eth_macaddr_get(port->port_id, &port->mac);
    if (rc != 0)
        return rc;

    printf("[dppd] port=%u peer=%u socket=%d queues=%u rss=0x%" PRIx64
           " tx-offloads=0x%" PRIx64 " mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
           port->port_id,
           port->peer_port_id,
           port->socket_id,
           cfg->nb_queues,
           port->configured_rss_hf,
           port->configured_tx_offloads,
           port->mac.addr_bytes[0], port->mac.addr_bytes[1], port->mac.addr_bytes[2],
           port->mac.addr_bytes[3], port->mac.addr_bytes[4], port->mac.addr_bytes[5]);
    return 0;
}

int dppd_devices_init(struct dppd_device_set *devices, const struct dppd_config *cfg)
{
    uint16_t i;
    int rc;

    if (devices == NULL || cfg == NULL)
        return -EINVAL;
    memset(devices, 0, sizeof(*devices));

    for (i = 0; i < cfg->nb_ports; ++i) {
        if (!rte_eth_dev_is_valid_port(cfg->ports[i])) {
            fprintf(stderr, "[dppd] invalid ethdev port %u\n", cfg->ports[i]);
            return -ENODEV;
        }
    }

    rc = create_socket_pools(devices, cfg);
    if (rc != 0)
        goto fail;

    devices->nb_ports = cfg->nb_ports;
    for (i = 0; i < cfg->nb_ports; ++i) {
        struct dppd_port *port = &devices->ports[i];
        const uint16_t pair_base = (uint16_t)(i & ~1U);

        port->port_id = cfg->ports[i];
        port->peer_port_id = cfg->ports[pair_base + (i % 2U == 0 ? 1U : 0U)];
        port->socket_id = port_socket(port->port_id);
        rc = configure_port(port, cfg, devices->pools[port->socket_id]);
        if (rc != 0) {
            fprintf(stderr, "[dppd] failed to configure port %u: %s (%d)\n",
                    port->port_id, rte_strerror(-rc), rc);
            goto fail;
        }
    }

    rc = dppd_topology_discover(cfg, &devices->topology);
    if (rc != 0)
        goto fail;
    dppd_topology_dump(&devices->topology);
    return 0;

fail:
    dppd_devices_stop(devices);
    return rc;
}

void dppd_devices_stop(struct dppd_device_set *devices)
{
    uint16_t i;
    int socket_id;

    if (devices == NULL)
        return;
    for (i = 0; i < devices->nb_ports; ++i) {
        struct dppd_port *port = &devices->ports[i];
        if (port->started)
            rte_eth_dev_stop(port->port_id);
        if (port->configured)
            rte_eth_dev_close(port->port_id);
        port->started = false;
        port->configured = false;
    }
    for (socket_id = 0; socket_id < RTE_MAX_NUMA_NODES; ++socket_id) {
        if (devices->pools[socket_id] != NULL) {
            rte_mempool_free(devices->pools[socket_id]);
            devices->pools[socket_id] = NULL;
        }
    }
    devices->nb_ports = 0;
}

const struct dppd_port *dppd_devices_find(const struct dppd_device_set *devices,
                                          uint16_t port_id)
{
    uint16_t i;

    if (devices == NULL)
        return NULL;
    for (i = 0; i < devices->nb_ports; ++i) {
        if (devices->ports[i].port_id == port_id)
            return &devices->ports[i];
    }
    return NULL;
}
