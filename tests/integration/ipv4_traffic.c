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
    uint64_t sent, forwarded;
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
        frame[12] = 0x86;
        frame[13] = 0xdd;
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
                void *object;
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
                    void *object;
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
            void *object;
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
    assert(dppd_conntrack_create(&fixture->runtime.conntrack, 32, 60, &nat) == 0);
    fixture->control.conntrack = fixture->runtime.conntrack;
    assert(dppd_runtime_start(&fixture->runtime) == 0);
    uint16_t mapped = 0;
    uint64_t deadline = rte_get_timer_cycles() + 5 * rte_get_timer_hz();
    for (uint16_t queue = 0; queue < fixture->queues; ++queue) {
        struct rte_mbuf *mbuf = make_packet(fixture, 13, queue, 991);
        assert(rte_ring_enqueue(fixture->rx[0][queue], mbuf) == 0);
        fixture->sent++;
        void *object;
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
        void *object;
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
    dppd_runtime_request_stop(&fixture->runtime);
    assert(dppd_runtime_wait(&fixture->runtime) == 0);
    struct dppd_stats_values statistics;
    dppd_runtime_stats_read(&fixture->runtime, &statistics);
    assert(statistics.rx_packets == fixture->sent && statistics.tx_packets == fixture->forwarded);
    assert(statistics.tx_bytes == fixture->forwarded * FRAME_SIZE);
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
