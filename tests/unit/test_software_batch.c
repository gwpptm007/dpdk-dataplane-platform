#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dppd/control.h"

static _Thread_local unsigned int fail_allocation;

void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size);

void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_allocation != 0 && --fail_allocation == 0)
        return NULL;
    return __real_calloc(count, size);
}

struct reader {
    struct dppd_software_backend *backend;
    unsigned int id;
    atomic_bool stop;
    atomic_uint samples;
};

static struct dppd_packet packet(void)
{
    const uint8_t frame[42] = {
        [12] = 0x08, [13] = 0x00, [14] = 0x45, [17] = 28,
        [23] = 17, [39] = 8,
    };
    struct dppd_packet parsed;

    assert(dppd_packet_parse_buffer(frame, sizeof(frame), sizeof(frame), &parsed) == 0);
    return parsed;
}

static void *read_packets(void *arg)
{
    struct reader *reader = arg;
    struct dppd_packet parsed = packet();

    assert(dppd_software_backend_worker_register(reader->backend, reader->id) == 0);
    while (!atomic_load_explicit(&reader->stop, memory_order_acquire)) {
        struct dppd_software_decision decision;

        dppd_software_backend_decide(reader->backend, (uint16_t)(5 + reader->id),
                                     &parsed, &decision);
        assert(decision.matched && decision.drop && decision.has_mark);
        assert(decision.mark_id == 7);
        dppd_software_backend_worker_quiescent(reader->backend, reader->id);
        atomic_fetch_add_explicit(&reader->samples, 1, memory_order_release);
    }
    dppd_software_backend_worker_unregister(reader->backend, reader->id);
    return NULL;
}

static void setup(struct dppd_control_service *service,
                  struct dppd_control_batch_update_request requests[4],
                  struct dppd_topology *topology, uint32_t count)
{
    uint32_t i;

    memset(topology, 0, sizeof(*topology));
    topology->nb_endpoints = 3;
    for (i = 0; i < 3; ++i)
        topology->endpoints[i].ethdev_port_id = (uint16_t)(5 + i);
    assert(dppd_control_init(service, topology, count + 1, NULL) == 0);
    memset(requests, 0, 4 * sizeof(*requests));
    for (i = 0; i <= count; ++i) {
        struct dppd_control_apply_result result;
        struct dppd_rule rule = {0};

        rule.id = 100 + i;
        rule.fallback = DPPD_FALLBACK_SOFTWARE_ONLY;
        rule.priority = i % 2 == 0 ? 10 : 20;
        rule.nb_matches = 1;
        rule.matches[0].type = DPPD_MATCH_ETH;
        rule.nb_actions = 3;
        rule.actions[0].type = DPPD_ACTION_MARK;
        rule.actions[0].conf.mark_id = i % 2 == 0 ? 7 : 9;
        rule.actions[1].type = DPPD_ACTION_COUNT;
        rule.actions[2].type = DPPD_ACTION_DROP;
        assert(dppd_control_apply(service, i == count ? 7 : (uint16_t)(5 + i / 2),
                                  &rule, 0, &result) == 0);
        if (i < count) {
            requests[i].rule = rule;
            requests[i].install_port_id = (uint16_t)(5 + i / 2);
            requests[i].expected_generation = result.generation;
        }
    }
}

static void toggle(struct dppd_control_batch_update_request requests[4], uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; ++i) {
        requests[i].rule.priority = requests[i].rule.priority == 10 ? 20 : 10;
        requests[i].rule.actions[0].conf.mark_id =
            requests[i].rule.priority == 10 ? 7 : 9;
    }
}

static void test_failures(uint32_t count)
{
    struct dppd_control_service service;
    struct dppd_topology topology;
    struct dppd_control_batch_update_request requests[4];
    struct dppd_control_apply_result results[4];
    struct dppd_software_decision decision;
    struct dppd_packet parsed = packet();
    struct dppd_software_classifier_snapshot *original;
    struct dppd_rule_install_info original_info[5], queried_info, batch_info;
    uint64_t hits, bytes;
    uint32_t i;

    setup(&service, requests, &topology, count);
    /** 安装记录属于具体版本，后面的每个失败位置都必须保留这些原始记录 */
    for (i = 0; i <= count; ++i) {
        assert(dppd_software_backend_install_info(&service.software, 100 + i, i + 1,
                                                  &original_info[i]) == 0);
    }
    assert(dppd_software_backend_worker_register(&service.software, 0) == 0);
    dppd_software_backend_decide(&service.software, 5, &parsed, &decision);
    dppd_software_backend_decide(&service.software, 7, &parsed, &decision);
    dppd_software_backend_worker_quiescent(&service.software, 0);
    original = atomic_load(&service.software.active);
    toggle(requests, count);
    for (i = 1; i <= count + 2; ++i) {
        fail_allocation = i;
        assert(dppd_control_update_batch(&service, requests, count, results) == -ENOMEM);
        assert(fail_allocation == 0);
        assert(atomic_load(&service.software.active) == original);
        assert(service.rules.generation == count + 1);
        assert(service.recovery_state == DPPD_CONTROL_RECOVERY_READY);
        assert(dppd_software_backend_count(&service.software) == count + 1);
        assert(dppd_software_backend_query_count(&service.software, 100, 1,
                                                  &hits, &bytes) == 0);
        assert(hits == 1 && bytes == parsed.packet_len);
        for (uint32_t j = 0; j < count; ++j) {
            struct dppd_rule stored;

            assert(dppd_rule_repository_get(&service.rules, 100 + j, &stored) == 0);
            assert(stored.generation == j + 1);
            assert(dppd_software_backend_install_info(&service.software, 100 + j, j + 1,
                                                      &queried_info) == 0);
            assert(memcmp(&queried_info, &original_info[j], sizeof(queried_info)) == 0);
            assert(dppd_software_backend_contains_version(&service.software,
                                                           100 + j, j + 1));
        }
    }
    assert(dppd_control_update_batch(&service, requests, count, results) == 0);
    assert(atomic_load(&service.software.active) != original);
    assert(service.software.retired != NULL);
    /** 成功的一次整表发布对应一份整批耗时，所有新版本共享它，未更新版本保持原记录 */
    assert(dppd_software_backend_install_info(&service.software, 100,
                                              results[0].generation, &batch_info) == 0);
    assert(batch_info.timing_available && batch_info.commit_rule_count == count);
    for (i = 1; i < count; ++i) {
        assert(dppd_software_backend_install_info(&service.software, 100 + i,
                                                  results[i].generation, &queried_info) == 0);
        assert(queried_info.timing_available && queried_info.commit_rule_count == count);
        assert(queried_info.install_duration_ns == batch_info.install_duration_ns);
    }
    assert(dppd_software_backend_install_info(&service.software, 100 + count,
                                              count + 1, &queried_info) == 0);
    assert(memcmp(&queried_info, &original_info[count], sizeof(queried_info)) == 0);
    assert(dppd_software_backend_query_count(&service.software, 100,
                                              results[0].generation, &hits, &bytes) == 0);
    assert(hits == 0 && bytes == 0);
    assert(dppd_software_backend_query_count(&service.software, 100 + count,
                                              count + 1, &hits, &bytes) == 0);
    assert(hits == 1 && bytes == parsed.packet_len);
    assert(dppd_software_backend_query_count(&service.software, 100, 1,
                                              &hits, &bytes) == -ENOENT);
    dppd_software_backend_decide(&service.software, 5, &parsed, &decision);
    assert(decision.drop && decision.mark_id == 7);
    assert(dppd_software_backend_query_count(&service.software, 101,
                                              results[1].generation, &hits, &bytes) == 0);
    assert(hits == 1 && bytes == parsed.packet_len);
    dppd_software_backend_worker_unregister(&service.software, 0);
    /** 读者已退出使旧表可以回收，但安装状态查询本身仍不能触发回收或改变 active */
    {
        struct dppd_software_retired_snapshot *retired = service.software.retired;
        struct dppd_software_classifier_snapshot *active = atomic_load(&service.software.active);

        assert(dppd_software_backend_install_info(&service.software, 100,
                                                  results[0].generation, &queried_info) == 0);
        assert(service.software.retired == retired && atomic_load(&service.software.active) == active);
    }
    assert(dppd_software_backend_contains_version(&service.software, 100,
                                                   results[0].generation));
    assert(service.software.retired == NULL);
    assert(dppd_control_fini(&service) == 0);
}

/** 新旧整表的胜出规则都标记为 7，逐条替换的中间表会出现标记 9，借此检查混合视图 */
static void test_concurrent(uint32_t count)
{
    struct dppd_control_service service;
    struct dppd_topology topology;
    struct dppd_control_batch_update_request requests[4];
    struct dppd_control_apply_result results[4];
    struct reader readers[2];
    pthread_t threads[2];
    uint32_t i, iteration;

    setup(&service, requests, &topology, count);
    for (i = 0; i < count / 2; ++i) {
        readers[i].backend = &service.software;
        readers[i].id = i;
        atomic_init(&readers[i].stop, false);
        atomic_init(&readers[i].samples, 0);
        assert(pthread_create(&threads[i], NULL, read_packets, &readers[i]) == 0);
    }
    for (iteration = 0; iteration < 2000; ++iteration) {
        toggle(requests, count);
        assert(dppd_control_update_batch(&service, requests, count, results) == 0);
        for (i = 0; i < count; ++i)
            requests[i].expected_generation = results[i].generation;
        for (i = 0; i < count / 2; ++i) {
            unsigned int sampled = atomic_load_explicit(&readers[i].samples,
                                                         memory_order_acquire);
            while (atomic_load_explicit(&readers[i].samples, memory_order_acquire) == sampled)
                sched_yield();
        }
    }
    for (i = 0; i < count / 2; ++i) {
        atomic_store_explicit(&readers[i].stop, true, memory_order_release);
        assert(pthread_join(threads[i], NULL) == 0);
        assert(atomic_load(&readers[i].samples) >= 2000);
    }
    assert(service.rules.generation == count + 1 + 2000 * count);
    assert(dppd_software_backend_count(&service.software) == count + 1);
    assert(dppd_control_fini(&service) == 0);
}

static void test_replay(uint32_t count)
{
    struct dppd_control_service source, restored;
    struct dppd_topology topology;
    struct dppd_control_batch_update_request requests[4];
    char directory[] = "/tmp/dppd-software-replay-XXXXXX";
    char path[128];
    uint32_t i, failure;

    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/state.bin", directory);
    setup(&source, requests, &topology, count);
    assert(dppd_control_persistence_attach(&source, path) == 0);
    assert(dppd_control_persistence_flush(&source) == 0);
    assert(dppd_control_fini(&source) == 0);
    for (failure = 1; failure <= 2 + 4 * (count + 1); ++failure) {
        assert(dppd_control_init(&restored, &topology, count + 1, NULL) == 0);
        fail_allocation = failure;
        assert(dppd_control_persistence_restore(&restored, path) == -ENOMEM);
        assert(fail_allocation == 0);
        assert(restored.rules.count == 0 && restored.rules.generation == 0);
        assert(dppd_software_backend_count(&restored.software) == 0);
        assert(restored.persistence_path == NULL);
        assert(restored.recovery_state == DPPD_CONTROL_RECOVERY_READY);
        assert(dppd_control_fini(&restored) == 0);
    }
    assert(dppd_control_init(&restored, &topology, count + 1, NULL) == 0);
    assert(dppd_control_persistence_restore(&restored, path) == 0);
    assert(restored.rules.generation == count + 1 && restored.rules.count == count + 1);
    for (i = 0; i <= count; ++i)
        assert(dppd_software_backend_contains_version(&restored.software, 100 + i, i + 1));
    assert(dppd_control_fini(&restored) == 0);
    {
        struct dppd_transaction_item item = {0};
        struct dppd_transaction transaction;
        struct dppd_transaction_backends backends = {0};

        assert(dppd_control_init(&restored, &topology, count + 1, NULL) == 0);
        item.rule = requests[0].rule;
        item.rule.install_port_id = 5;
        item.rule.generation = 1;
        item.plan.backend = DPPD_PLAN_BACKEND_SOFTWARE;
        item.plan.rule_id = item.rule.id;
        item.plan.rule_generation = 1;
        item.plan.install_port_id = 5;
        backends.software = dppd_software_transaction_backend(&restored.software);
        assert(dppd_transaction_init(&transaction, 1, &item, 1) == 0);
        assert(dppd_transaction_run(&transaction, &backends) == 0);
        assert(dppd_transaction_finalize(&transaction, &backends) == 0);
        assert(dppd_control_persistence_restore(&restored, path) == -EBUSY);
        assert(restored.rules.count == 0 && restored.persistence_path == NULL);
        assert(dppd_software_backend_count(&restored.software) == 1);
        assert(dppd_control_fini(&restored) == 0);
    }
    assert(unlink(path) == 0 && rmdir(directory) == 0);
}

int main(void)
{
    test_failures(2);
    test_failures(4);
    test_concurrent(2);
    test_concurrent(4);
    test_replay(2);
    test_replay(4);
    return 0;
}
