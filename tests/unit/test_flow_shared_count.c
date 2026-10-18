#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "dppd/flow_shared_count.h"

struct counter {
    struct dppd_shared_count_spec spec;
    uint64_t hits, bytes;
};

struct model {
    uint32_t created, destroyed, live;
    bool fail_create, partial, fail_destroy, fail_query;
    struct counter *last;
};

static unsigned int fail_allocation;
void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_allocation != 0 && --fail_allocation == 0)
        return NULL;
    return __real_calloc(count, size);
}

static int create(void *context, const struct dppd_shared_count_spec *spec, struct dppd_shared_count_handle *handle)
{
    struct model *model = context;
    ++model->created;
    if (model->fail_create && !model->partial)
        return -EIO;
    struct counter *counter = malloc(sizeof(*counter));
    assert(counter != NULL);
    *counter = (struct counter){.spec = *spec};
    model->last = counter;
    ++model->live;
    *handle = (struct dppd_shared_count_handle){*spec, (struct rte_flow_action_handle *)counter};
    return model->fail_create ? -EIO : 0;
}

static int query(void *context, const struct dppd_shared_count_handle *handle, uint64_t *hits, uint64_t *bytes)
{
    struct model *model = context;
    struct counter *counter = (struct counter *)handle->opaque;
    assert(counter->spec.id == handle->spec.id && counter->spec.generation == handle->spec.generation);
    *hits = counter->hits;
    *bytes = counter->bytes;
    return model->fail_query ? -EIO : 0;
}

static int destroy(void *context, struct dppd_shared_count_handle *handle)
{
    struct model *model = context;
    struct counter *counter = (struct counter *)handle->opaque;
    assert(
        counter != NULL && counter->spec.id == handle->spec.id && counter->spec.generation == handle->spec.generation);
    ++model->destroyed;
    if (model->fail_destroy)
        return -EIO;
    --model->live;
    free(counter);
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static struct dppd_admission *admission(void)
{
    struct dppd_admission_config limits = {.ticket_capacity = 128, .port_count = 2};
    struct dppd_admission *value = NULL;
    limits.ports[0].port_id = 7;
    limits.ports[1].port_id = 8;
    for (unsigned int k = 0; k < DPPD_RESOURCE_KIND_COUNT; ++k) {
        limits.limits[k] = 64;
        limits.ports[0].limits[k] = limits.ports[1].limits[k] = 64;
    }
    assert(dppd_admission_create(&limits, &value) == 0);
    return value;
}

static struct dppd_rule rule(const struct dppd_flow_shared_count_info *counter)
{
    struct dppd_rule rule = {.id = 100,
        .install_port_id = counter->spec.install_port_id,
        .domain = counter->spec.domain,
        .nb_matches = 1,
        .nb_actions = 2};
    rule.matches[0].type = DPPD_MATCH_ETH;
    rule.actions[0].type = DPPD_ACTION_SHARED_COUNT;
    rule.actions[0].conf.shared_count.id = counter->spec.id;
    rule.actions[0].conf.shared_count.generation = counter->spec.generation;
    rule.actions[1].type = DPPD_ACTION_DROP;
    return rule;
}

static void test_lease_identity_and_counter(void)
{
    struct model model = {0};
    struct dppd_admission *quota = admission();
    struct dppd_flow_shared_count_driver driver = {&model, create, query, destroy};
    struct dppd_flow_shared_counts *counts = NULL, *other = NULL;
    struct dppd_flow_shared_count_info info, old;
    struct dppd_flow_shared_count_lease leases[2], held;
    struct dppd_flow_binding binding = {0};
    uint64_t hits = 99, bytes = 88;
    for (unsigned int i = 1; i <= 2; ++i) {
        fail_allocation = i;
        assert(dppd_flow_shared_counts_create(quota, 2, &driver, &counts) == -ENOMEM && counts == NULL);
    }
    assert(dppd_flow_shared_counts_create(quota, 2, &driver, &counts) == 0);
    assert(dppd_flow_shared_counts_create(quota, 2, &driver, &other) == 0);
    assert(dppd_flow_shared_counts_add(counts, 42, 7, DPPD_RULE_DOMAIN_INGRESS, &info) == 0);
    old = info;
    struct dppd_rule request = rule(&info);
    request.install_port_id = 8;
    assert(dppd_flow_shared_counts_bind(counts, &request, &binding, &held) == -EXDEV);
    request = rule(&info);
    assert(dppd_flow_shared_counts_bind(counts, &request, &binding, &leases[0]) == 0);
    assert(binding.actions[0] == (struct rte_flow_action_handle *)model.last);
    assert(dppd_flow_shared_counts_bind(counts, &request, &binding, &leases[1]) == 0);
    held = leases[0];
    assert(dppd_flow_shared_counts_bind(counts, &request, &binding, &held) == -ENOSPC);
    assert(held.serial == leases[0].serial);
    assert(dppd_flow_shared_counts_remove(counts, 42, info.spec.generation) == -EBUSY && model.destroyed == 0);
    model.last->hits = 7;
    model.last->bytes = 700;
    assert(dppd_flow_shared_counts_query(counts, 42, info.spec.generation, &hits, &bytes) == 0 && hits == 7 &&
        bytes == 700);
    model.fail_query = true;
    hits = 99;
    bytes = 88;
    assert(dppd_flow_shared_counts_query(counts, 42, info.spec.generation, &hits, &bytes) == -EIO && hits == 99 &&
        bytes == 88);
    model.fail_query = false;
    assert(dppd_flow_shared_counts_unbind(other, &leases[0]) == -ESTALE);
    assert(dppd_flow_shared_counts_unbind(counts, &leases[0]) == 0);
    assert(dppd_flow_shared_counts_bind(counts, &request, &binding, &held) == 0);
    assert(dppd_flow_shared_counts_unbind(counts, &leases[0]) == -ESTALE);
    assert(dppd_flow_shared_counts_unbind(counts, &held) == 0);
    assert(dppd_flow_shared_counts_unbind(counts, &leases[1]) == 0);
    assert(dppd_flow_shared_counts_remove(counts, 42, info.spec.generation) == 0);
    assert(dppd_flow_shared_counts_add(counts, 42, 7, DPPD_RULE_DOMAIN_INGRESS, &info) == 0);
    assert(info.spec.generation > old.spec.generation);
    request = rule(&old);
    assert(dppd_flow_shared_counts_bind(counts, &request, &binding, &held) == -ESTALE);
    assert(dppd_flow_shared_counts_remove(counts, 42, old.spec.generation) == -ESTALE);
    assert(dppd_flow_shared_counts_remove(counts, 42, info.spec.generation) == 0);
    assert(dppd_flow_shared_counts_close(&counts) == 0 && dppd_flow_shared_counts_close(&other) == 0);
    assert(model.live == 0 && dppd_admission_close(&quota) == 0);
}

static void test_partial_creation_and_residuals(void)
{
    struct model model = {.fail_create = true};
    struct dppd_admission *quota = admission();
    struct dppd_flow_shared_count_driver driver = {&model, create, query, destroy};
    struct dppd_flow_shared_counts *counts = NULL;
    struct dppd_flow_shared_count_info info = {.spec.id = 999};
    struct dppd_admission_snapshot usage;
    assert(dppd_flow_shared_counts_create(quota, 4, &driver, &counts) == 0);
    assert(dppd_flow_shared_counts_add(counts, 42, 7, DPPD_RULE_DOMAIN_INGRESS, &info) == -EIO && info.spec.id == 999);
    model.partial = true;
    assert(dppd_flow_shared_counts_add(counts, 42, 7, DPPD_RULE_DOMAIN_INGRESS, &info) == -EIO && model.live == 0);
    model.fail_destroy = true;
    assert(dppd_flow_shared_counts_add(counts, 42, 7, DPPD_RULE_DOMAIN_INGRESS, &info) == -EUCLEAN && info.residual);
    assert(dppd_admission_snapshot(quota, &usage) == 0 && usage.residual_tickets == 1);
    assert(
        usage.global.reserved[DPPD_RESOURCE_SHARED_ACTION] == 1 && usage.global.reserved[DPPD_RESOURCE_COUNTER] == 1);
    struct dppd_flow_shared_count_info untouched = {.spec.id = 999};
    assert(dppd_flow_shared_counts_add(counts, 43, 7, DPPD_RULE_DOMAIN_INGRESS, &untouched) == -EUCLEAN &&
        untouched.spec.id == 999);
    assert(dppd_flow_shared_counts_close(&counts) == -EBUSY);
    model.fail_destroy = model.fail_create = model.partial = false;
    assert(dppd_flow_shared_counts_remove(counts, info.spec.id, info.spec.generation) == 0);
    assert(dppd_admission_snapshot(quota, &usage) == 0 && usage.held_tickets == 0);
    assert(dppd_flow_shared_counts_add(counts, 42, 7, DPPD_RULE_DOMAIN_INGRESS, &info) == 0);
    model.fail_destroy = true;
    assert(dppd_flow_shared_counts_remove(counts, 42, info.spec.generation) == -EUCLEAN);
    assert(dppd_admission_snapshot(quota, &usage) == 0 && usage.residual_tickets == 1);
    assert(
        usage.global.committed[DPPD_RESOURCE_COUNTER] == 1 && usage.global.committed[DPPD_RESOURCE_SHARED_ACTION] == 1);
    model.fail_destroy = false;
    assert(dppd_flow_shared_counts_remove(counts, 42, info.spec.generation) == 0);
    assert(dppd_flow_shared_counts_close(&counts) == 0 && model.live == 0);
    assert(dppd_admission_close(&quota) == 0);
}

static void test_sixty_four_objects(void)
{
    struct model model = {0};
    struct dppd_admission *quota = admission();
    struct dppd_flow_shared_count_driver driver = {&model, create, query, destroy};
    struct dppd_flow_shared_counts *counts = NULL;
    struct dppd_flow_shared_count_info objects[64], untouched = {.spec.id = 999};
    assert(dppd_flow_shared_counts_create(quota, 4, &driver, &counts) == 0);
    for (unsigned int i = 0; i < 64; ++i)
        assert(dppd_flow_shared_counts_add(counts, i + 1U, 7, DPPD_RULE_DOMAIN_INGRESS, &objects[i]) == 0);
    assert(dppd_flow_shared_counts_add(counts, 65, 7, DPPD_RULE_DOMAIN_INGRESS, &untouched) == -ENOSPC);
    assert(untouched.spec.id == 999 && model.created == 64);
    for (unsigned int i = 0; i < 64; ++i)
        assert(dppd_flow_shared_counts_remove(counts, objects[i].spec.id, objects[i].spec.generation) == 0);
    assert(dppd_flow_shared_counts_close(&counts) == 0 && model.live == 0);
    assert(dppd_admission_close(&quota) == 0);
}

struct producer {
    struct dppd_flow_shared_counts *counts;
    struct dppd_rule rule;
    pthread_barrier_t *start, *held, *finish;
    int result;
};

static void *produce(void *context)
{
    struct producer *job = context;
    struct dppd_flow_shared_count_lease lease;
    struct dppd_flow_binding binding;
    pthread_barrier_wait(job->start);
    job->result = dppd_flow_shared_counts_bind(job->counts, &job->rule, &binding, &lease);
    pthread_barrier_wait(job->held);
    pthread_barrier_wait(job->finish);
    if (job->result == 0)
        assert(dppd_flow_shared_counts_unbind(job->counts, &lease) == 0);
    return NULL;
}

static void test_concurrent_leases(void)
{
    struct model model = {0};
    struct dppd_admission *quota = admission();
    struct dppd_flow_shared_count_driver driver = {&model, create, query, destroy};
    struct dppd_flow_shared_counts *counts = NULL;
    struct dppd_flow_shared_count_info info;
    pthread_barrier_t start, held, finish;
    pthread_t threads[8];
    struct producer jobs[8];
    assert(dppd_flow_shared_counts_create(quota, 2, &driver, &counts) == 0);
    assert(dppd_flow_shared_counts_add(counts, 42, 7, DPPD_RULE_DOMAIN_INGRESS, &info) == 0);
    assert(pthread_barrier_init(&start, NULL, 9) == 0 && pthread_barrier_init(&held, NULL, 9) == 0);
    assert(pthread_barrier_init(&finish, NULL, 9) == 0);
    for (unsigned int i = 0; i < 8; ++i) {
        jobs[i] =
            (struct producer){.counts = counts, .rule = rule(&info), .start = &start, .held = &held, .finish = &finish};
        assert(pthread_create(&threads[i], NULL, produce, &jobs[i]) == 0);
    }
    pthread_barrier_wait(&start);
    pthread_barrier_wait(&held);
    assert(dppd_flow_shared_counts_get(counts, 42, info.spec.generation, &info) == 0 && info.references == 2);
    assert(dppd_flow_shared_counts_remove(counts, 42, info.spec.generation) == -EBUSY);
    pthread_barrier_wait(&finish);
    unsigned int accepted = 0, rejected = 0;
    for (unsigned int i = 0; i < 8; ++i) {
        assert(pthread_join(threads[i], NULL) == 0);
        accepted += jobs[i].result == 0;
        rejected += jobs[i].result == -ENOSPC;
    }
    assert(accepted == 2 && rejected == 6);
    assert(dppd_flow_shared_counts_remove(counts, 42, info.spec.generation) == 0);
    assert(dppd_flow_shared_counts_close(&counts) == 0 && dppd_admission_close(&quota) == 0);
    pthread_barrier_destroy(&start);
    pthread_barrier_destroy(&held);
    pthread_barrier_destroy(&finish);
}

static void test_replay_generation_seed(void)
{
    struct model model = {0};
    struct dppd_admission *quota = admission();
    struct dppd_flow_shared_count_driver driver = {&model, create, query, destroy};
    struct dppd_flow_shared_counts *counts = NULL;
    struct dppd_flow_shared_count_info info, unchanged = {.spec.id = 999};
    struct dppd_shared_count_spec restored = {42, 9, 7, DPPD_RULE_DOMAIN_INGRESS};
    assert(dppd_flow_shared_counts_create(quota, 8, &driver, &counts) == 0);
    assert(dppd_flow_shared_counts_replay_add(counts, &restored, &info) == 0 && info.spec.generation == 9);
    restored.id = 43;
    restored.generation = 3;
    assert(dppd_flow_shared_counts_replay_add(counts, &restored, &unchanged) == -ESTALE && unchanged.spec.id == 999);
    assert(model.created == 1 && dppd_flow_shared_counts_seed(counts, 20) == 0);
    assert(
        dppd_flow_shared_counts_add(counts, 43, 7, DPPD_RULE_DOMAIN_INGRESS, &info) == 0 && info.spec.generation == 20);
    assert(dppd_flow_shared_counts_seed(counts, 20) == -ESTALE);
    assert(dppd_flow_shared_counts_seed(counts, UINT64_MAX) == 0);
    assert(dppd_flow_shared_counts_add(counts, 44, 7, DPPD_RULE_DOMAIN_INGRESS, &unchanged) == -EOVERFLOW);
    assert(model.created == 2 && dppd_flow_shared_counts_remove(counts, 42, 9) == 0);
    assert(dppd_flow_shared_counts_remove(counts, 43, 20) == 0);
    assert(dppd_flow_shared_counts_close(&counts) == 0 && dppd_admission_close(&quota) == 0);
}

int main(void)
{
    test_lease_identity_and_counter();
    test_partial_creation_and_residuals();
    test_sixty_four_objects();
    test_concurrent_leases();
    test_replay_generation_seed();
    return 0;
}
