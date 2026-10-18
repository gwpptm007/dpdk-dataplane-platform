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
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include "dppd/flow_state.h"
#include "dppd/runtime.h"

struct fixture {
    struct dppd_runtime runtime;
    struct dppd_flow_template_id drop, allow;
    struct dppd_flow_store *store;
    struct rte_ring *rx[2], *tx[2];
    struct rte_mempool *pool;
    uint16_t ports[2];
    uint64_t sent, forwarded, dropped;
};

static uint64_t now_ns(void)
{
    return dppd_monotonic_ms() * UINT64_C(1000000);
}

static struct dppd_async_flow_create_request request(struct fixture *fixture, bool allow, uint64_t id)
{
    struct dppd_async_flow_create_request value = {.table = allow ? fixture->allow : fixture->drop};
    value.rule = (struct dppd_rule){.id = id,
        .install_port_id = fixture->ports[0],
        .fallback = DPPD_FALLBACK_SOFTWARE_ONLY,
        .nb_matches = 3,
        .nb_actions = allow ? 3 : 2};
    value.rule.matches[0].type = DPPD_MATCH_ETH;
    value.rule.matches[1].type = DPPD_MATCH_IPV4;
    value.rule.matches[2].type = DPPD_MATCH_UDP;
    value.rule.matches[2].spec.l4.dst_be = rte_cpu_to_be_16(allow ? 9001 : 9000);
    value.rule.matches[2].spec.l4.dst_mask_be = UINT16_MAX;
    if (allow)
        value.rule.actions[0] = (struct dppd_action){.type = DPPD_ACTION_MARK, .conf.mark_id = 42};
    value.rule.actions[allow ? 1 : 0].type = DPPD_ACTION_COUNT;
    value.rule.actions[allow ? 2 : 1].type = allow ? DPPD_ACTION_ALLOW : DPPD_ACTION_DROP;
    return value;
}

/** 从真实 RX ring 进入正式 worker，转发逐字节核对，DROP 通过完整收发统计和缓冲池回收核对 */
static void traffic(struct fixture *fixture, uint16_t destination, uint32_t count, bool drop, bool mark)
{
    uint8_t expected[64] = {0};
    expected[12] = 8;
    expected[14] = 0x45;
    expected[17] = 50;
    expected[22] = 64;
    expected[23] = 17;
    expected[34] = 0x04;
    expected[35] = 0xd2;
    expected[36] = (uint8_t)(destination >> 8);
    expected[37] = (uint8_t)destination;
    expected[39] = 30;
    for (uint32_t i = 0; i < count; ++i) {
        struct rte_mbuf *mbuf = rte_pktmbuf_alloc(fixture->pool);
        assert(mbuf != NULL);
        void *bytes = rte_pktmbuf_append(mbuf, 64);
        assert(bytes != NULL);
        memcpy(bytes, expected, sizeof(expected));
        assert(rte_ring_enqueue(fixture->rx[0], mbuf) == 0);
    }
    fixture->sent += count;
    fixture->dropped += drop ? count : 0;
    uint64_t expected_forwarded = fixture->forwarded + (drop ? 0 : count);
    uint64_t deadline = rte_get_timer_cycles() + 5U * rte_get_timer_hz();
    for (;;) {
        void *object = NULL;
        while (rte_ring_dequeue(fixture->tx[1], &object) == 0) {
            struct rte_mbuf *mbuf = object;
            assert(!drop && mbuf->pkt_len == sizeof(expected));
            assert(memcmp(rte_pktmbuf_mtod(mbuf, void *), expected, sizeof(expected)) == 0);
            assert(((mbuf->ol_flags & RTE_MBUF_F_RX_FDIR_ID) != 0) == mark);
            if (mark)
                assert(mbuf->hash.fdir.hi == 42);
            ++fixture->forwarded;
            rte_pktmbuf_free(mbuf);
        }
        struct dppd_stats_values stats;
        dppd_runtime_stats_read(&fixture->runtime, &stats);
        if (stats.rx_packets == fixture->sent && stats.tx_packets == expected_forwarded &&
            stats.policy_drops == fixture->dropped && stats.rx_bytes == fixture->sent * 64U &&
            stats.tx_bytes == expected_forwarded * 64U && fixture->forwarded == expected_forwarded)
            break;
        assert(rte_get_timer_cycles() < deadline);
        usleep(100);
    }
}

static struct dppd_flow_job_result finish(struct fixture *fixture, struct dppd_flow_job_id job)
{
    uint64_t deadline = rte_get_timer_cycles() + 5U * rte_get_timer_hz();
    struct dppd_flow_job_result result;
    for (;;) {
        assert(dppd_flow_store_process(fixture->store, now_ns(), 64) == 0);
        int rc = dppd_flow_store_collect(fixture->store, &job, &result);
        if (rc == 0)
            return result;
        assert(rc == -EAGAIN && rte_get_timer_cycles() < deadline);
        usleep(100);
    }
}

static void initialize(const char *path, struct fixture *fixture)
{
    struct dppd_flow_state *state = calloc(1, sizeof(*state));
    assert(state != NULL);
    uint16_t port = fixture->ports[0];
    state->config = (struct dppd_flow_state_config){.mode = DPPD_FLOW_STATE_SOFTWARE,
        .port_id = port,
        .rule_capacity = 64,
        .queue_count = 1,
        .depth = 64,
        .queue_capacity = {128},
        .template_leases = 256,
        .shared_leases = 1,
        .timeout_ns = UINT64_C(5000000000)};
    state->config.admission = (struct dppd_admission_config){.ticket_capacity = 320, .port_count = 1};
    state->config.admission.ports[0].port_id = port;
    for (unsigned int k = 0; k < DPPD_RESOURCE_KIND_COUNT; ++k)
        state->config.admission.limits[k] = state->config.admission.ports[0].limits[k] = 128;
    state->table_count = 2;
    state->shared_next_generation = 1;
    state->revision = 47;
    state->rule_count = 2;
    state->rules = calloc(2, sizeof(*state->rules));
    assert(state->rules != NULL);
    for (unsigned int i = 0; i < 2; ++i) {
        state->tables[i].key = (i + 1U) * 100U;
        state->tables[i].spec.capacity = 128;
        state->tables[i].spec.example = request(fixture, i != 0, i + 1U).rule;
        state->rules[i] = (struct dppd_flow_state_rule){state->tables[i].key, 0, state->tables[i].spec.example};
        state->rules[i].rule.generation = i == 0 ? 11 : 29;
    }
    assert(dppd_flow_state_initialize(path, state) == 0);
    dppd_flow_state_free(&state);
}

int main(int argc, char **argv)
{
    assert(argc >= 3);
    const char *path = argv[argc - 2], *stage = argv[argc - 1];
    bool before = strcmp(stage, "before") == 0;
    assert(before || strcmp(stage, "after") == 0);
    assert(rte_eal_init(argc, argv) >= 0);
    struct fixture fixture = {0};
    struct dppd_config config;
    dppd_config_defaults(&config);
    config.nb_ports = 2;
    config.nb_queues = 1;
    config.burst_size = 32;
    config.mbufs_per_socket = 2048;
    config.mbuf_cache = 0;
    config.rule_capacity = 64;
    config.promiscuous = false;
    for (unsigned int i = 0; i < 2; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "m5_rx_%u", i);
        fixture.rx[i] = rte_ring_create(name, 1024, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
        snprintf(name, sizeof(name), "m5_tx_%u", i);
        fixture.tx[i] = rte_ring_create(name, 1024, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
        assert(fixture.rx[i] != NULL && fixture.tx[i] != NULL);
        snprintf(name, sizeof(name), "m5_port_%u", i);
        int port = rte_eth_from_rings(name, &fixture.rx[i], 1, &fixture.tx[i], 1, rte_socket_id());
        assert(port >= 0);
        fixture.ports[i] = config.ports[i] = (uint16_t)port;
    }
    assert(dppd_runtime_init(&fixture.runtime, &config) == 0);
    fixture.pool = fixture.runtime.devices.pools[fixture.runtime.devices.ports[0].socket_id];
    unsigned int available = rte_mempool_avail_count(fixture.pool);
    if (before)
        initialize(path, &fixture);
    assert(dppd_flow_store_open(path, NULL, &fixture.store) == 0);
    struct dppd_software_backend *software = dppd_flow_store_software(fixture.store);
    assert(software != NULL);
    uint64_t hits, bytes;
    assert(dppd_software_backend_query_count(software, 1, before ? 11 : 48, &hits, &bytes) == 0 && hits == 0 &&
        bytes == 0);
    dppd_runtime_set_software_backend(&fixture.runtime, software);
    assert(dppd_runtime_start(&fixture.runtime) == 0);
    if (before) {
        traffic(&fixture, 9000, 16, true, false);
        traffic(&fixture, 9001, 16, false, true);
        struct dppd_flow_state_rule updates[2] = {{200, 0, request(&fixture, true, 1).rule},
            {100, 0, request(&fixture, false, 2).rule}};
        updates[0].rule.matches[2].spec.l4.dst_be = rte_cpu_to_be_16(9000);
        updates[1].rule.matches[2].spec.l4.dst_be = rte_cpu_to_be_16(9001);
        uint64_t expected[2] = {11, 29};
        struct dppd_flow_job_id job;
        assert(dppd_flow_store_update(fixture.store, 0, updates, expected, 2, now_ns(), &job) == 0);
        struct dppd_flow_job_result result = finish(&fixture, job);
        assert(result.applied && result.generations[0] == 48 && result.generations[1] == 49);
    }
    traffic(&fixture, 9000, 16, false, true);
    traffic(&fixture, 9001, 16, true, false);
    uint64_t deadline = rte_get_timer_cycles() + 5U * rte_get_timer_hz();
    do {
        assert(dppd_software_backend_query_count(software, 1, 48, &hits, &bytes) == 0);
        assert(rte_get_timer_cycles() < deadline);
    } while (hits != 16 || bytes != 1024);
    assert(rte_mempool_avail_count(fixture.pool) == available);
    if (before) {
        /** 故意跳过服务、worker 和 EAL 的退出清理，下一进程必须只凭磁盘配置重新安装 */
        puts("PASS M5 before abrupt-exit rx=64 forwarded=32 dropped=32 durable-revision=49 COUNT=16/1024 mbuf-leaks=0");
        fflush(stdout);
        _exit(0);
    }
    struct dppd_rule restored;
    assert(dppd_flow_store_get(fixture.store, 1, &restored) == 0 && restored.generation == 48);
    dppd_runtime_request_stop(&fixture.runtime);
    assert(dppd_runtime_wait(&fixture.runtime) == 0);
    assert(dppd_flow_store_close(&fixture.store) == 0);
    struct dppd_flow_state *state = NULL;
    assert(dppd_flow_state_load(path, &state) == 0 && state->rule_count == 2 && state->revision == 49);
    dppd_flow_state_free(&state);
    for (unsigned int i = 0; i < 2; ++i) {
        rte_ring_free(fixture.rx[i]);
        rte_ring_free(fixture.tx[i]);
    }
    assert(rte_eal_cleanup() == 0);
    puts(
        "PASS M5 after fresh-process rx=32 forwarded=16 dropped=16 durable-revision=49 COUNT-reset=0 COUNT=16/1024 mbuf-leaks=0");
    return 0;
}
