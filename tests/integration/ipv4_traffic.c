#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_eth_ring.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include "dppd/runtime.h"
#include "dppd/control.h"

#define PORTS 4U
#define CASES 17U
#define FRAME_SIZE 128U
#define HOT_ROUNDS 1000U

struct fixture {
    struct dppd_runtime runtime;
    struct dppd_control_service control;
    struct dppd_ipv4_router *router;
    struct dppd_ipv4_configuration configuration;
    struct rte_mempool *pool;
    struct rte_ring *rx[PORTS][2], *tx[PORTS][2];
    struct rte_mbuf *retained[2][CASES];
    uint16_t queues;
    atomic_uint external_frees;
    uint64_t sent, forwarded, nd_packets, nd_bytes;
};

static uint32_t ip(const char *text)
{
    struct in_addr value;
    assert(inet_pton(AF_INET, text, &value) == 1);
    return value.s_addr;
}

static uint16_t checksum(const uint8_t *header, uint16_t size)
{
    uint32_t sum = 0;
    for (uint16_t i = 0; i < size; ++i) {
        sum += (uint32_t)header[i] << (i & 1U ? 0 : 8);
        sum = (sum & UINT16_MAX) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static void fill_frame(uint8_t frame[FRAME_SIZE], unsigned int kind, unsigned int tag)
{
    memset(frame, 0, FRAME_SIZE);
    frame[0] = frame[6] = 2;
    uint16_t l2 = kind == 11 ? 18 : 14, l3 = kind == 10 || kind == 11 ? 24 : 20;
    frame[12] = kind == 11 ? 0x81 : 8;
    if (kind == 11) {
        frame[15] = 7;
        frame[16] = 8;
    }
    uint8_t *header = frame + l2;
    header[0] = (uint8_t)(0x40 | l3 / 4);
    header[3] = (uint8_t)(FRAME_SIZE - l2);
    header[8] = kind == 5 ? 1 : 64;
    header[9] = 17;
    uint32_t source = ip("198.18.0.1"), destination = ip("203.0.113.7");
    if (kind == 1 || kind == 11 || kind == 14)
        destination = ip("10.2.0.7");
    else if (kind == 2 || kind == 12 || kind == 15)
        destination = ip("10.3.0.7");
    else if (kind == 3)
        destination = ip("10.3.0.8");
    else if (kind == 4)
        destination = ip("10.4.0.7");
    else if (kind == 9)
        destination = ip("224.0.0.1");
    memcpy(header + 12, &source, 4);
    memcpy(header + 16, &destination, 4);
    if (kind == 10) {
        header[20] = 131;
        header[21] = 4;
    } else if (kind == 11)
        header[20] = 1;
    if (kind == 12)
        header[6] = 0x20;
    header[l3] = 4;
    header[l3 + 1] = 0xd2;
    header[l3 + 2] = 0x16;
    header[l3 + 3] = 0x2e;
    header[l3 + 5] = (uint8_t)(FRAME_SIZE - l2 - l3);
    uint16_t crc = checksum(header, l3);
    header[10] = (uint8_t)(crc >> 8);
    header[11] = (uint8_t)crc;
    if (kind == 6)
        header[10] ^= 1;
    else if (kind == 7) {
        /** IPv6 已可解析，这里改用实验 EtherType 继续验证未知网络协议的原有拒绝路径 */
        frame[12] = 0x88;
        frame[13] = 0xb5;
    } else if (kind == 8)
        header[2] = 0xff;
    memcpy(frame + 100, &tag, sizeof(tag));
}

static void external_free(void *address, void *context)
{
    atomic_uint *count = context;
    atomic_fetch_add_explicit(count, 1, memory_order_relaxed);
    free(address);
}

/** 共享直接包、间接包和外部包均保留原始引用，输出改写后必须仍能读回原内容 */
static struct rte_mbuf *make_packet(struct fixture *fixture, unsigned int kind, unsigned int queue, unsigned int tag)
{
    uint8_t frame[FRAME_SIZE];
    fill_frame(frame, kind, tag);
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(fixture->pool);
    assert(mbuf != NULL);
    if (kind == 16) {
        uint16_t size = 512;
        void *buffer = malloc(size);
        assert(buffer != NULL);
        struct rte_mbuf_ext_shared_info *shared =
            rte_pktmbuf_ext_shinfo_init_helper(buffer, &size, external_free, &fixture->external_frees);
        assert(shared != NULL);
        rte_pktmbuf_attach_extbuf(mbuf, buffer, RTE_BAD_IOVA, size, shared);
    }
    if (kind == 15) {
        struct rte_mbuf *tail = rte_pktmbuf_alloc(fixture->pool);
        assert(tail != NULL);
        memcpy(rte_pktmbuf_append(mbuf, 23), frame, 23);
        memcpy(rte_pktmbuf_append(tail, FRAME_SIZE - 23), frame + 23, FRAME_SIZE - 23);
        assert(rte_pktmbuf_chain(mbuf, tail) == 0);
    } else
        memcpy(rte_pktmbuf_append(mbuf, FRAME_SIZE), frame, FRAME_SIZE);
    if (kind == 13 || kind == 16) {
        rte_mbuf_refcnt_update(mbuf, 1);
        fixture->retained[queue][kind] = mbuf;
    } else if (kind == 14) {
        fixture->retained[queue][kind] = mbuf;
        mbuf = rte_pktmbuf_clone(mbuf, fixture->pool);
        assert(mbuf != NULL);
    }
    return mbuf;
}

static int output_for(unsigned int kind)
{
    if (kind == 0 || kind == 13 || kind == 16)
        return 1;
    if (kind == 1 || kind == 11 || kind == 14)
        return 2;
    if (kind == 2 || kind == 12 || kind == 15)
        return 3;
    return -1;
}

static void verify_output(struct fixture *fixture, struct rte_mbuf *mbuf, uint16_t port, unsigned int kind,
    unsigned int tag, bool hot)
{
    uint8_t expected[FRAME_SIZE], buffer[FRAME_SIZE];
    fill_frame(expected, kind, tag);
    const void *data = rte_pktmbuf_read(mbuf, 0, FRAME_SIZE, buffer);
    assert(data != NULL && rte_pktmbuf_pkt_len(mbuf) == FRAME_SIZE);
    unsigned int last = hot && port == 1 ? 42 : port;
    uint8_t destination[6] = {2, 0, 0, 0, 0, (uint8_t)last};
    memcpy(expected, destination, 6);
    memcpy(expected + 6, fixture->runtime.devices.ports[port].mac.addr_bytes, 6);
    uint16_t l2 = kind == 11 ? 18 : 14, l3 = kind == 11 ? 24 : 20;
    expected[l2 + 8] = 63;
    expected[l2 + 10] = expected[l2 + 11] = 0;
    uint16_t crc = checksum(expected + l2, l3);
    expected[l2 + 10] = (uint8_t)(crc >> 8);
    expected[l2 + 11] = (uint8_t)crc;
    assert(memcmp(data, expected, FRAME_SIZE) == 0);
    assert((mbuf->ol_flags & RTE_MBUF_F_TX_OFFLOAD_MASK) == 0);
}

static void matrix(struct fixture *fixture, uint16_t ingress)
{
    struct dppd_stats_values before, after;
    dppd_runtime_stats_read(&fixture->runtime, &before);
    for (uint16_t queue = 0; queue < fixture->queues; ++queue) {
        for (unsigned int kind = 0; kind < CASES; ++kind) {
            struct rte_mbuf *mbuf = make_packet(fixture, kind, queue, queue * 100 + kind);
            assert(rte_ring_enqueue(fixture->rx[ingress][queue], mbuf) == 0);
            fixture->sent++;
        }
    }
    unsigned int received = 0;
    bool seen[2][CASES] = {{false}};
    uint64_t deadline = rte_get_timer_cycles() + 5 * rte_get_timer_hz();
    for (;;) {
        for (uint16_t port = 0; port < PORTS; ++port) {
            for (uint16_t queue = 0; queue < fixture->queues; ++queue) {
                void *object = NULL;
                while (rte_ring_dequeue(fixture->tx[port][queue], &object) == 0) {
                    struct rte_mbuf *mbuf = object;
                    unsigned int tag;
                    uint8_t scratch[4];
                    const void *value = rte_pktmbuf_read(mbuf, 100, sizeof(tag), scratch);
                    assert(value != NULL);
                    memcpy(&tag, value, sizeof(tag));
                    unsigned int kind = tag % 100;
                    assert(tag / 100 == queue && kind < CASES && !seen[queue][kind] && output_for(kind) == port);
                    seen[queue][kind] = true;
                    verify_output(fixture, mbuf, port, kind, tag, false);
                    rte_pktmbuf_free(mbuf);
                    received++;
                    fixture->forwarded++;
                }
            }
        }
        dppd_runtime_stats_read(&fixture->runtime, &after);
        if (received == 9U * fixture->queues && after.rx_packets == fixture->sent &&
            after.tx_packets == fixture->forwarded &&
            after.policy_drops == before.policy_drops + 7U * fixture->queues &&
            after.no_neighbor_drops == before.no_neighbor_drops + 2U * fixture->queues &&
            after.ipv4_ttl_drops == before.ipv4_ttl_drops + fixture->queues &&
            after.ipv4_checksum_drops == before.ipv4_checksum_drops + fixture->queues &&
            after.ipv4_unsupported_drops == before.ipv4_unsupported_drops + 3U * fixture->queues)
            break;
        assert(rte_get_timer_cycles() < deadline);
        rte_pause();
    }
    assert(after.no_neighbor_drops - before.no_neighbor_drops == 2U * fixture->queues);
    assert(after.ipv4_ttl_drops - before.ipv4_ttl_drops == fixture->queues);
    assert(after.ipv4_checksum_drops - before.ipv4_checksum_drops == fixture->queues);
    assert(after.ipv4_unsupported_drops - before.ipv4_unsupported_drops == 3U * fixture->queues);
    assert(after.rx_malformed - before.rx_malformed == fixture->queues);
    for (uint16_t queue = 0; queue < fixture->queues; ++queue) {
        for (unsigned int kind = 0; kind < CASES; ++kind) {
            struct rte_mbuf *original = fixture->retained[queue][kind];
            if (original == NULL)
                continue;
            uint8_t expected[FRAME_SIZE];
            fill_frame(expected, kind, queue * 100 + kind);
            assert(rte_mbuf_refcnt_read(original) == 1);
            assert(memcmp(rte_pktmbuf_mtod(original, const void *), expected, FRAME_SIZE) == 0);
            rte_pktmbuf_free(original);
            fixture->retained[queue][kind] = NULL;
        }
    }
}

/** 连续换出口及下一跳 MAC，每包允许旧版或新版，但不能出现缺失邻居或混合改写 */
static void hot_updates(struct fixture *fixture)
{
    struct dppd_ipv4_configuration configuration = fixture->configuration;
    for (unsigned int round = 0; round < HOT_ROUNDS; ++round) {
        for (uint16_t queue = 0; queue < fixture->queues; ++queue) {
            struct rte_mbuf *mbuf = make_packet(fixture, 1, queue, round);
            assert(rte_ring_enqueue(fixture->rx[0][queue], mbuf) == 0);
            fixture->sent++;
        }
        bool alternate = (round & 1U) == 0;
        configuration.routes[1].port_id = alternate ? 1 : 2;
        configuration.neighbors[1].port_id = alternate ? 1 : 2;
        configuration.neighbors[1].mac[5] = alternate ? 42 : 2;
        assert(dppd_ipv4_router_replace(fixture->router, configuration.generation, &configuration, NULL, NULL) == 0);
        configuration.generation++;
        unsigned int received = 0;
        uint64_t deadline = rte_get_timer_cycles() + 5 * rte_get_timer_hz();
        while (received < fixture->queues) {
            for (uint16_t port = 0; port < PORTS; ++port) {
                for (uint16_t queue = 0; queue < fixture->queues; ++queue) {
                    void *object = NULL;
                    while (rte_ring_dequeue(fixture->tx[port][queue], &object) == 0) {
                        assert(port == 1 || port == 2);
                        verify_output(fixture, object, port, 1, round, true);
                        rte_pktmbuf_free(object);
                        fixture->forwarded++;
                        received++;
                    }
                }
            }
            assert(rte_get_timer_cycles() < deadline);
            rte_pause();
        }
    }
}

/** 独立检查无路由、出口不可用和软件规则优先级，避免多个丢弃条件互相掩盖 */
static void failure_paths(struct fixture *fixture)
{
    struct dppd_ipv4_configuration saved, empty = {0};
    assert(dppd_ipv4_router_get(fixture->router, &saved) == 0);
    for (unsigned int mode = 0; mode < 3; ++mode) {
        struct dppd_stats_values before, after;
        struct dppd_control_apply_result applied;
        if (mode == 0)
            assert(dppd_ipv4_router_replace(fixture->router, saved.generation, &empty, NULL, NULL) == 0);
        else if (mode == 1)
            atomic_store_explicit(&fixture->runtime.devices.ports[1].link_state, DPPD_LINK_DOWN, memory_order_release);
        else {
            struct dppd_rule rule = {.id = 901,
                .fallback = DPPD_FALLBACK_SOFTWARE_ONLY,
                .nb_matches = 1,
                .nb_actions = 2};
            rule.matches[0].type = DPPD_MATCH_ETH;
            rule.actions[0].type = DPPD_ACTION_COUNT;
            rule.actions[1].type = DPPD_ACTION_DROP;
            assert(dppd_control_apply(&fixture->control, 0, &rule, 0, &applied) == 0);
        }
        dppd_runtime_stats_read(&fixture->runtime, &before);
        struct rte_mbuf *mbuf = make_packet(fixture, mode == 2 ? 5 : 0, 0, mode);
        assert(rte_ring_enqueue(fixture->rx[0][0], mbuf) == 0);
        fixture->sent++;
        uint64_t deadline = rte_get_timer_cycles() + 5 * rte_get_timer_hz();
        for (;;) {
            dppd_runtime_stats_read(&fixture->runtime, &after);
            /* 统计字段独立发布，等待本场景的原因字段也到齐，不把一次中间采样当作整组快照 */
            if (after.rx_packets == fixture->sent && after.policy_drops == before.policy_drops + 1 &&
                after.no_route_drops == before.no_route_drops + (mode == 0 ? 1U : 0U) &&
                after.egress_drops == before.egress_drops + (mode == 1 ? 1U : 0U) &&
                after.rule_drops == before.rule_drops + (mode == 2 ? 1U : 0U))
                break;
            assert(rte_get_timer_cycles() < deadline);
            rte_pause();
        }
        assert(after.tx_packets == before.tx_packets);
        assert(after.no_route_drops - before.no_route_drops == (mode == 0 ? 1U : 0U));
        assert(after.egress_drops - before.egress_drops == (mode == 1 ? 1U : 0U));
        assert(after.rule_drops - before.rule_drops == (mode == 2 ? 1U : 0U));
        assert(after.ipv4_ttl_drops == before.ipv4_ttl_drops);
        if (mode == 0)
            assert(dppd_ipv4_router_replace(fixture->router, saved.generation + 1, &saved, NULL, NULL) == 0);
        else if (mode == 1)
            atomic_store_explicit(&fixture->runtime.devices.ports[1].link_state, DPPD_LINK_UP, memory_order_release);
        else {
            uint64_t hits, bytes, generation;
            bool removed;
            assert(dppd_software_backend_query_count(&fixture->control.software, 901, applied.generation, &hits,
                       &bytes) == 0 &&
                hits == 1 && bytes == FRAME_SIZE);
            assert(
                dppd_control_remove(&fixture->control, 901, applied.generation, &removed, &generation) == 0 && removed);
        }
    }
}

/** 允许规则优先于兜底拒绝，真实出口保留 MARK，共享原包不能被改写或写入新标记 */
static void acl_allow_path(struct fixture *fixture)
{
    struct dppd_rule allow = {.id = 951,
        .fallback = DPPD_FALLBACK_SOFTWARE_ONLY,
        .priority = 10,
        .nb_matches = 3,
        .nb_actions = 3};
    allow.matches[0].type = DPPD_MATCH_ETH;
    allow.matches[1].type = DPPD_MATCH_IPV4;
    allow.matches[2].type = DPPD_MATCH_UDP_RANGE;
    allow.matches[2].spec.range = (struct dppd_match_l4_range){.src_first_be = htons(1000),
        .src_last_be = htons(2000),
        .dst_first_be = htons(5000),
        .dst_last_be = htons(6000)};
    allow.actions[0].type = DPPD_ACTION_MARK;
    allow.actions[0].conf.mark_id = 0xfeed;
    allow.actions[1].type = DPPD_ACTION_COUNT;
    allow.actions[2].type = DPPD_ACTION_ALLOW;
    struct dppd_rule deny = allow;
    deny.id = 950;
    deny.priority = 100;
    deny.nb_matches = 1;
    deny.actions[2].type = DPPD_ACTION_DROP;
    struct dppd_control_apply_result allowed, denied;
    assert(dppd_control_apply(&fixture->control, 0, &deny, 0, &denied) == 0);
    assert(dppd_control_apply(&fixture->control, 0, &allow, 0, &allowed) == 0);
    const unsigned int kinds[] = {0, 13};
    for (uint16_t queue = 0; queue < fixture->queues; ++queue)
        for (unsigned int i = 0; i < 2; ++i) {
            struct rte_mbuf *mbuf = make_packet(fixture, kinds[i], queue, kinds[i]);
            assert(rte_ring_enqueue(fixture->rx[0][queue], mbuf) == 0);
            fixture->sent++;
        }
    uint64_t deadline = rte_get_timer_cycles() + 5 * rte_get_timer_hz();
    for (uint16_t queue = 0; queue < fixture->queues; ++queue) {
        for (unsigned int i = 0; i < 2; ++i) {
            void *object = NULL;
            while (rte_ring_dequeue(fixture->tx[1][queue], &object) != 0) {
                assert(rte_get_timer_cycles() < deadline);
                rte_pause();
            }
            struct rte_mbuf *mbuf = object;
            verify_output(fixture, mbuf, 1, kinds[i], kinds[i], false);
            assert((mbuf->ol_flags & RTE_MBUF_F_RX_FDIR_ID) != 0 && mbuf->hash.fdir.hi == 0xfeed);
            rte_pktmbuf_free(mbuf);
            fixture->forwarded++;
        }
        struct rte_mbuf *original = fixture->retained[queue][13];
        assert(original != NULL && (original->ol_flags & RTE_MBUF_F_RX_FDIR_ID) == 0);
        uint8_t expected[FRAME_SIZE], buffer[FRAME_SIZE];
        fill_frame(expected, 13, 13);
        assert(memcmp(rte_pktmbuf_read(original, 0, FRAME_SIZE, buffer), expected, FRAME_SIZE) == 0);
        rte_pktmbuf_free(original);
        fixture->retained[queue][13] = NULL;
    }
    uint64_t hits, bytes, generation;
    assert(dppd_software_backend_query_count(&fixture->control.software, 951, allowed.generation, &hits, &bytes) == 0);
    assert(hits == 2U * fixture->queues && bytes == hits * FRAME_SIZE);
    assert(dppd_software_backend_query_count(&fixture->control.software, 950, denied.generation, &hits, &bytes) == 0);
    assert(hits == 0 && bytes == 0);
    bool removed;
    assert(dppd_control_remove(&fixture->control, 951, allowed.generation, &removed, &generation) == 0 && removed);
    assert(dppd_control_remove(&fixture->control, 950, denied.generation, &removed, &generation) == 0 && removed);
    struct dppd_stats_values stats;
    do {
        dppd_runtime_stats_read(&fixture->runtime, &stats);
        assert(rte_get_timer_cycles() < deadline);
    } while (stats.rx_packets != fixture->sent || stats.tx_packets != fixture->forwarded ||
        stats.tx_bytes != fixture->forwarded * FRAME_SIZE);
    printf("PASS ACL worker queues=%u allow+range+mark+count COW=unchanged\n", fixture->queues);
}

/** net_ring 没有 RSS，测试明确配置两队列后复用生产 worker，不放宽正式设备能力检查 */
static void configure_queues(struct fixture *fixture)
{
    if (fixture->queues == 1)
        return;
    for (uint16_t i = 0; i < PORTS; ++i) {
        struct dppd_port *port = &fixture->runtime.devices.ports[i];
        struct rte_eth_conf configuration = {0};
        struct rte_eth_dev_info information;
        assert(rte_eth_dev_stop(port->port_id) == 0);
        configuration.txmode.offloads = port->configured_tx_offloads;
        assert(rte_eth_dev_info_get(port->port_id, &information) == 0);
        assert(rte_eth_dev_configure(port->port_id, fixture->queues, fixture->queues, &configuration) == 0);
        for (uint16_t queue = 0; queue < fixture->queues; ++queue) {
            assert(rte_eth_rx_queue_setup(port->port_id, queue, 128, port->socket_id, &information.default_rxconf,
                       fixture->pool) == 0);
            assert(
                rte_eth_tx_queue_setup(port->port_id, queue, 128, port->socket_id, &information.default_txconf) == 0);
        }
        assert(rte_eth_dev_start(port->port_id) == 0);
    }
    unsigned int lcore;
    RTE_LCORE_FOREACH_WORKER (lcore) {
        if (lcore == fixture->runtime.workers[0].lcore_id)
            continue;
        struct dppd_worker *worker = &fixture->runtime.workers[1];
        worker->runtime = &fixture->runtime;
        worker->lcore_id = lcore;
        worker->queue_id = 1;
        atomic_init(&worker->state, DPPD_WORKER_IDLE);
        dppd_stats_init(&worker->stats);
        for (unsigned int port = 0; port < PORTS; ++port)
            dppd_stats_init(&worker->port_stats[port]);
        fixture->runtime.nb_workers = fixture->runtime.config.nb_queues = 2;
        return;
    }
    assert(false);
}

/** 静态服务两向都走正式 worker，共享原包保持不变，跨队列只创建一个会话 */
static void dnat_queues(struct fixture *fixture, const struct dppd_dnat_rule *dnat)
{
    uint64_t deadline = rte_get_timer_cycles() + 5 * rte_get_timer_hz();
    for (unsigned int reply = 0; reply < 2; ++reply)
        for (uint16_t queue = 0; queue < fixture->queues; ++queue) {
            uint8_t wire[FRAME_SIZE], expected[FRAME_SIZE], buffer[FRAME_SIZE];
            fill_frame(wire, 0, 993);
            uint32_t remote = ip("203.0.113.7");
            uint16_t remote_port = htons(5678), public_port = htons(dnat->public_first);
            uint16_t private_port = htons(dnat->private_first);
            memcpy(wire + 26, reply ? &dnat->private_address_be : &remote, 4);
            memcpy(wire + 30, reply ? &remote : &dnat->public_address_be, 4);
            memcpy(wire + 34, reply ? &private_port : &remote_port, 2);
            memcpy(wire + 36, reply ? &remote_port : &public_port, 2);
            wire[24] = wire[25] = 0;
            uint16_t crc = checksum(wire + 14, 20);
            wire[24] = (uint8_t)(crc >> 8);
            wire[25] = (uint8_t)crc;
            memcpy(expected, wire, FRAME_SIZE);
            uint16_t ingress = reply ? 0 : 1, egress = reply ? 1 : 0;
            memcpy(expected, reply ? (uint8_t[]){2, 0, 0, 0, 0, 1} : (uint8_t[]){2, 0, 0, 0, 0, 10}, 6);
            memcpy(expected + 6, fixture->runtime.devices.ports[egress].mac.addr_bytes, 6);
            memcpy(expected + (reply ? 26 : 30), reply ? &dnat->public_address_be : &dnat->private_address_be, 4);
            memcpy(expected + (reply ? 34 : 36), reply ? &public_port : &private_port, 2);
            expected[22]--;
            expected[24] = expected[25] = 0;
            crc = checksum(expected + 14, 20);
            expected[24] = (uint8_t)(crc >> 8);
            expected[25] = (uint8_t)crc;
            struct rte_mbuf *mbuf = rte_pktmbuf_alloc(fixture->pool);
            assert(mbuf != NULL);
            memcpy(rte_pktmbuf_append(mbuf, FRAME_SIZE), wire, FRAME_SIZE);
            rte_mbuf_refcnt_update(mbuf, 1);
            assert(rte_ring_enqueue(fixture->rx[ingress][queue], mbuf) == 0);
            fixture->sent++;
            void *object = NULL;
            while (rte_ring_dequeue(fixture->tx[egress][queue], &object) != 0) {
                assert(rte_get_timer_cycles() < deadline);
                rte_pause();
            }
            assert(memcmp(rte_pktmbuf_read(object, 0, FRAME_SIZE, buffer), expected, FRAME_SIZE) == 0);
            assert(memcmp(rte_pktmbuf_read(mbuf, 0, FRAME_SIZE, buffer), wire, FRAME_SIZE) == 0);
            rte_pktmbuf_free(object);
            rte_pktmbuf_free(mbuf);
            fixture->forwarded++;
        }
    struct dppd_connection_page page;
    assert(dppd_conntrack_list(fixture->runtime.conntrack, dppd_monotonic_ms(), 0, 0, &page) == 0 && page.count == 2);
    assert(page.entries[1].dnat_rule_id == 1 && page.entries[1].seen_reply &&
        page.entries[1].packets[0] == fixture->queues && page.entries[1].packets[1] == fixture->queues);
    printf("PASS DNAT worker queues=%u shared-session=1 original-unchanged=yes\n", fixture->queues);
}

/** 同一流从不同 RX 队列进出也必须共用映射，回包恢复内网地址，共享原包保持不变 */
static void stateful_queues(struct fixture *fixture)
{
    dppd_runtime_request_stop(&fixture->runtime);
    assert(dppd_runtime_wait(&fixture->runtime) == 0);
    struct dppd_ipv4_configuration routes;
    assert(dppd_ipv4_router_get(fixture->router, &routes) == 0);
    routes.routes[routes.route_count++] = (struct dppd_ipv4_route){.prefix_be = ip("198.18.0.0"),
        .depth = 16,
        .port_id = 0,
        .gateway_be = ip("192.0.2.10")};
    routes.neighbors[routes.neighbor_count++] =
        (struct dppd_ipv4_neighbor){.port_id = 0, .address_be = ip("192.0.2.10"), .mac = {2, 0, 0, 0, 0, 10}};
    assert(dppd_ipv4_router_replace(fixture->router, routes.generation, &routes, NULL, NULL) == 0);
    struct dppd_snat_config nat = {.enabled = true,
        .inside_port = 0,
        .outside_port = 1,
        .address_be = ip("192.0.2.250"),
        .first_port = 40000,
        .last_port = 40007};
    struct dppd_dnat_rule dnat = {.outside_port = 1,
        .inside_port = 0,
        .protocol = 17,
        .public_address_be = nat.address_be,
        .private_address_be = ip("198.18.0.1"),
        .public_first = 40000,
        .public_last = 40000,
        .private_first = 8000};
    assert(dppd_conntrack_create(&fixture->runtime.conntrack, 32, 60, &nat, &dnat, 1) == 0);
    fixture->control.conntrack = fixture->runtime.conntrack;
    assert(dppd_runtime_start(&fixture->runtime) == 0);
    uint16_t mapped = 0;
    uint64_t deadline = rte_get_timer_cycles() + 5 * rte_get_timer_hz();
    for (uint16_t queue = 0; queue < fixture->queues; ++queue) {
        struct rte_mbuf *mbuf = make_packet(fixture, 13, queue, 991);
        assert(rte_ring_enqueue(fixture->rx[0][queue], mbuf) == 0);
        fixture->sent++;
        void *object = NULL;
        while (rte_ring_dequeue(fixture->tx[1][queue], &object) != 0) {
            assert(rte_get_timer_cycles() < deadline);
            rte_pause();
        }
        uint8_t expected[FRAME_SIZE], buffer[FRAME_SIZE];
        fill_frame(expected, 13, 991);
        const uint8_t *output = rte_pktmbuf_read(object, 0, FRAME_SIZE, buffer);
        assert(output != NULL);
        uint16_t observed;
        memcpy(&observed, output + 34, 2);
        assert(ntohs(observed) >= 40000 && ntohs(observed) <= 40007);
        if (queue == 0)
            mapped = observed;
        assert(observed == mapped);
        memcpy(expected, (uint8_t[]){2, 0, 0, 0, 0, 1}, 6);
        memcpy(expected + 6, fixture->runtime.devices.ports[1].mac.addr_bytes, 6);
        memcpy(expected + 26, &nat.address_be, 4);
        memcpy(expected + 34, &mapped, 2);
        expected[22]--;
        expected[24] = expected[25] = 0;
        uint16_t crc = checksum(expected + 14, 20);
        expected[24] = (uint8_t)(crc >> 8);
        expected[25] = (uint8_t)crc;
        assert(memcmp(output, expected, FRAME_SIZE) == 0);
        rte_pktmbuf_free(object);
        fixture->forwarded++;
        struct rte_mbuf *retained = fixture->retained[queue][13];
        fill_frame(expected, 13, 991);
        assert(memcmp(rte_pktmbuf_read(retained, 0, FRAME_SIZE, buffer), expected, FRAME_SIZE) == 0);
        rte_pktmbuf_free(retained);
        fixture->retained[queue][13] = NULL;
    }
    for (uint16_t queue = 0; queue < fixture->queues; ++queue) {
        uint8_t wire[FRAME_SIZE], expected[FRAME_SIZE], buffer[FRAME_SIZE];
        fill_frame(wire, 0, 992);
        uint32_t remote = ip("203.0.113.7"), internal = ip("198.18.0.1");
        memcpy(wire + 26, &remote, 4);
        memcpy(wire + 30, &nat.address_be, 4);
        uint16_t remote_port = htons(5678), internal_port = htons(1234);
        memcpy(wire + 34, &remote_port, 2);
        memcpy(wire + 36, &mapped, 2);
        wire[24] = wire[25] = 0;
        uint16_t crc = checksum(wire + 14, 20);
        wire[24] = (uint8_t)(crc >> 8);
        wire[25] = (uint8_t)crc;
        memcpy(expected, wire, FRAME_SIZE);
        memcpy(expected, (uint8_t[]){2, 0, 0, 0, 0, 10}, 6);
        memcpy(expected + 6, fixture->runtime.devices.ports[0].mac.addr_bytes, 6);
        memcpy(expected + 30, &internal, 4);
        memcpy(expected + 36, &internal_port, 2);
        expected[22]--;
        expected[24] = expected[25] = 0;
        crc = checksum(expected + 14, 20);
        expected[24] = (uint8_t)(crc >> 8);
        expected[25] = (uint8_t)crc;
        struct rte_mbuf *mbuf = rte_pktmbuf_alloc(fixture->pool);
        assert(mbuf != NULL);
        memcpy(rte_pktmbuf_append(mbuf, FRAME_SIZE), wire, FRAME_SIZE);
        assert(rte_ring_enqueue(fixture->rx[1][queue], mbuf) == 0);
        fixture->sent++;
        void *object = NULL;
        while (rte_ring_dequeue(fixture->tx[0][queue], &object) != 0) {
            assert(rte_get_timer_cycles() < deadline);
            rte_pause();
        }
        assert(memcmp(rte_pktmbuf_read(object, 0, FRAME_SIZE, buffer), expected, FRAME_SIZE) == 0);
        rte_pktmbuf_free(object);
        fixture->forwarded++;
    }
    struct dppd_connection_page page;
    assert(dppd_conntrack_list(fixture->runtime.conntrack, dppd_monotonic_ms(), 0, 0, &page) == 0 && page.count == 1);
    assert(page.entries[0].seen_reply && page.entries[0].packets[0] == fixture->queues &&
        page.entries[0].packets[1] == fixture->queues);
    printf("PASS conntrack/NAT worker queues=%u shared-session=1 original-unchanged=yes\n", fixture->queues);
    dnat_queues(fixture, &dnat);
}

/** 在 IPv4 NAT 已运行时加入 IPv6，验证跨队列、联合快照更新、分段共享包及 MARK/COUNT */
static void ipv6_queues(struct fixture *fixture)
{
    struct dppd_ipv4_configuration configuration;
    assert(dppd_ipv4_router_get(fixture->router, &configuration) == 0);
    configuration.route6_count = 1;
    configuration.neighbor6_count = 2;
    struct dppd_ipv6_route *route = &configuration.routes6[0];
    memset(route, 0, sizeof(*route));
    assert(inet_pton(AF_INET6, "2001:db8:2::", route->prefix) == 1);
    assert(inet_pton(AF_INET6, "fe80::1", route->gateway) == 1);
    route->depth = 64;
    for (unsigned int i = 0; i < 2; ++i) {
        struct dppd_ipv6_neighbor *neighbor = &configuration.neighbors6[i];
        memset(neighbor, 0, sizeof(*neighbor));
        memcpy(neighbor->address, route->gateway, 16);
        neighbor->port_id = (uint16_t)(i + 1);
        neighbor->mac[0] = 2;
        neighbor->mac[5] = (uint8_t)(i + 1);
    }
    struct dppd_rule allow = {.id = 990, .fallback = DPPD_FALLBACK_SOFTWARE_ONLY, .nb_matches = 4, .nb_actions = 3};
    allow.matches[0].type = DPPD_MATCH_ETH;
    allow.matches[1].type = DPPD_MATCH_IPV6;
    memcpy(allow.matches[1].spec.ipv6.dst, route->prefix, 16);
    memset(allow.matches[1].spec.ipv6.dst_mask, 255, 8);
    allow.matches[2].type = DPPD_MATCH_UDP_RANGE;
    allow.matches[2].spec.range = (struct dppd_match_l4_range){.src_first_be = htons(1000),
        .src_last_be = htons(2000),
        .dst_first_be = htons(5000),
        .dst_last_be = htons(6000)};
    allow.matches[3].type = DPPD_MATCH_CONNECTION_STATE;
    allow.matches[3].spec.connection_states = DPPD_CT_NEW;
    allow.actions[0] = (struct dppd_action){.type = DPPD_ACTION_MARK, .conf.mark_id = 0x600d};
    allow.actions[1].type = DPPD_ACTION_COUNT;
    allow.actions[2].type = DPPD_ACTION_ALLOW;
    struct dppd_control_apply_result applied;
    assert(dppd_control_apply(&fixture->control, 0, &allow, 0, &applied) == 0);
    for (unsigned int round = 0; round < 200; ++round) {
        uint16_t egress = (uint16_t)(1 + round % 2), queue = (uint16_t)(round % fixture->queues);
        route->port_id = egress;
        assert(dppd_ipv4_router_replace(fixture->router, configuration.generation, &configuration, NULL, NULL) == 0);
        configuration.generation++;
        uint8_t wire[FRAME_SIZE] = {0}, expected[FRAME_SIZE], buffer[FRAME_SIZE], pseudo[FRAME_SIZE + 40] = {0};
        unsigned int l2 = round % 2 == 0 ? 14 : 18;
        wire[0] = wire[6] = 2;
        if (l2 == 18) {
            wire[12] = 0x81;
            wire[15] = 7;
        }
        wire[l2 - 2] = 0x86;
        wire[l2 - 1] = 0xdd;
        uint8_t *ip = wire + l2, *udp = ip + 40;
        ip[0] = 0x6a;
        ip[3] = 0x55;
        ip[5] = (uint8_t)(FRAME_SIZE - l2 - 40);
        ip[6] = 17;
        ip[7] = 64;
        assert(inet_pton(AF_INET6, "2001:db8:1::1", ip + 8) == 1);
        assert(inet_pton(AF_INET6, "2001:db8:2::7", ip + 24) == 1);
        memcpy(udp, (uint8_t[]){4, 0xd2, 0x16, 0x2e, 0, (uint8_t)(FRAME_SIZE - l2 - 40), 0, 0}, 8);
        /** 两个出口各用一个五元组，已建立的会话不能随路由切换偷偷换出口 */
        udp[1] = (uint8_t)(0xd2 + egress);
        memcpy(pseudo, ip + 8, 32);
        pseudo[35] = ip[5];
        pseudo[39] = 17;
        memcpy(pseudo + 40, udp, ip[5]);
        uint16_t crc = checksum(pseudo, (uint16_t)(40 + ip[5]));
        udp[6] = (uint8_t)(crc >> 8);
        udp[7] = (uint8_t)crc;
        memcpy(expected, wire, FRAME_SIZE);
        memcpy(expected, configuration.neighbors6[egress - 1].mac, 6);
        memcpy(expected + 6, fixture->runtime.devices.ports[egress].mac.addr_bytes, 6);
        expected[l2 + 7]--;
        struct rte_mbuf *original = rte_pktmbuf_alloc(fixture->pool), *tail = rte_pktmbuf_alloc(fixture->pool);
        assert(original != NULL && tail != NULL);
        memcpy(rte_pktmbuf_append(original, 33), wire, 33);
        memcpy(rte_pktmbuf_append(tail, FRAME_SIZE - 33), wire + 33, FRAME_SIZE - 33);
        assert(rte_pktmbuf_chain(original, tail) == 0);
        rte_mbuf_refcnt_update(original, 1);
        rte_mbuf_refcnt_update(tail, 1);
        assert(rte_ring_enqueue(fixture->rx[0][queue], original) == 0);
        fixture->sent++;
        void *object = NULL;
        uint64_t deadline = rte_get_timer_cycles() + 5 * rte_get_timer_hz();
        while (rte_ring_dequeue(fixture->tx[egress][queue], &object) != 0) {
            assert(rte_get_timer_cycles() < deadline);
            rte_pause();
        }
        /** 成功出队还要取得有效报文，不能把未填写的输出交给后面的字节核对 */
        assert(object != NULL);
        assert(memcmp(rte_pktmbuf_read(object, 0, FRAME_SIZE, buffer), expected, FRAME_SIZE) == 0);
        assert(((struct rte_mbuf *)object)->hash.fdir.hi == 0x600d);
        assert(memcmp(rte_pktmbuf_read(original, 0, FRAME_SIZE, buffer), wire, FRAME_SIZE) == 0);
        assert((original->ol_flags & RTE_MBUF_F_RX_FDIR_ID) == 0);
        rte_pktmbuf_free(object);
        rte_pktmbuf_free(original);
        fixture->forwarded++;
    }
    uint64_t hits, bytes;
    assert(dppd_software_backend_query_count(&fixture->control.software, 990, applied.generation, &hits, &bytes) == 0);
    assert(hits == 200 && bytes == 200 * FRAME_SIZE);
    struct dppd_conntrack_status status;
    dppd_conntrack_status(fixture->runtime.conntrack, dppd_monotonic_ms(), &status);
    assert(status.active == 4);
    printf("PASS IPv6 worker queues=%u updates=200 MARK+COUNT=yes shared-chain=unchanged IPv4-NAT=isolated\n",
        fixture->queues);
}

/** 用完整伪首部独立组包，保留 VLAN 和两个填充扩展头，不能只核对转换后的地址 */
static void nat66_frame(uint8_t bytes[FRAME_SIZE], const char *source, const char *destination, uint16_t source_port,
    uint16_t destination_port)
{
    memset(bytes, 0, FRAME_SIZE);
    bytes[0] = bytes[6] = 2;
    memcpy(bytes + 12, (uint8_t[]){0x81, 0, 0, 37, 0x86, 0xdd}, 6);
    uint8_t *ip6 = bytes + 18, *udp = ip6 + 56;
    ip6[0] = 0x6a;
    ip6[3] = 0x55;
    ip6[5] = FRAME_SIZE - 18 - 40;
    ip6[6] = 0;
    ip6[7] = 64;
    assert(inet_pton(AF_INET6, source, ip6 + 8) == 1);
    assert(inet_pton(AF_INET6, destination, ip6 + 24) == 1);
    ip6[40] = 60;
    ip6[48] = 17;
    uint16_t port = htons(source_port);
    memcpy(udp, &port, 2);
    port = htons(destination_port);
    memcpy(udp + 2, &port, 2);
    udp[5] = FRAME_SIZE - 18 - 56;
    for (unsigned int i = 8; i < udp[5]; ++i)
        udp[i] = (uint8_t)(i * 7);
    uint8_t pseudo[FRAME_SIZE + 40] = {0};
    memcpy(pseudo, ip6 + 8, 32);
    pseudo[35] = udp[5];
    pseudo[39] = 17;
    memcpy(pseudo + 40, udp, udp[5]);
    uint16_t value = checksum(pseudo, (uint16_t)(40 + udp[5]));
    if (value == 0)
        value = UINT16_MAX;
    udp[6] = (uint8_t)(value >> 8);
    udp[7] = (uint8_t)value;
}

/** 正式 worker 的两个队列共用 NAT66 映射，SNAT 与 DNAT 双向均不得污染共享分段原包 */
static void nat66_queues(struct fixture *fixture)
{
    dppd_runtime_request_stop(&fixture->runtime);
    assert(dppd_runtime_wait(&fixture->runtime) == 0);
    dppd_conntrack_destroy(fixture->runtime.conntrack);
    fixture->runtime.conntrack = NULL;
    struct dppd_snat_config nat =
        {.enabled = true, .ipv6 = true, .inside_port = 0, .outside_port = 1, .first_port = 40000, .last_port = 40001};
    assert(inet_pton(AF_INET6, "2001:db8:100::9", nat.address6) == 1);
    struct dppd_dnat_rule dnat = {.ipv6 = true,
        .inside_port = 0,
        .outside_port = 1,
        .protocol = 17,
        .public_first = 40000,
        .public_last = 40000,
        .private_first = 8000};
    memcpy(dnat.public_address6, nat.address6, 16);
    assert(inet_pton(AF_INET6, "2001:db8:1::8", dnat.private_address6) == 1);
    assert(dppd_conntrack_create_dual(&fixture->runtime.conntrack, 16, 60, NULL, &nat, &dnat, 1) == 0);
    fixture->control.conntrack = fixture->runtime.conntrack;
    struct dppd_ipv4_configuration routes;
    assert(dppd_ipv4_router_get(fixture->router, &routes) == 0);
    routes.routes6[0].port_id = 1;
    routes.routes6[1] = routes.routes6[0];
    assert(inet_pton(AF_INET6, "2001:db8:1::", routes.routes6[1].prefix) == 1);
    routes.routes6[1].port_id = 0;
    routes.route6_count = 2;
    routes.neighbors6[2] = routes.neighbors6[0];
    routes.neighbors6[2].port_id = 0;
    routes.neighbors6[2].mac[5] = 10;
    routes.neighbor6_count = 3;
    assert(dppd_ipv4_router_replace(fixture->router, routes.generation, &routes, NULL, NULL) == 0);
    uint64_t generations[2];
    for (uint16_t port = 0; port < 2; ++port) {
        struct dppd_rule rule = {.id = (uint64_t)(991 + port),
            .priority = 10,
            .fallback = DPPD_FALLBACK_SOFTWARE_ONLY,
            .nb_matches = 3,
            .nb_actions = 3};
        rule.matches[0].type = DPPD_MATCH_ETH;
        rule.matches[1].type = DPPD_MATCH_IPV6;
        rule.matches[2].type = DPPD_MATCH_CONNECTION_STATE;
        rule.matches[2].spec.connection_states = DPPD_CT_NEW | DPPD_CT_ESTABLISHED;
        rule.actions[0] = (struct dppd_action){.type = DPPD_ACTION_MARK, .conf.mark_id = 0x6600};
        rule.actions[1].type = DPPD_ACTION_COUNT;
        rule.actions[2].type = DPPD_ACTION_ALLOW;
        struct dppd_control_apply_result result;
        assert(dppd_control_apply(&fixture->control, port, &rule, 0, &result) == 0);
        generations[port] = result.generation;
    }
    assert(dppd_runtime_start(&fixture->runtime) == 0);
    for (unsigned int destination_nat = 0; destination_nat < 2; ++destination_nat)
        for (unsigned int reply = 0; reply < 2; ++reply)
            for (uint16_t queue = 0; queue < fixture->queues; ++queue) {
                uint16_t ingress = (uint16_t)(destination_nat ? !reply : reply), egress = (uint16_t)!ingress;
                const char *inside = destination_nat ? "2001:db8:1::8" : "2001:db8:1::1";
                const char *remote = "2001:db8:2::7", *public = "2001:db8:100::9";
                uint8_t wire[FRAME_SIZE], expected[FRAME_SIZE], buffer[FRAME_SIZE];
                if (!destination_nat) {
                    nat66_frame(wire, reply ? remote : inside, reply ? public : remote, reply ? 443 : 1234,
                        reply ? 40001 : 443);
                    nat66_frame(expected, reply ? remote : public, reply ? inside : remote, reply ? 443 : 40001,
                        reply ? 1234 : 443);
                } else {
                    nat66_frame(wire, reply ? inside : remote, reply ? remote : public, reply ? 8000 : 1234,
                        reply ? 1234 : 40000);
                    nat66_frame(expected, reply ? public : remote, reply ? remote : inside, reply ? 40000 : 1234,
                        reply ? 1234 : 8000);
                }
                memcpy(expected, routes.neighbors6[egress == 0 ? 2 : 0].mac, 6);
                memcpy(expected + 6, fixture->runtime.devices.ports[egress].mac.addr_bytes, 6);
                expected[25]--;
                struct rte_mbuf *original = rte_pktmbuf_alloc(fixture->pool), *tail = rte_pktmbuf_alloc(fixture->pool);
                assert(original != NULL && tail != NULL);
                memcpy(rte_pktmbuf_append(original, 33), wire, 33);
                memcpy(rte_pktmbuf_append(tail, FRAME_SIZE - 33), wire + 33, FRAME_SIZE - 33);
                assert(rte_pktmbuf_chain(original, tail) == 0);
                rte_mbuf_refcnt_update(original, 1);
                rte_mbuf_refcnt_update(tail, 1);
                assert(rte_ring_enqueue(fixture->rx[ingress][queue], original) == 0);
                fixture->sent++;
                void *object = NULL;
                uint64_t deadline = rte_get_timer_cycles() + 5 * rte_get_timer_hz();
                while (rte_ring_dequeue(fixture->tx[egress][queue], &object) != 0) {
                    assert(rte_get_timer_cycles() < deadline);
                    rte_pause();
                }
                assert(memcmp(rte_pktmbuf_read(object, 0, FRAME_SIZE, buffer), expected, FRAME_SIZE) == 0);
                assert(((struct rte_mbuf *)object)->hash.fdir.hi == 0x6600);
                assert(memcmp(rte_pktmbuf_read(original, 0, FRAME_SIZE, buffer), wire, FRAME_SIZE) == 0);
                assert((original->ol_flags & RTE_MBUF_F_RX_FDIR_ID) == 0);
                rte_pktmbuf_free(object);
                rte_pktmbuf_free(original);
                fixture->forwarded++;
            }
    for (uint16_t port = 0; port < 2; ++port) {
        uint64_t hits, bytes;
        assert(dppd_software_backend_query_count(&fixture->control.software, 991 + port, generations[port], &hits,
                   &bytes) == 0);
        assert(hits == 2U * fixture->queues && bytes == hits * FRAME_SIZE);
    }
    struct dppd_connection_page page;
    assert(dppd_conntrack_list(fixture->runtime.conntrack, dppd_monotonic_ms(), 0, 0, &page) == 0 && page.count == 2);
    for (unsigned int i = 0; i < 2; ++i)
        assert(page.entries[i].original.ipv6 && page.entries[i].seen_reply &&
            page.entries[i].packets[0] == fixture->queues && page.entries[i].packets[1] == fixture->queues);
    printf("PASS NAT66 worker queues=%u SNAT+DNAT shared-session=2 MARK+COUNT=yes shared-chain=unchanged\n",
        fixture->queues);
}

static int prime_dad(void *context, uint16_t port, const uint8_t *wire, uint16_t length)
{
    (void)context;
    (void)port;
    (void)wire;
    (void)length;
    return 0;
}

/** 先保存共享首包，再从另一 RX 队列注入 NA，原始 mbuf 和两条队列的统计都不能被重复处理 */
static void nd6_queues(struct fixture *fixture)
{
    dppd_runtime_request_stop(&fixture->runtime);
    assert(dppd_runtime_wait(&fixture->runtime) == 0);
    struct dppd_ipv4_configuration routes;
    assert(dppd_ipv4_router_get(fixture->router, &routes) == 0);
    routes.neighbor6_count = 0;
    assert(dppd_ipv4_router_replace(fixture->router, routes.generation, &routes, NULL, NULL) == 0);
    struct dppd_config configuration = fixture->runtime.config;
    strcpy(configuration.routing_path, "/private/nd-test");
    configuration.nd6_count = 2;
    uint8_t macs[DPPD_MAX_PORTS][6] = {{0}};
    for (uint16_t i = 0; i < PORTS; ++i)
        memcpy(macs[i], fixture->runtime.devices.ports[i].mac.addr_bytes, 6);
    for (uint16_t i = 0; i < 2; ++i) {
        configuration.nd6[i].port_id = i;
        configuration.nd6[i].vlan_depth = 1;
        configuration.nd6[i].vlan[0] = 37;
        assert(inet_pton(AF_INET6, i == 0 ? "2001:db8:1::fe" : "2001:db8:2::fe", configuration.nd6[i].address) == 1);
    }
    uint64_t now = dppd_monotonic_ms();
    assert(now > 3000 &&
        dppd_nd6_create(&fixture->runtime.nd6, &configuration, (const uint8_t(*)[6])macs, now - 3000) == 0);
    fixture->control.nd6 = fixture->runtime.nd6;
    dppd_nd6_tick(fixture->runtime.nd6, now - 1000, prime_dad, NULL);
    dppd_nd6_tick(fixture->runtime.nd6, now, prime_dad, NULL);
    assert(dppd_nd6_port_ready(fixture->runtime.nd6, 0) && dppd_nd6_port_ready(fixture->runtime.nd6, 1));
    assert(dppd_runtime_start(&fixture->runtime) == 0);
    struct rte_mbuf *retained[2];
    uint8_t wire[FRAME_SIZE], expected[FRAME_SIZE], buffer[FRAME_SIZE];
    nat66_frame(wire, "2001:db8:1::1", "2001:db8:2::7", 1234, 443);
    for (uint16_t queue = 0; queue < fixture->queues; ++queue) {
        retained[queue] = rte_pktmbuf_alloc(fixture->pool);
        struct rte_mbuf *tail = rte_pktmbuf_alloc(fixture->pool);
        assert(retained[queue] != NULL && tail != NULL);
        memcpy(rte_pktmbuf_append(retained[queue], 33), wire, 33);
        memcpy(rte_pktmbuf_append(tail, FRAME_SIZE - 33), wire + 33, FRAME_SIZE - 33);
        assert(rte_pktmbuf_chain(retained[queue], tail) == 0);
        rte_mbuf_refcnt_update(retained[queue], 1);
        rte_mbuf_refcnt_update(tail, 1);
        assert(rte_ring_enqueue(fixture->rx[0][queue], retained[queue]) == 0);
        fixture->sent++;
    }
    void *object = NULL;
    uint64_t deadline = rte_get_timer_cycles() + 5 * rte_get_timer_hz();
    while (rte_ring_dequeue(fixture->tx[1][0], &object) != 0) {
        assert(rte_get_timer_cycles() < deadline);
        rte_pause();
    }
    struct dppd_packet parsed;
    assert(dppd_packet_parse_mbuf(object, &parsed) == 0 && parsed.ip_protocol == 58);
    const uint8_t *ns = rte_pktmbuf_read(object, 0, rte_pktmbuf_pkt_len((struct rte_mbuf *)object), buffer);
    assert(ns != NULL && ns[58] == 135 && ns[25] == 255 && ns[0] == 0x33);
    fixture->nd_packets++;
    fixture->nd_bytes += rte_pktmbuf_pkt_len((struct rte_mbuf *)object);
    rte_pktmbuf_free(object);
    struct dppd_nd6_status status;
    do {
        dppd_nd6_status(fixture->runtime.nd6, &status);
        assert(rte_get_timer_cycles() < deadline);
        rte_pause();
    } while (status.pending != fixture->queues);
    uint8_t na[90] = {0}, pseudo[72] = {0};
    memcpy(na, macs[1], 6);
    memcpy(na + 6, (uint8_t[]){2, 0, 0, 0, 0, 1}, 6);
    memcpy(na + 12, (uint8_t[]){0x81, 0, 0, 37, 0x86, 0xdd}, 6);
    uint8_t *ip6 = na + 18, *icmp = ip6 + 40;
    ip6[0] = 0x60;
    ip6[5] = 32;
    ip6[6] = 58;
    ip6[7] = 255;
    assert(inet_pton(AF_INET6, "fe80::1", ip6 + 8) == 1);
    memcpy(ip6 + 24, configuration.nd6[1].address, 16);
    icmp[0] = 136;
    icmp[4] = 0x60;
    memcpy(icmp + 8, ip6 + 8, 16);
    memcpy(icmp + 24, (uint8_t[]){2, 1, 2, 0, 0, 0, 0, 1}, 8);
    memcpy(pseudo, ip6 + 8, 32);
    pseudo[35] = 32;
    pseudo[39] = 58;
    memcpy(pseudo + 40, icmp, 32);
    uint16_t crc = checksum(pseudo, sizeof(pseudo));
    icmp[2] = (uint8_t)(crc >> 8);
    icmp[3] = (uint8_t)crc;
    struct rte_mbuf *advertisement = rte_pktmbuf_alloc(fixture->pool);
    assert(advertisement != NULL);
    memcpy(rte_pktmbuf_append(advertisement, sizeof(na)), na, sizeof(na));
    assert(rte_ring_enqueue(fixture->rx[1][fixture->queues - 1], advertisement) == 0);
    fixture->sent++;
    nat66_frame(expected, "2001:db8:100::9", "2001:db8:2::7", 40001, 443);
    memcpy(expected, (uint8_t[]){2, 0, 0, 0, 0, 1}, 6);
    memcpy(expected + 6, macs[1], 6);
    expected[25]--;
    for (uint16_t queue = 0; queue < fixture->queues; ++queue) {
        while (rte_ring_dequeue(fixture->tx[1][queue], &object) != 0) {
            assert(rte_get_timer_cycles() < deadline);
            rte_pause();
        }
        assert(memcmp(rte_pktmbuf_read(object, 0, FRAME_SIZE, buffer), expected, FRAME_SIZE) == 0);
        assert(memcmp(rte_pktmbuf_read(retained[queue], 0, FRAME_SIZE, buffer), wire, FRAME_SIZE) == 0);
        assert((retained[queue]->ol_flags & RTE_MBUF_F_RX_FDIR_ID) == 0);
        rte_pktmbuf_free(object);
        rte_pktmbuf_free(retained[queue]);
        fixture->forwarded++;
    }
    dppd_nd6_status(fixture->runtime.nd6, &status);
    assert(status.active == 1 && status.pending == 0 && status.resolved == 1);
    printf("PASS ND worker queues=%u held-first-packet=yes cross-queue-NA=yes NAT66=yes shared-chain=unchanged\n",
        fixture->queues);
}

static void run_case(uint16_t queues)
{
    struct fixture *fixture = calloc(1, sizeof(*fixture));
    assert(fixture != NULL);
    fixture->queues = queues;
    atomic_init(&fixture->external_frees, 0);
    struct dppd_config configuration;
    dppd_config_defaults(&configuration);
    configuration.nb_ports = PORTS;
    configuration.nb_queues = 1;
    configuration.mbufs_per_socket = 4095;
    configuration.mbuf_cache = 0;
    for (uint16_t port = 0; port < PORTS; ++port) {
        char name[48];
        for (uint16_t queue = 0; queue < queues; ++queue) {
            snprintf(name, sizeof(name), "ipv4_rx_%u_%u_%u", queues, port, queue);
            fixture->rx[port][queue] = rte_ring_create(name, 256, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
            snprintf(name, sizeof(name), "ipv4_tx_%u_%u_%u", queues, port, queue);
            fixture->tx[port][queue] = rte_ring_create(name, 256, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
            assert(fixture->rx[port][queue] != NULL && fixture->tx[port][queue] != NULL);
        }
        snprintf(name, sizeof(name), "ipv4_port_%u_%u", queues, port);
        int id = rte_eth_from_rings(name, fixture->rx[port], queues, fixture->tx[port], queues, rte_socket_id());
        assert(id >= 0 && id == port);
        configuration.ports[port] = (uint16_t)id;
    }
    assert(dppd_runtime_init(&fixture->runtime, &configuration) == 0);
    fixture->pool = fixture->runtime.devices.pools[fixture->runtime.devices.ports[0].socket_id];
    configure_queues(fixture);
    unsigned int available = rte_mempool_avail_count(fixture->pool);
    for (uint16_t port = 0; port < PORTS; ++port) {
        uint8_t source[6] = {2, 0, 0, 0, 0x10, (uint8_t)port};
        memcpy(fixture->runtime.devices.ports[port].mac.addr_bytes, source, 6);
    }
    struct dppd_ipv4_configuration *view = &fixture->configuration;
    view->generation = 1;
    view->route_count = 4;
    view->neighbor_count = 3;
    view->routes[0] = (struct dppd_ipv4_route){.port_id = 1, .gateway_be = ip("192.0.2.1")};
    view->routes[1] =
        (struct dppd_ipv4_route){.prefix_be = ip("10.2.0.0"), .depth = 16, .port_id = 2, .gateway_be = ip("192.0.2.2")};
    view->routes[2] = (struct dppd_ipv4_route){.prefix_be = ip("10.3.0.0"), .depth = 16, .port_id = 3};
    view->routes[3] =
        (struct dppd_ipv4_route){.prefix_be = ip("10.4.0.0"), .depth = 16, .port_id = 3, .gateway_be = ip("192.0.2.4")};
    view->neighbors[0] =
        (struct dppd_ipv4_neighbor){.port_id = 1, .address_be = ip("192.0.2.1"), .mac = {2, 0, 0, 0, 0, 1}};
    view->neighbors[1] =
        (struct dppd_ipv4_neighbor){.port_id = 2, .address_be = ip("192.0.2.2"), .mac = {2, 0, 0, 0, 0, 2}};
    view->neighbors[2] =
        (struct dppd_ipv4_neighbor){.port_id = 3, .address_be = ip("10.3.0.7"), .mac = {2, 0, 0, 0, 0, 3}};
    assert(dppd_ipv4_router_create(&fixture->router, configuration.ports, PORTS, view) == 0);
    assert(dppd_control_init(&fixture->control, &fixture->runtime.devices.topology, 16, NULL) == 0);
    dppd_runtime_set_software_backend(&fixture->runtime, &fixture->control.software);
    dppd_runtime_set_ipv4_router(&fixture->runtime, fixture->router);
    assert(dppd_runtime_start(&fixture->runtime) == 0);
    matrix(fixture, 0);
    matrix(fixture, 3);
    hot_updates(fixture);
    acl_allow_path(fixture);
    failure_paths(fixture);
    stateful_queues(fixture);
    ipv6_queues(fixture);
    nat66_queues(fixture);
    nd6_queues(fixture);
    dppd_runtime_request_stop(&fixture->runtime);
    assert(dppd_runtime_wait(&fixture->runtime) == 0);
    struct dppd_stats_values statistics;
    dppd_runtime_stats_read(&fixture->runtime, &statistics);
    assert(statistics.rx_packets == fixture->sent && statistics.tx_packets == fixture->forwarded + fixture->nd_packets);
    assert(statistics.tx_bytes == fixture->forwarded * FRAME_SIZE + fixture->nd_bytes);
    struct dppd_stats_values ports = {0};
    for (uint16_t port = 0; port < PORTS; ++port) {
        struct dppd_stats_values values;
        assert(dppd_runtime_stats_query(&fixture->runtime, port, DPPD_STATS_ALL, &values) == 0);
        dppd_stats_accumulate(&ports, &values);
    }
    assert(memcmp(&ports, &statistics, sizeof(ports)) == 0);
    assert(atomic_load_explicit(&fixture->external_frees, memory_order_relaxed) == 2U * queues);
    assert(rte_mempool_avail_count(fixture->pool) == available);
    assert(dppd_control_fini(&fixture->control) == 0);
    assert(dppd_ipv4_router_destroy(fixture->router) == 0);
    assert(dppd_runtime_destroy(&fixture->runtime) == 0);
    for (uint16_t port = 0; port < PORTS; ++port)
        for (uint16_t queue = 0; queue < queues; ++queue) {
            assert(rte_ring_empty(fixture->rx[port][queue]) && rte_ring_empty(fixture->tx[port][queue]));
            rte_ring_free(fixture->rx[port][queue]);
            rte_ring_free(fixture->tx[port][queue]);
        }
    free(fixture);
    printf("PASS IPv4 worker ports=4 queues=%u shapes=17 hot-updates=1000 COW=direct+indirect+external leaks=0\n",
        queues);
}

int main(int argc, char **argv)
{
    assert(rte_eal_init(argc, argv) >= 0);
    run_case(1);
    if (rte_lcore_count() >= 3)
        run_case(2);
    assert(rte_eal_cleanup() == 0);
    return 0;
}
