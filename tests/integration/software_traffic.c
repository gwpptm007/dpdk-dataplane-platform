#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_eth_ring.h>
#include <rte_mbuf.h>
#include "dppd/control.h"
#include "dppd/runtime.h"
#include "dppd/management.h"

#define PACKETS_PER_PORT 24U
#define ROUNDS 1000U

static _Thread_local unsigned int fail_allocation;
void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size);

void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_allocation != 0 && --fail_allocation == 0)
        return NULL;
    return __real_calloc(count, size);
}

struct fixture {
    struct dppd_runtime runtime;
    struct dppd_control_service control;
    struct dppd_control_batch_update_request requests[4];
    struct rte_ring *rx[2], *tx[2];
    struct rte_mempool *pool;
    uint32_t count;
    uint64_t sent, received, forwarded;
};

static struct rte_mbuf *make_packet(struct fixture *fixture, uint16_t destination,
                                    uint16_t ingress)
{
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(fixture->pool);
    uint8_t *data;

    assert(mbuf != NULL);
    data = (uint8_t *)rte_pktmbuf_append(mbuf, 64);
    assert(data != NULL);
    memset(data, 0, 64);
    data[12] = 0x08;
    data[14] = 0x45;
    data[17] = 50;
    data[22] = 64;
    data[23] = 17;
    data[34] = 0x04;
    data[35] = 0xd2;
    data[36] = (uint8_t)(destination >> 8);
    data[37] = (uint8_t)destination;
    data[39] = 30;
    data[42] = (uint8_t)ingress;
    return mbuf;
}

static void enqueue_round(struct fixture *fixture)
{
    uint32_t port, i;

    for (port = 0; port < fixture->count / 2; ++port) {
        for (i = 0; i < PACKETS_PER_PORT; ++i) {
            struct rte_mbuf *mbuf = make_packet(fixture, (uint16_t)(8000 + i % 3 * 1000),
                                                (uint16_t)port);
            assert(rte_ring_enqueue(fixture->rx[port], mbuf) == 0);
            fixture->sent++;
        }
    }
}

static void drain_round(struct fixture *fixture)
{
    const uint64_t deadline = rte_get_timer_cycles() + 5 * rte_get_timer_hz();
    const uint64_t expected_forwarded = fixture->sent / 3;
    struct dppd_stats_values stats;

    for (;;) {
        uint32_t port;

        for (port = 0; port < 2; ++port) {
            void *object;

            while (rte_ring_dequeue(fixture->tx[port], &object) == 0) {
                struct rte_mbuf *mbuf = object;
                struct dppd_packet packet;
                const uint8_t *data = rte_pktmbuf_mtod(mbuf, const uint8_t *);

                assert(dppd_packet_parse_mbuf(mbuf, &packet) == DPPD_PARSE_OK);
                assert(packet.l4_dst_port_be == rte_cpu_to_be_16(10000));
                assert(packet.packet_len == 64 && data[42] == 1 - port);
                assert((mbuf->ol_flags & RTE_MBUF_F_RX_FDIR_ID) == 0);
                fixture->forwarded++;
                rte_pktmbuf_free(mbuf);
            }
        }
        dppd_runtime_stats_read(&fixture->runtime, &stats);
        if (fixture->forwarded == expected_forwarded && stats.rx_packets == fixture->sent &&
            stats.tx_packets == expected_forwarded &&
            stats.policy_drops == fixture->sent - expected_forwarded)
            break;
        assert(fixture->forwarded <= expected_forwarded);
        assert(rte_get_timer_cycles() < deadline);
        rte_pause();
    }
    assert(stats.rx_bytes == fixture->sent * 64);
    assert(stats.tx_bytes == fixture->forwarded * 64);
    assert(stats.rx_malformed == 0 && stats.rx_unsupported == 0 && stats.tx_drops == 0);
    fixture->received = stats.rx_packets;
}

static void toggle(struct fixture *fixture)
{
    uint32_t i;

    for (i = 0; i < fixture->count; ++i) {
        uint16_t *destination = &fixture->requests[i].rule.matches[2].spec.l4.dst_be;
        *destination = *destination == rte_cpu_to_be_16(8000) ?
            rte_cpu_to_be_16(9000) : rte_cpu_to_be_16(8000);
    }
}

static void run_updates(struct fixture *fixture)
{
    struct dppd_control_apply_result results[4];
    uint32_t i, round;

    for (round = 0; round < ROUNDS; ++round) {
        enqueue_round(fixture);
        toggle(fixture);
        assert(dppd_control_update_batch(&fixture->control, fixture->requests,
                                          fixture->count, results) == 0);
        for (i = 0; i < fixture->count; ++i)
            fixture->requests[i].expected_generation = results[i].generation;
        drain_round(fixture);
    }

    for (round = 1; round <= fixture->count + 2; ++round) {
        uint64_t before[4], hits, bytes;
        uint64_t revision = fixture->control.rules.generation;

        for (i = 0; i < fixture->count; ++i)
            assert(dppd_software_backend_query_count(&fixture->control.software,
                fixture->requests[i].rule.id, fixture->requests[i].expected_generation,
                &before[i], &bytes) == 0);
        enqueue_round(fixture);
        toggle(fixture);
        fail_allocation = round;
        assert(dppd_control_update_batch(&fixture->control, fixture->requests,
                                          fixture->count, results) == -ENOMEM);
        assert(fail_allocation == 0 && fixture->control.rules.generation == revision);
        toggle(fixture);
        drain_round(fixture);
        for (i = 0; i < fixture->count; ++i) {
            assert(dppd_software_backend_query_count(&fixture->control.software,
                fixture->requests[i].rule.id, fixture->requests[i].expected_generation,
                &hits, &bytes) == 0);
            assert(hits == before[i] + PACKETS_PER_PORT / 3);
            assert(bytes == hits * 64);
        }
    }
    toggle(fixture);
    assert(dppd_control_update_batch(&fixture->control, fixture->requests,
                                      fixture->count, results) == 0);
    for (i = 0; i < fixture->count; ++i) {
        uint64_t hits, bytes;

        fixture->requests[i].expected_generation = results[i].generation;
        assert(dppd_software_backend_query_count(&fixture->control.software,
            fixture->requests[i].rule.id, results[i].generation, &hits, &bytes) == 0);
        assert(hits == 0 && bytes == 0);
    }
    enqueue_round(fixture);
    drain_round(fixture);
    for (i = 0; i < fixture->count; ++i) {
        uint64_t hits, bytes;

        assert(dppd_software_backend_query_count(&fixture->control.software,
            fixture->requests[i].rule.id, results[i].generation, &hits, &bytes) == 0);
        assert(hits == PACKETS_PER_PORT / 3 && bytes == hits * 64);
    }
}

static void verify_stats(struct fixture *fixture)
{
    struct dppd_stats_values total, sum = {0}, queue;
    uint64_t per_port_rx = fixture->received / (fixture->count / 2);

    for (uint16_t i = 0; i < 2; ++i) {
        struct dppd_stats_values port;
        struct dppd_management_request request = {0};
        struct dppd_management_response response;
        uint16_t id = fixture->runtime.devices.ports[i].port_id;
        uint64_t expected_rx = i < fixture->count / 2 ? per_port_rx : 0;
        uint64_t expected_tx = 1U - i < fixture->count / 2 ? per_port_rx / 3 : 0;

        assert(dppd_runtime_stats_query(&fixture->runtime, id, 0, &port) == 0);
        assert(port.rx_packets == expected_rx && port.tx_packets == expected_tx);
        assert(port.rx_bytes == expected_rx * 64 && port.tx_bytes == expected_tx * 64);
        assert(port.policy_drops == expected_rx * 2 / 3 && port.rule_drops == port.policy_drops);
        assert(port.no_route_drops == 0 && port.egress_drops == 0 && port.tx_drops == 0);
        request.version = DPPD_MANAGEMENT_VERSION;
        request.size = sizeof(request);
        request.operation = DPPD_MANAGEMENT_STATS_QUERY;
        request.payload.stats_query.port_id = id;
        request.payload.stats_query.queue_id = 0;
        assert(dppd_management_handle(&fixture->control, &fixture->runtime.devices,
                                      &fixture->runtime, &request, &response) == 0);
        assert(response.status == 0 && memcmp(&response.payload.stats, &port, sizeof(port)) == 0);
        dppd_stats_accumulate(&sum, &port);
    }
    dppd_runtime_stats_read(&fixture->runtime, &total);
    assert(memcmp(&sum, &total, sizeof(total)) == 0);
    assert(dppd_runtime_stats_query(&fixture->runtime, DPPD_STATS_ALL, 0, &queue) == 0);
    assert(memcmp(&queue, &total, sizeof(total)) == 0);
}

static void verify_drop_reasons(struct fixture *fixture)
{
    for (uint16_t scenario = 0; scenario < 4; ++scenario) {
        struct dppd_stats_values before, after, ingress, egress;
        struct dppd_forwarding_snapshot snapshot = fixture->runtime.snapshot;
        uint16_t peer = fixture->runtime.devices.ports[0].peer_port_id;
        struct rte_mbuf *mbuf = make_packet(fixture, 10000, 0);
        const uint64_t deadline = rte_get_timer_cycles() + 5 * rte_get_timer_hz();

        dppd_runtime_stats_read(&fixture->runtime, &before);
        assert(dppd_runtime_stats_query(&fixture->runtime,
            fixture->runtime.devices.ports[0].port_id, 0, &ingress) == 0);
        assert(dppd_runtime_stats_query(&fixture->runtime,
            fixture->runtime.devices.ports[1].port_id, 0, &egress) == 0);
        if (scenario == 0)
            assert(rte_pktmbuf_trim(mbuf, 54) == 0);
        else if (scenario == 1)
            fixture->runtime.snapshot.nb_peers = 0;
        else if (scenario == 2)
            fixture->runtime.devices.ports[0].peer_port_id = UINT16_MAX;
        else {
            unsigned int slots = rte_ring_free_count(fixture->tx[1]);
            for (unsigned int i = 0; i < slots; ++i) {
                struct rte_mbuf *filler = make_packet(fixture, 10000, 0);
                assert(rte_ring_enqueue(fixture->tx[1], filler) == 0);
            }
        }
        assert(rte_ring_enqueue(fixture->rx[0], mbuf) == 0);
        assert(dppd_runtime_start(&fixture->runtime) == 0);
        do {
            dppd_runtime_stats_read(&fixture->runtime, &after);
            assert(rte_get_timer_cycles() < deadline);
        } while (after.rx_packets != before.rx_packets + 1);
        dppd_runtime_request_stop(&fixture->runtime);
        assert(dppd_runtime_wait(&fixture->runtime) == 0);
        dppd_runtime_stats_read(&fixture->runtime, &after);
        assert(after.tx_packets == before.tx_packets);
        assert(after.rx_malformed - before.rx_malformed == (scenario == 0 ? 1U : 0U));
        assert(after.no_route_drops - before.no_route_drops == (scenario == 1 ? 1U : 0U));
        assert(after.egress_drops - before.egress_drops == (scenario == 2 ? 1U : 0U));
        assert(after.tx_queue_drops - before.tx_queue_drops == (scenario == 3 ? 1U : 0U));
        {
            struct dppd_stats_values current;
            assert(dppd_runtime_stats_query(&fixture->runtime,
                fixture->runtime.devices.ports[0].port_id, 0, &current) == 0);
            assert(current.rx_packets == ingress.rx_packets + 1);
            assert(current.tx_drops == ingress.tx_drops);
            assert(dppd_runtime_stats_query(&fixture->runtime,
                fixture->runtime.devices.ports[1].port_id, 0, &current) == 0);
            assert(current.tx_queue_drops == egress.tx_queue_drops + (scenario == 3 ? 1U : 0U));
        }
        fixture->runtime.snapshot = snapshot;
        fixture->runtime.devices.ports[0].peer_port_id = peer;
        if (scenario == 3) {
            void *object;
            while (rte_ring_dequeue(fixture->tx[1], &object) == 0)
                rte_pktmbuf_free(object);
        }
    }
}

static void run_case(uint32_t count)
{
    struct fixture fixture = {0};
    struct dppd_config config;
    uint32_t i;
    unsigned int available;

    fixture.count = count;
    dppd_config_defaults(&config);
    config.nb_ports = 2;
    config.nb_queues = 1;
    config.burst_size = 32;
    config.mbufs_per_socket = 2048;
    config.mbuf_cache = 0;
    config.rule_capacity = count;
    config.promiscuous = false;
    for (i = 0; i < 2; ++i) {
        char name[32];
        int port;

        snprintf(name, sizeof(name), "traffic_rx_%u_%u", count, i);
        fixture.rx[i] = rte_ring_create(name, 1024, rte_socket_id(),
                                         RING_F_SP_ENQ | RING_F_SC_DEQ);
        snprintf(name, sizeof(name), "traffic_tx_%u_%u", count, i);
        fixture.tx[i] = rte_ring_create(name, 1024, rte_socket_id(),
                                         RING_F_SP_ENQ | RING_F_SC_DEQ);
        assert(fixture.rx[i] != NULL && fixture.tx[i] != NULL);
        snprintf(name, sizeof(name), "traffic_port_%u_%u", count, i);
        port = rte_eth_from_rings(name, &fixture.rx[i], 1, &fixture.tx[i], 1,
                                   rte_socket_id());
        assert(port >= 0);
        config.ports[i] = (uint16_t)port;
    }
    assert(dppd_runtime_init(&fixture.runtime, &config) == 0);
    fixture.pool = fixture.runtime.devices.pools[fixture.runtime.devices.ports[0].socket_id];
    available = rte_mempool_avail_count(fixture.pool);
    assert(dppd_control_init(&fixture.control, &fixture.runtime.devices.topology, count, NULL) == 0);
    for (i = 0; i < count; ++i) {
        struct dppd_control_apply_result result;
        struct dppd_control_batch_update_request *request = &fixture.requests[i];

        request->rule.id = 100 + i;
        request->rule.fallback = DPPD_FALLBACK_SOFTWARE_ONLY;
        request->rule.nb_matches = 3;
        request->rule.matches[0].type = DPPD_MATCH_ETH;
        request->rule.matches[1].type = DPPD_MATCH_IPV4;
        request->rule.matches[2].type = DPPD_MATCH_UDP;
        request->rule.matches[2].spec.l4.dst_be = rte_cpu_to_be_16(8000 + i % 2 * 1000);
        request->rule.matches[2].spec.l4.dst_mask_be = UINT16_MAX;
        request->rule.nb_actions = 2;
        request->rule.actions[0].type = DPPD_ACTION_COUNT;
        request->rule.actions[1].type = DPPD_ACTION_DROP;
        request->install_port_id = config.ports[i / 2];
        assert(dppd_control_apply(&fixture.control, request->install_port_id,
                                  &request->rule, 0, &result) == 0);
        request->expected_generation = result.generation;
    }
    dppd_runtime_set_software_backend(&fixture.runtime, &fixture.control.software);
    assert(dppd_runtime_start(&fixture.runtime) == 0);
    run_updates(&fixture);
    dppd_runtime_request_stop(&fixture.runtime);
    assert(dppd_runtime_wait(&fixture.runtime) == 0);
    {
        char directory[] = "/tmp/dppd-traffic-replay-XXXXXX";
        char path[128];
        const uint64_t generation = fixture.control.rules.generation;

        assert(mkdtemp(directory) != NULL);
        snprintf(path, sizeof(path), "%s/state.bin", directory);
        assert(dppd_control_persistence_attach(&fixture.control, path) == 0);
        assert(dppd_control_persistence_flush(&fixture.control) == 0);
        assert(dppd_control_fini(&fixture.control) == 0);
        assert(dppd_control_init(&fixture.control, &fixture.runtime.devices.topology,
                                  count, NULL) == 0);
        assert(dppd_control_persistence_restore(&fixture.control, path) == 0);
        assert(fixture.control.rules.generation == generation);
        for (i = 0; i < count; ++i) {
            struct dppd_rule stored;
            struct dppd_rule expected = fixture.requests[i].rule;
            uint64_t hits, bytes;

            assert(dppd_rule_repository_get(&fixture.control.rules,
                fixture.requests[i].rule.id, &stored) == 0);
            assert(stored.generation == fixture.requests[i].expected_generation);
            assert(stored.install_port_id == fixture.requests[i].install_port_id);
            expected.install_port_id = fixture.requests[i].install_port_id;
            assert(dppd_rule_equal(&stored, &expected));
            assert(dppd_software_backend_query_count(&fixture.control.software,
                stored.id, stored.generation, &hits, &bytes) == 0);
            assert(hits == 0 && bytes == 0);
        }
        dppd_runtime_set_software_backend(&fixture.runtime, &fixture.control.software);
        assert(dppd_runtime_start(&fixture.runtime) == 0);
        enqueue_round(&fixture);
        drain_round(&fixture);
        dppd_runtime_request_stop(&fixture.runtime);
        assert(dppd_runtime_wait(&fixture.runtime) == 0);
        for (i = 0; i < count; ++i) {
            uint64_t hits, bytes;

            assert(dppd_software_backend_query_count(&fixture.control.software,
                fixture.requests[i].rule.id, fixture.requests[i].expected_generation,
                &hits, &bytes) == 0);
            assert(hits == PACKETS_PER_PORT / 3 && bytes == hits * 64);
        }
        verify_stats(&fixture);
        verify_drop_reasons(&fixture);
        assert(dppd_control_fini(&fixture.control) == 0);
        assert(unlink(path) == 0 && rmdir(directory) == 0);
    }
    assert(rte_mempool_avail_count(fixture.pool) == available);
    for (i = 0; i < 2; ++i)
        assert(rte_ring_empty(fixture.rx[i]) && rte_ring_empty(fixture.tx[i]));
    dppd_runtime_destroy(&fixture.runtime);
    for (i = 0; i < 2; ++i) {
        rte_ring_free(fixture.rx[i]);
        rte_ring_free(fixture.tx[i]);
    }
    printf("PASS rules=%u updates=%u rx=%" PRIu64 " forwarded=%" PRIu64
           " dropped=%" PRIu64 " allocation-failures=%u replay=passed stats=passed"
           " drop-reasons=passed mbuf-leaks=0\n",
           count, ROUNDS + 1, fixture.received, fixture.forwarded,
           fixture.received - fixture.forwarded, count + 2);
}

int main(int argc, char **argv)
{
    assert(rte_eal_init(argc, argv) >= 0);
    run_case(2);
    run_case(4);
    assert(rte_eal_cleanup() == 0);
    return 0;
}
