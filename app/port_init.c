#include "port_init.h"
#include "dppd/app.h"
#include <stdio.h>
#include <string.h>

/*
 * port_init.c 负责把“配置”落成“运行时对象”。
 * 在 DPDK 模式下，这里会真正完成：
 * - mbuf pool 创建
 * - ethdev 配置
 * - rx/tx queue 建立
 * - 端口启动前的能力探测
 * mock 模式下则保留同样的调用顺序，便于先把工程主线跑通。
 */

#if DPPD_HAS_DPDK
#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_malloc.h>
#endif

static struct dppd_app_config g_cfg;
static void *g_mbuf_pool;
static uint64_t g_tx_offloads;

void *dppd_pktmbuf_pool_get(void)
{
    return g_mbuf_pool;
}

uint64_t dppd_port_tx_offloads_get(void)
{
    return g_tx_offloads;
}

const struct dppd_app_config *dppd_port_cfg_get(void)
{
    return &g_cfg;
}

int dppd_port_init(uint16_t port_id, const struct dppd_app_config *cfg)
{
    if (cfg == NULL)
        return -1;

    g_cfg = *cfg;
    g_tx_offloads = 0;

#if DPPD_HAS_DPDK
    {
        struct rte_eth_conf port_conf;
        struct rte_eth_dev_info dev_info;
        struct rte_eth_txconf tx_conf;
        int nb_rxd = 1024;
        int nb_txd = 1024;
        int ret;
        uint16_t q;
        struct rte_ether_addr mac;

        memset(&port_conf, 0, sizeof(port_conf));
        memset(&dev_info, 0, sizeof(dev_info));
        memset(&tx_conf, 0, sizeof(tx_conf));

        if (!rte_eth_dev_is_valid_port(port_id)) {
            fprintf(stderr, "[dppd] invalid port id: %u\n", port_id);
            return -1;
        }

        g_mbuf_pool = rte_pktmbuf_pool_create("dppd_mp",
                                              cfg->mbuf_count,
                                              cfg->mbuf_cache,
                                              0,
                                              RTE_MBUF_DEFAULT_BUF_SIZE,
                                              rte_socket_id());
        if (g_mbuf_pool == NULL) {
            fprintf(stderr, "[dppd] rte_pktmbuf_pool_create failed\n");
            return -1;
        }

        ret = rte_eth_dev_info_get(port_id, &dev_info);
        if (ret != 0) {
            fprintf(stderr, "[dppd] rte_eth_dev_info_get failed: %d\n", ret);
            return -1;
        }

        if (cfg->nb_rxq > 1)
            port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
        else
            port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;

#ifdef RTE_ETH_RSS_IP
        port_conf.rx_adv_conf.rss_conf.rss_hf = RTE_ETH_RSS_IP;
#endif

#ifdef RTE_ETH_TX_OFFLOAD_IPV4_CKSUM
        if ((dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM) != 0)
            g_tx_offloads |= RTE_ETH_TX_OFFLOAD_IPV4_CKSUM;
#endif
#ifdef RTE_ETH_TX_OFFLOAD_UDP_CKSUM
        if ((dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM) != 0)
            g_tx_offloads |= RTE_ETH_TX_OFFLOAD_UDP_CKSUM;
#endif
        port_conf.txmode.offloads = g_tx_offloads;

        ret = rte_eth_dev_configure(port_id, cfg->nb_rxq, cfg->nb_txq, &port_conf);
        if (ret != 0) {
            fprintf(stderr, "[dppd] rte_eth_dev_configure failed: %d\n", ret);
            return -1;
        }

        ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rxd, &nb_txd);
        if (ret != 0) {
            fprintf(stderr, "[dppd] rte_eth_dev_adjust_nb_rx_tx_desc failed: %d\n", ret);
            return -1;
        }

        for (q = 0; q < cfg->nb_rxq; ++q) {
            ret = rte_eth_rx_queue_setup(port_id,
                                         q,
                                         (uint16_t)nb_rxd,
                                         rte_eth_dev_socket_id(port_id),
                                         NULL,
                                         (struct rte_mempool *)g_mbuf_pool);
            if (ret != 0) {
                fprintf(stderr, "[dppd] rte_eth_rx_queue_setup q=%u failed: %d\n", q, ret);
                return -1;
            }
        }

        tx_conf = dev_info.default_txconf;
        tx_conf.offloads = g_tx_offloads;
        for (q = 0; q < cfg->nb_txq; ++q) {
            ret = rte_eth_tx_queue_setup(port_id,
                                         q,
                                         (uint16_t)nb_txd,
                                         rte_eth_dev_socket_id(port_id),
                                         &tx_conf);
            if (ret != 0) {
                fprintf(stderr, "[dppd] rte_eth_tx_queue_setup q=%u failed: %d\n", q, ret);
                return -1;
            }
        }

        rte_eth_macaddr_get(port_id, &mac);
        printf("[dppd] port_init(dpdk) port=%u rxq=%u txq=%u mbufs=%u cache=%u mac=%02x:%02x:%02x:%02x:%02x:%02x tx_offloads=0x%" PRIx64 "\n",
               port_id,
               cfg->nb_rxq,
               cfg->nb_txq,
               cfg->mbuf_count,
               cfg->mbuf_cache,
               mac.addr_bytes[0], mac.addr_bytes[1], mac.addr_bytes[2],
               mac.addr_bytes[3], mac.addr_bytes[4], mac.addr_bytes[5],
               g_tx_offloads);
    }
#else
    printf("[dppd] port_init(mock) port=%u rxq=%u txq=%u mbufs=%u cache=%u\n",
           port_id,
           cfg->nb_rxq,
           cfg->nb_txq,
           cfg->mbuf_count,
           cfg->mbuf_cache);
#endif
    return 0;
}

int dppd_port_start(uint16_t port_id)
{
#if DPPD_HAS_DPDK
    {
        int ret;
        struct rte_eth_link link;

        ret = rte_eth_dev_start(port_id);
        if (ret != 0) {
            fprintf(stderr, "[dppd] rte_eth_dev_start failed: %d\n", ret);
            return -1;
        }

        if (g_cfg.promiscuous)
            rte_eth_promiscuous_enable(port_id);

        memset(&link, 0, sizeof(link));
        ret = rte_eth_link_get_nowait(port_id, &link);
        if (ret == 0) {
            printf("[dppd] port_start(dpdk) port=%u link=%s speed=%u duplex=%s\n",
                   port_id,
                   link.link_status ? "up" : "down",
                   link.link_speed,
                   link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX ? "full" : "half");
        } else {
            printf("[dppd] port_start(dpdk) port=%u link=unknown\n", port_id);
        }
    }
#else
    printf("[dppd] port_start(mock) port=%u\n", port_id);
#endif
    return 0;
}

void dppd_port_stop(uint16_t port_id)
{
#if DPPD_HAS_DPDK
    rte_eth_dev_stop(port_id);
    rte_eth_dev_close(port_id);
    printf("[dppd] port_stop(dpdk) port=%u\n", port_id);
#else
    printf("[dppd] port_stop(mock) port=%u\n", port_id);
#endif
}
