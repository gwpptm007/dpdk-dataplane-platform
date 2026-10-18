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
#include "dppd/flow_service.h"
#include "dppd/runtime.h"

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
    struct dppd_admission *admission;
    struct dppd_flow_templates *templates;
    struct dppd_flow_template_id drop, allow;
    struct dppd_software_backend software;
    struct dppd_software_async_flows *driver;
    struct dppd_async_flow_queue *queue;
    struct dppd_flow_service *service;
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

static struct dppd_flow_job_result finish(struct fixture *fixture, const struct dppd_flow_job_id *job)
{
    struct dppd_flow_job_result result;
    uint64_t deadline = rte_get_timer_cycles() + 5U * rte_get_timer_hz();
    for (;;) {
        assert(dppd_flow_service_process(fixture->service, now_ns(), 64) == 0);
        int rc = dppd_flow_service_collect(fixture->service, job, &result);
        if (rc == 0)
            return result;
        assert(rc == -EAGAIN && rte_get_timer_cycles() < deadline);
    }
}

int main(int argc, char **argv)
{
    struct fixture fixture = {0};
    struct dppd_config config;
    struct dppd_admission_config quota = {.ticket_capacity = 256, .port_count = 1};
    struct dppd_async_flow_driver driver;
    struct dppd_flow_template_spec shape = {.capacity = 64};
    struct dppd_flow_job_id job;
    struct dppd_async_flow_create_request requests[2];
    struct dppd_flow_remove_request removals[2];
    assert(rte_eal_init(argc, argv) >= 0);
    assert(rte_lcore_count() >= 2);
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
        snprintf(name, sizeof(name), "m4_rx_%u", i);
        fixture.rx[i] = rte_ring_create(name, 1024, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
        snprintf(name, sizeof(name), "m4_tx_%u", i);
        fixture.tx[i] = rte_ring_create(name, 1024, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
        assert(fixture.rx[i] != NULL && fixture.tx[i] != NULL);
        snprintf(name, sizeof(name), "m4_port_%u", i);
        int port = rte_eth_from_rings(name, &fixture.rx[i], 1, &fixture.tx[i], 1, rte_socket_id());
        assert(port >= 0);
        fixture.ports[i] = config.ports[i] = (uint16_t)port;
    }
    assert(dppd_runtime_init(&fixture.runtime, &config) == 0);
    fixture.pool = fixture.runtime.devices.pools[fixture.runtime.devices.ports[0].socket_id];
    unsigned int available = rte_mempool_avail_count(fixture.pool);
    /** 在实际 PMD 上验证明确拒绝，不给 net_ring 伪造原生模板或异步卸载能力 */
    int native_result = dppd_native_flow_configure(fixture.ports[0], 1, 64, 64, 0);
    assert(native_result == -ENOTSUP || native_result == -ENOSYS);
    quota.ports[0].port_id = fixture.ports[0];
    for (unsigned int k = 0; k < DPPD_RESOURCE_KIND_COUNT; ++k)
        quota.limits[k] = quota.ports[0].limits[k] = 128;
    assert(dppd_admission_create(&quota, &fixture.admission) == 0);
    assert(dppd_flow_templates_create(fixture.admission, 256, NULL, &fixture.templates) == 0);
    shape.example = request(&fixture, false, 1).rule;
    assert(dppd_flow_template_create(fixture.templates, &shape, &fixture.drop) == 0);
    shape.example = request(&fixture, true, 2).rule;
    assert(dppd_flow_template_create(fixture.templates, &shape, &fixture.allow) == 0);
    {
        struct dppd_flow_templates *native = NULL;
        struct dppd_flow_template_driver native_driver = dppd_native_flow_template_driver();
        struct dppd_flow_template_id unchanged = {.owner = 999};
        shape.example = request(&fixture, false, 77).rule;
        shape.example.fallback = DPPD_FALLBACK_REQUIRE_HARDWARE;
        assert(dppd_flow_templates_create(fixture.admission, 64, &native_driver, &native) == 0);
        int rc = dppd_flow_template_create(native, &shape, &unchanged);
        assert((rc == -ENOTSUP || rc == -ENOSYS) && unchanged.owner == 999);
        assert(dppd_flow_templates_close(&native) == 0);
    }
    assert(dppd_software_backend_init(&fixture.software, 64) == 0);
    assert(dppd_software_async_flows_create(&fixture.driver, &driver) == 0);
    struct dppd_async_flow_config queue = {.admission = fixture.admission,
        .templates = fixture.templates,
        .driver = driver,
        .flow_resource = DPPD_RESOURCE_SOFTWARE_RULE,
        .port_id = fixture.ports[0],
        .depth = 64,
        .flow_capacity = 128};
    assert(dppd_async_flow_queue_create(&queue, &fixture.queue) == 0);
    struct dppd_flow_service_config service = {.admission = fixture.admission,
        .templates = fixture.templates,
        .queues = {fixture.queue},
        .software = &fixture.software,
        .rule_capacity = 64,
        .queue_count = 1,
        .depth = 64,
        .timeout_ns = UINT64_C(5000000000),
        .port_id = fixture.ports[0]};
    assert(dppd_flow_service_create(&service, &fixture.service) == 0);
    dppd_runtime_set_software_backend(&fixture.runtime, &fixture.software);
    assert(dppd_runtime_start(&fixture.runtime) == 0);
    traffic(&fixture, 9000, 32, false, false);
    requests[0] = request(&fixture, false, 1);
    requests[1] = request(&fixture, true, 2);
    assert(dppd_flow_service_submit(fixture.service, 0, requests, 2, now_ns(), &job) == 0);
    assert(dppd_flow_service_process(fixture.service, now_ns(), 1) == 0);
    traffic(&fixture, 9000, 32, false, false);
    struct dppd_flow_job_result result = finish(&fixture, &job);
    assert(result.status == 0 && result.applied);
    traffic(&fixture, 9000, 32, true, false);
    traffic(&fixture, 9001, 32, false, true);
    removals[0] = (struct dppd_flow_remove_request){1, result.generations[0]};
    removals[1] = (struct dppd_flow_remove_request){2, result.generations[1]};
    uint64_t hits = 0, bytes = 0, deadline = rte_get_timer_cycles() + 5U * rte_get_timer_hz();
    do {
        assert(dppd_software_backend_query_count(&fixture.software, 1, removals[0].generation, &hits, &bytes) == 0);
        assert(rte_get_timer_cycles() < deadline);
    } while (hits != 32 || bytes != 2048);
    /** 失败更新保留旧 COUNT，成功更新在同一次软件发布中交换两条策略并重置新版本计数 */
    struct dppd_flow_update_request updates[2] = {{removals[0].generation, request(&fixture, true, 1)},
        {removals[1].generation, request(&fixture, false, 2)}};
    updates[0].replacement.rule.matches[2].spec.l4.dst_be = rte_cpu_to_be_16(9000);
    updates[1].replacement.rule.matches[2].spec.l4.dst_be = rte_cpu_to_be_16(9001);
    assert(dppd_flow_service_update(fixture.service, 0, updates, 2, now_ns(), &job) == 0);
    assert(dppd_flow_service_process(fixture.service, now_ns(), 64) == 0);
    fail_allocation = 1;
    result = finish(&fixture, &job);
    assert(!result.applied && result.status == -ENOMEM);
    assert(dppd_software_backend_query_count(&fixture.software, 1, removals[0].generation, &hits, &bytes) == 0);
    assert(hits == 32 && bytes == 2048);
    traffic(&fixture, 9000, 8, true, false);
    traffic(&fixture, 9001, 8, false, true);
    assert(dppd_flow_service_update(fixture.service, 0, updates, 2, now_ns(), &job) == 0);
    assert(dppd_flow_service_process(fixture.service, now_ns(), 1) == 0);
    traffic(&fixture, 9000, 8, true, false);
    traffic(&fixture, 9001, 8, false, true);
    result = finish(&fixture, &job);
    assert(result.applied && result.update);
    removals[0].generation = result.generations[0];
    removals[1].generation = result.generations[1];
    traffic(&fixture, 9000, 8, false, true);
    traffic(&fixture, 9001, 8, true, false);
    assert(dppd_software_backend_query_count(&fixture.software, 1, removals[0].generation, &hits, &bytes) == 0);
    assert(hits == 8 && bytes == 512);
    assert(dppd_flow_service_remove(fixture.service, 0, removals, 2, now_ns(), &job) == 0);
    assert(dppd_flow_service_process(fixture.service, now_ns(), 1) == 0);
    traffic(&fixture, 9000, 16, false, true);
    traffic(&fixture, 9001, 16, true, false);
    result = finish(&fixture, &job);
    assert(result.status == 0 && result.applied);
    traffic(&fixture, 9000, 32, false, false);
    requests[0] = request(&fixture, false, 3);
    requests[1] = request(&fixture, true, 4);
    assert(dppd_flow_service_submit(fixture.service, 0, requests, 2, now_ns(), &job) == 0);
    fail_allocation = 2;
    result = finish(&fixture, &job);
    assert(result.status == -ENOMEM && !result.applied);
    traffic(&fixture, 9000, 32, false, false);
    assert(dppd_software_backend_count(&fixture.software) == 0);
    dppd_runtime_request_stop(&fixture.runtime);
    assert(dppd_runtime_wait(&fixture.runtime) == 0);
    assert(rte_mempool_avail_count(fixture.pool) == available);
    assert(dppd_flow_service_close(&fixture.service) == 0);
    assert(dppd_async_flow_queue_close(&fixture.queue) == 0);
    assert(dppd_software_async_flows_close(&fixture.driver) == 0);
    dppd_software_backend_fini(&fixture.software);
    assert(dppd_flow_template_destroy(fixture.templates, &fixture.drop) == 0);
    assert(dppd_flow_template_destroy(fixture.templates, &fixture.allow) == 0);
    assert(dppd_flow_templates_close(&fixture.templates) == 0);
    assert(dppd_admission_close(&fixture.admission) == 0);
    assert(dppd_runtime_destroy(&fixture.runtime) == 0);
    for (unsigned int i = 0; i < 2; ++i) {
        assert(rte_ring_empty(fixture.rx[i]) && rte_ring_empty(fixture.tx[i]));
        rte_ring_free(fixture.rx[i]);
        rte_ring_free(fixture.tx[i]);
    }
    assert(rte_eal_cleanup() == 0);
    printf("PASS M4 worker rx=%" PRIu64 " forwarded=%" PRIu64 " dropped=%" PRIu64
           " atomic-create=passed atomic-delete=passed atomic-update=passed update-count-reset=passed"
           " allocation-rollback=passed MARK=42 COUNT=32/2048"
           " native-rejection=%d mbuf-leaks=0 hardware=not_run\n",
        fixture.sent, fixture.forwarded, fixture.dropped, native_result);
    return 0;
}
