#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include "dppd/software_backend.h"

static unsigned int fail_allocation;
void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_allocation != 0 && --fail_allocation == 0)
        return NULL;
    return __real_calloc(count, size);
}

static struct dppd_rule rule(uint64_t id, uint64_t generation, uint32_t priority, uint32_t mark)
{
    struct dppd_rule value = {.id = id,
        .generation = generation,
        .install_port_id = 7,
        .fallback = DPPD_FALLBACK_SOFTWARE_ONLY,
        .priority = priority,
        .nb_matches = 1,
        .nb_actions = 3};
    value.matches[0].type = DPPD_MATCH_ETH;
    value.actions[0] = (struct dppd_action){.type = DPPD_ACTION_MARK, .conf.mark_id = mark};
    value.actions[1].type = DPPD_ACTION_COUNT;
    value.actions[2].type = DPPD_ACTION_DROP;
    return value;
}

static void test_failures_and_full_capacity(void)
{
    struct dppd_software_backend backend;
    struct dppd_rule original[] = {rule(1, 1, 0, 10), rule(2, 2, 1, 20), rule(3, 3, 2, 30)};
    struct dppd_rule replacements[] = {rule(1, 4, 3, 40), rule(4, 5, 0, 50)};
    struct dppd_software_change changes[] = {{1, 0, &original[0]}, {2, 0, &original[1]}, {3, 0, &original[2]}};
    struct dppd_packet packet = {.packet_len = 100};
    struct dppd_software_decision decision;
    uint64_t hits, bytes;
    assert(dppd_software_backend_init(&backend, 3) == 0);
    for (unsigned int allocation = 1; allocation <= 5; ++allocation) {
        fail_allocation = allocation;
        assert(dppd_software_backend_change_batch(&backend, changes, 3) == -ENOMEM);
        assert(dppd_software_backend_count(&backend) == 0);
    }
    assert(dppd_software_backend_change_batch(&backend, changes, 3) == 0);
    assert(dppd_software_backend_worker_register(&backend, 0) == 0);
    dppd_software_backend_decide(&backend, 7, &packet, &decision);
    assert(decision.matched && decision.mark_id == 10);
    dppd_software_backend_worker_quiescent(&backend, 0);
    dppd_software_backend_worker_unregister(&backend, 0);
    assert(dppd_software_backend_query_count(&backend, 1, 1, &hits, &bytes) == 0 && hits == 1 && bytes == 100);
    changes[0] = (struct dppd_software_change){1, 1, &replacements[0]};
    changes[1] = (struct dppd_software_change){2, 2, NULL};
    changes[2] = (struct dppd_software_change){4, 0, &replacements[1]};
    for (unsigned int allocation = 1; allocation <= 4; ++allocation) {
        fail_allocation = allocation;
        assert(dppd_software_backend_change_batch(&backend, changes, 3) == -ENOMEM);
        assert(dppd_software_backend_count(&backend) == 3);
        assert(dppd_software_backend_query_count(&backend, 1, 1, &hits, &bytes) == 0 && hits == 1);
        assert(dppd_software_backend_query_count(&backend, 4, 5, &hits, &bytes) == -ENOENT);
    }
    changes[1].expected_generation = 99;
    assert(dppd_software_backend_change_batch(&backend, changes, 3) == -ESTALE);
    changes[1].expected_generation = 2;
    assert(dppd_software_backend_change_batch(&backend, changes, 3) == 0);
    assert(dppd_software_backend_count(&backend) == 3);
    assert(dppd_software_backend_query_count(&backend, 1, 1, &hits, &bytes) == -ENOENT);
    assert(dppd_software_backend_query_count(&backend, 1, 4, &hits, &bytes) == 0 && hits == 0);
    assert(dppd_software_backend_query_count(&backend, 3, 3, &hits, &bytes) == 0);
    assert(dppd_software_backend_query_count(&backend, 2, 2, &hits, &bytes) == -ENOENT);
    changes[0] = (struct dppd_software_change){1, 4, NULL};
    changes[1] = (struct dppd_software_change){3, 3, NULL};
    changes[2] = (struct dppd_software_change){4, 5, NULL};
    assert(dppd_software_backend_change_batch(&backend, changes, 3) == 0);
    assert(dppd_software_backend_count(&backend) == 0);
    dppd_software_backend_fini(&backend);
}

struct reader_state {
    struct dppd_software_backend *backend;
    atomic_bool stop;
    atomic_uint observations;
};

static void *reader(void *context)
{
    struct reader_state *state = context;
    const struct dppd_packet packet = {.packet_len = 100};
    struct dppd_software_decision decision;
    assert(dppd_software_backend_worker_register(state->backend, 0) == 0);
    while (!atomic_load(&state->stop)) {
        dppd_software_backend_decide(state->backend, 7, &packet, &decision);
        /** 中间只替换第一条会暴露 MARK 20，整批只允许完整旧表 10 或完整新表 40 */
        assert(decision.matched && decision.drop && (decision.mark_id == 10 || decision.mark_id == 40));
        dppd_software_backend_worker_quiescent(state->backend, 0);
        atomic_fetch_add(&state->observations, 1);
    }
    dppd_software_backend_worker_unregister(state->backend, 0);
    return NULL;
}

static void test_concurrent_publication(void)
{
    struct dppd_software_backend backend;
    struct dppd_rule rules[] = {rule(1, 1, 0, 10), rule(2, 2, 1, 20)};
    struct dppd_software_change changes[] = {{1, 0, &rules[0]}, {2, 0, &rules[1]}};
    struct reader_state state = {.backend = &backend};
    pthread_t thread;
    assert(dppd_software_backend_init(&backend, 2) == 0);
    assert(dppd_software_backend_change_batch(&backend, changes, 2) == 0);
    assert(pthread_create(&thread, NULL, reader, &state) == 0);
    while (atomic_load(&state.observations) == 0)
        sched_yield();
    for (unsigned int iteration = 0; iteration < 1000; ++iteration) {
        changes[0].expected_generation = rules[0].generation;
        changes[1].expected_generation = rules[1].generation;
        rules[0].generation += 2;
        rules[1].generation += 2;
        bool new_view = (iteration & 1U) == 0;
        rules[0].priority = new_view ? 2 : 0;
        rules[0].actions[0].conf.mark_id = new_view ? 30 : 10;
        rules[1].priority = new_view ? 0 : 1;
        rules[1].actions[0].conf.mark_id = new_view ? 40 : 20;
        assert(dppd_software_backend_change_batch(&backend, changes, 2) == 0);
    }
    atomic_store(&state.stop, true);
    assert(pthread_join(thread, NULL) == 0 && atomic_load(&state.observations) > 0);
    dppd_software_backend_fini(&backend);
}

int main(void)
{
    test_failures_and_full_capacity();
    test_concurrent_publication();
    return 0;
}
