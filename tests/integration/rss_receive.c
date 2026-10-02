#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>

#define PACKETS 4096U
#define FLOWS 64U
#define QUEUES 2U

static uint64_t read_be(const unsigned char *data, unsigned int length)
{
    uint64_t value = 0;
    for (unsigned int i = 0; i < length; ++i)
        value = (value << 8) | data[i];
    return value;
}

int main(int argc, char **argv)
{
    struct rte_eth_dev_info info;
    struct rte_eth_conf conf = {0};
    struct rte_mempool *pool = NULL;
    struct rte_mbuf *burst[32];
    unsigned char seen[PACKETS] = {0};
    unsigned char flow_seen[FLOWS] = {0};
    uint16_t flow_queue[FLOWS] = {0};
    uint32_t flow_hash[FLOWS] = {0};
    uint32_t counts[QUEUES] = {0};
    uint32_t received = 0, errors = 0, rss_packets = 0;
    uint16_t port = UINT16_MAX, candidate;
    uint16_t rx_desc = 512, tx_desc = 512;
    int result = 1;
    int configured = 0, started = 0;
    uint64_t deadline;

    if (rte_eal_init(argc, argv) < 0)
        return 1;
    RTE_ETH_FOREACH_DEV(candidate) {
        if (port != UINT16_MAX) {
            fprintf(stderr, "allow exactly one data NIC with EAL -a\n");
            goto cleanup;
        }
        port = candidate;
    }
    if (port == UINT16_MAX) {
        fprintf(stderr, "no data NIC; allow one data NIC with EAL -a\n");
        goto cleanup;
    }
    if (rte_eth_dev_info_get(port, &info) != 0)
        goto cleanup;
    printf("PMD=%s rx_queues=%u tx_queues=%u rss_capa=0x%" PRIx64 "\n",
           info.driver_name, info.max_rx_queues, info.max_tx_queues,
           info.flow_type_rss_offloads);
    if (info.max_rx_queues < QUEUES || info.max_tx_queues < QUEUES ||
        !(info.flow_type_rss_offloads & RTE_ETH_RSS_NONFRAG_IPV4_UDP)) {
        fprintf(stderr, "two queues and IPv4 UDP RSS are required\n");
        goto cleanup;
    }
    pool = rte_pktmbuf_pool_create("rss_receive_pool", 8191, 0, 0,
                                  RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (pool == NULL)
        goto cleanup;
    conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    conf.rx_adv_conf.rss_conf.rss_hf =
        (RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP | RTE_ETH_RSS_TCP) &
        info.flow_type_rss_offloads;
    if (rte_eth_dev_configure(port, QUEUES, QUEUES, &conf) != 0)
        goto cleanup;
    configured = 1;
    if (rte_eth_dev_adjust_nb_rx_tx_desc(port, &rx_desc, &tx_desc) != 0)
        goto cleanup;
    for (uint16_t q = 0; q < QUEUES; ++q) {
        if (rte_eth_rx_queue_setup(port, q, rx_desc, rte_socket_id(),
                                  &info.default_rxconf, pool) != 0 ||
            rte_eth_tx_queue_setup(port, q, tx_desc, rte_socket_id(),
                                  &info.default_txconf) != 0)
            goto cleanup;
    }
    if (rte_eth_dev_start(port) != 0)
        goto cleanup;
    started = 1;
    printf("RSS_RECEIVER_READY packets=%u flows=%u queues=%u rss=0x%" PRIx64 "\n",
           PACKETS, FLOWS, QUEUES, conf.rx_adv_conf.rss_conf.rss_hf);
    fflush(stdout);
    deadline = rte_get_timer_cycles() + 60 * rte_get_timer_hz();
    while (received < PACKETS && rte_get_timer_cycles() < deadline) {
        for (uint16_t q = 0; q < QUEUES; ++q) {
            uint16_t count = rte_eth_rx_burst(port, q, burst, 32);
            for (uint16_t i = 0; i < count; ++i) {
                unsigned char buffer[62];
                const unsigned char *data = rte_pktmbuf_read(burst[i], 0, sizeof(buffer), buffer);
                if (data != NULL && memcmp(data + 42, "DPPRSS01", 8) == 0) {
                    uint64_t flow = read_be(data + 50, 4);
                    uint64_t sequence = read_be(data + 54, 8);
                    if (rte_pktmbuf_pkt_len(burst[i]) < sizeof(buffer) ||
                        read_be(data + 12, 2) != 0x0800 || data[14] != 0x45 ||
                        data[23] != 17 || read_be(data + 16, 2) != 48 ||
                        read_be(data + 38, 2) != 28 ||
                        read_be(data + 26, 4) != 0xc0a86402 ||
                        read_be(data + 30, 4) != 0xc0a86401 ||
                        flow >= FLOWS || sequence >= PACKETS ||
                        flow != sequence % FLOWS ||
                        read_be(data + 34, 2) != 20000 + flow ||
                        read_be(data + 36, 2) != 10000) {
                        ++errors;
                    } else if (seen[sequence]) {
                        ++errors;
                    } else {
                        seen[sequence] = 1;
                        ++received;
                        ++counts[q];
                        if (burst[i]->ol_flags & RTE_MBUF_F_RX_RSS_HASH) {
                            ++rss_packets;
                            if (flow_seen[flow] &&
                                (flow_queue[flow] != q || flow_hash[flow] != burst[i]->hash.rss))
                                ++errors;
                            flow_seen[flow] = 1;
                            flow_queue[flow] = q;
                            flow_hash[flow] = burst[i]->hash.rss;
                        } else {
                            ++errors;
                        }
                    }
                }
                rte_pktmbuf_free(burst[i]);
            }
        }
    }
    printf("RSS_RESULT received=%u missing=%u queue0=%u queue1=%u rss_hash_packets=%u errors=%u\n",
           received, PACKETS - received, counts[0], counts[1], rss_packets, errors);
    result = !(received == PACKETS && counts[0] > 0 && counts[1] > 0 &&
               rss_packets == PACKETS && errors == 0);
cleanup:
    if (started)
        rte_eth_dev_stop(port);
    if (configured)
        rte_eth_dev_close(port);
    if (pool != NULL)
        rte_mempool_free(pool);
    rte_eal_cleanup();
    return result;
}
