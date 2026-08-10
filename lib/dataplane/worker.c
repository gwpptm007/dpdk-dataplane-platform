#include "dppd/runtime.h"

#include <string.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>

static void free_packets(struct rte_mbuf **packets, uint16_t count)
{
    uint16_t i;

    for (i = 0; i < count; ++i)
        rte_pktmbuf_free(packets[i]);
}

static void process_ingress(struct dppd_worker *worker, const struct dppd_port *ingress)
{
    struct dppd_runtime *runtime = worker->runtime;
    const struct dppd_port *egress;
    struct rte_mbuf *rx[DPPD_MAX_BURST];
    struct rte_mbuf *tx[DPPD_MAX_BURST];
    uint32_t tx_lengths[DPPD_MAX_BURST];
    struct dppd_stats_values delta;
    uint16_t nb_rx;
    uint16_t nb_tx = 0;
    uint16_t i;

    nb_rx = rte_eth_rx_burst(ingress->port_id,
                             worker->queue_id,
                             rx,
                             runtime->config.burst_size);
    if (nb_rx == 0)
        return;

    memset(&delta, 0, sizeof(delta));
    delta.rx_packets = nb_rx;
    egress = dppd_devices_find(&runtime->devices, ingress->peer_port_id);

    for (i = 0; i < nb_rx; ++i) {
        struct dppd_pipeline_decision decision;
        struct dppd_packet packet;
        const uint32_t packet_len = rte_pktmbuf_pkt_len(rx[i]);
        int parse_status;

        delta.rx_bytes += packet_len;
        parse_status = dppd_packet_parse_mbuf(rx[i], &packet);
        if (parse_status == DPPD_PARSE_MALFORMED)
            delta.rx_malformed++;
        else if (parse_status == DPPD_PARSE_UNSUPPORTED)
            delta.rx_unsupported++;

        dppd_pipeline_decide(&runtime->snapshot,
                             ingress->port_id,
                             parse_status,
                             &packet,
                             &decision);
        if (decision.action != DPPD_PACKET_FORWARD) {
            if (decision.drop_reason != DPPD_DROP_MALFORMED)
                delta.policy_drops++;
            rte_pktmbuf_free(rx[i]);
            continue;
        }

        if (egress == NULL || decision.egress_port != egress->port_id) {
            delta.policy_drops++;
            rte_pktmbuf_free(rx[i]);
            continue;
        }

        if (rx[i]->nb_segs > 1U &&
            (egress->configured_tx_offloads & RTE_ETH_TX_OFFLOAD_MULTI_SEGS) == 0) {
            if (rte_pktmbuf_linearize(rx[i]) != 0) {
                delta.tx_drops++;
                rte_pktmbuf_free(rx[i]);
                continue;
            }
        }

        tx_lengths[nb_tx] = packet_len;
        tx[nb_tx++] = rx[i];
    }

    if (nb_tx != 0) {
        const uint16_t sent = rte_eth_tx_burst(egress->port_id,
                                               worker->queue_id,
                                               tx,
                                               nb_tx);

        delta.tx_packets += sent;
        for (i = 0; i < sent; ++i)
            delta.tx_bytes += tx_lengths[i];
        if (sent < nb_tx) {
            delta.tx_drops += (uint64_t)(nb_tx - sent);
            free_packets(&tx[sent], (uint16_t)(nb_tx - sent));
        }
    }

    dppd_stats_add(&worker->stats, &delta);
}

int dppd_worker_main(void *arg)
{
    struct dppd_worker *worker = arg;
    struct dppd_runtime *runtime;

    if (worker == NULL || worker->runtime == NULL)
        return -1;
    runtime = worker->runtime;

    while (!atomic_load_explicit(&runtime->stop_requested, memory_order_acquire)) {
        uint16_t i;

        for (i = 0; i < runtime->devices.nb_ports; ++i)
            process_ingress(worker, &runtime->devices.ports[i]);
    }
    return 0;
}
