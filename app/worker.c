#include "worker.h"
#include "port_init.h"
#include "stats.h"
#include "dppd/flow.h"
#include "dppd/app.h"
#include "parser.h"
#include "pipeline_fwd.h"
#include "pkt_rewrite.h"
#include "tx_offload.h"
#include "nat_session.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

/*
 * worker.c 是当前 Phase 1 最关键的文件。
 * 这一版开始，worker 不再只靠 fake call-chain 占位：
 * - DPDK 模式下走真实 rx_burst / tx_burst
 * - mock 模式下构造真实二层/三层/四层报文，验证 parser/pipeline/rewrite 主链路
 */

#if DPPD_HAS_DPDK
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#endif

struct dppd_eth_hdr {
    uint8_t dst_addr[6];
    uint8_t src_addr[6];
    uint16_t ether_type;
} __attribute__((packed));

struct dppd_arp_hdr {
    uint16_t hrd;
    uint16_t pro;
    uint8_t hln;
    uint8_t pln;
    uint16_t op;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
} __attribute__((packed));

struct dppd_ipv4_hdr {
    uint8_t version_ihl;
    uint8_t tos;
    uint16_t total_length;
    uint16_t id;
    uint16_t frag_off;
    uint8_t ttl;
    uint8_t next_proto_id;
    uint16_t hdr_checksum;
    uint32_t src_addr;
    uint32_t dst_addr;
} __attribute__((packed));

struct dppd_udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t dgram_len;
    uint16_t dgram_cksum;
} __attribute__((packed));

static int dppd_process_frame(void *frame,
                              uint32_t frame_len,
                              void *mbuf,
                              const struct dppd_app_config *cfg)
{
    struct dppd_parse_result res;
    enum dppd_pipeline_path path;
    int decision;

    if (dppd_parse_packet(frame, frame_len, &res) != 0) {
        dppd_stats_account_rx_drop();
        return DPPD_FWD_DROP;
    }

    dppd_stats_account_parse(res.meta.is_arp,
                             res.meta.is_ipv4,
                             res.meta.is_udp,
                             res.meta.is_tcp);

    path = dppd_select_pipeline_path(&res);
    if (path != DPPD_PATH_SW)
        return DPPD_FWD_DROP;

    decision = dppd_pipeline_forward(&res);
    if (decision != DPPD_FWD_TX)
        return decision;

    if (res.meta.is_ipv4 && res.meta.is_udp) {
        struct dppd_nat_key nat;
        nat.src_ip = res.ipv4_src_be;
        nat.dst_ip = res.ipv4_dst_be;
        nat.src_port = ntohs(res.l4_src_port_be);
        nat.dst_port = ntohs(res.l4_dst_port_be);
        nat.proto = res.meta.l4_proto;

        if (dppd_nat_translate(&nat) != 0 ||
            dppd_rewrite_udp_ipv4(frame, frame_len, &res, &nat) != 0) {
            return DPPD_FWD_DROP;
        }
    }

    dppd_prepare_tx_checksum_flags(mbuf, &res);
    (void)cfg;
    return DPPD_FWD_TX;
}

static uint32_t dppd_build_mock_udp_frame(uint8_t *buf, uint32_t cap)
{
    struct dppd_eth_hdr *eth;
    struct dppd_ipv4_hdr *ip;
    struct dppd_udp_hdr *udp;
    uint8_t *payload;
    const char *text = "hello-dpdk";
    uint16_t payload_len = (uint16_t)strlen(text);
    uint32_t total = sizeof(*eth) + sizeof(*ip) + sizeof(*udp) + payload_len;

    if (cap < total)
        return 0;

    memset(buf, 0, total);
    eth = (struct dppd_eth_hdr *)buf;
    memcpy(eth->dst_addr, "\x00\x11\x22\x33\x44\x55", 6);
    memcpy(eth->src_addr, "\x66\x77\x88\x99\xaa\xbb", 6);
    eth->ether_type = htons(0x0800);

    ip = (struct dppd_ipv4_hdr *)(buf + sizeof(*eth));
    ip->version_ihl = 0x45;
    ip->ttl = 64;
    ip->next_proto_id = 17;
    ip->total_length = htons((uint16_t)(sizeof(*ip) + sizeof(*udp) + payload_len));
    ip->src_addr = htonl(0x0a000001U);
    ip->dst_addr = htonl(0x0a000002U);

    udp = (struct dppd_udp_hdr *)((uint8_t *)ip + sizeof(*ip));
    udp->src_port = htons(12345);
    udp->dst_port = htons(2152);
    udp->dgram_len = htons((uint16_t)(sizeof(*udp) + payload_len));

    payload = (uint8_t *)(udp + 1);
    memcpy(payload, text, payload_len);
    return total;
}

static uint32_t dppd_build_mock_arp_frame(uint8_t *buf, uint32_t cap)
{
    struct dppd_eth_hdr *eth;
    struct dppd_arp_hdr *arp;
    uint32_t total = sizeof(*eth) + sizeof(*arp);

    if (cap < total)
        return 0;

    memset(buf, 0, total);
    eth = (struct dppd_eth_hdr *)buf;
    memset(eth->dst_addr, 0xff, 6);
    memcpy(eth->src_addr, "\x66\x77\x88\x99\xaa\xbb", 6);
    eth->ether_type = htons(0x0806);

    arp = (struct dppd_arp_hdr *)(buf + sizeof(*eth));
    arp->hrd = htons(1);
    arp->pro = htons(0x0800);
    arp->hln = 6;
    arp->pln = 4;
    arp->op = htons(1);
    memcpy(arp->sha, eth->src_addr, 6);
    arp->spa = htonl(0x0a000001U);
    arp->tpa = htonl(0x0a000002U);
    return total;
}

int dppd_worker_main(void *arg)
{
    const struct dppd_app_config *cfg = (const struct dppd_app_config *)arg;
    if (cfg == NULL)
        return -1;
    dppd_worker_poll_once(cfg->port_id, 0);
    return 0;
}

void dppd_worker_poll_once(uint16_t port_id, uint16_t queue_id)
{
    const struct dppd_app_config *cfg = dppd_port_cfg_get();

    if (cfg == NULL)
        return;

#if DPPD_HAS_DPDK
    {
        struct rte_mbuf *pkts[256];
        uint16_t burst_size;
        uint32_t loop;

        burst_size = cfg->burst_size;
        if (burst_size > (uint16_t)(sizeof(pkts) / sizeof(pkts[0])))
            burst_size = (uint16_t)(sizeof(pkts) / sizeof(pkts[0]));

        printf("[dppd] worker poll(dpdk) port=%u queue=%u burst=%u loops=%u\n",
               port_id, queue_id, burst_size, cfg->poll_loops);

        for (loop = 0; loop < cfg->poll_loops; ++loop) {
            uint16_t nb_rx;
            uint16_t i;

            nb_rx = rte_eth_rx_burst(port_id, queue_id, pkts, burst_size);
            for (i = 0; i < nb_rx; ++i) {
                struct rte_mbuf *m = pkts[i];
                void *data = rte_pktmbuf_mtod(m, void *);
                uint32_t pkt_len = rte_pktmbuf_pkt_len(m);
                int decision = dppd_process_frame(data, pkt_len, m, cfg);

                if (decision == DPPD_FWD_TX) {
                    uint16_t sent = rte_eth_tx_burst(port_id, queue_id, &m, 1);
                    if (sent == 0) {
                        dppd_stats_account_fwd_drop();
                        rte_pktmbuf_free(m);
                    } else {
                        dppd_stats_account_fwd_tx();
                    }
                } else {
                    dppd_stats_account_fwd_drop();
                    rte_pktmbuf_free(m);
                }
            }
        }
    }
#else
    {
        uint8_t frame[256];
        uint32_t len;

        printf("[dppd] worker poll(mock) port=%u queue=%u burst=%u loops=%u\n",
               port_id, queue_id, cfg->burst_size, cfg->poll_loops);

        len = dppd_build_mock_udp_frame(frame, sizeof(frame));
        if (len > 0) {
            if (dppd_process_frame(frame, len, NULL, cfg) == DPPD_FWD_TX)
                dppd_stats_account_fwd_tx();
            else
                dppd_stats_account_fwd_drop();
        }

        len = dppd_build_mock_arp_frame(frame, sizeof(frame));
        if (len > 0) {
            if (dppd_process_frame(frame, len, NULL, cfg) == DPPD_FWD_TX)
                dppd_stats_account_fwd_tx();
            else
                dppd_stats_account_fwd_drop();
        }
    }
#endif
}
