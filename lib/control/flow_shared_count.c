#include "dppd/flow_shared_count.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct shared_object {
    struct dppd_flow_shared_count_info info;
    struct dppd_shared_count_handle handle;
    struct dppd_resource_ticket ticket;
    bool used, uncertain;
};

struct shared_lease {
    uint64_t serial;
    uint32_t object, next;
    bool used;
};

struct dppd_flow_shared_counts {
    pthread_mutex_t lock;
    struct dppd_admission *admission;
    struct dppd_flow_shared_count_driver driver;
    struct shared_object objects[64];
    struct shared_lease *leases;
    uint64_t owner, serial, generation;
    uint32_t lease_capacity, free_head;
    bool isolated;
};

static _Atomic uint64_t next_owner = 1;

static int owner_allocate(uint64_t *owner)
{
    uint64_t current = atomic_load(&next_owner);
    while (current != UINT64_MAX)
        if (atomic_compare_exchange_weak(&next_owner, &current, current + 1U)) {
            *owner = current;
            return 0;
        }
    return -EOVERFLOW;
}

static struct shared_object *find(struct dppd_flow_shared_counts *counts, uint64_t id)
{
    for (unsigned int i = 0; i < 64; ++i)
        if (counts->objects[i].used && counts->objects[i].info.spec.id == id)
            return &counts->objects[i];
    return NULL;
}

static bool exact(const struct shared_object *object, uint64_t generation)
{
    return object != NULL && generation != 0 && generation != UINT64_MAX && object->info.spec.generation == generation;
}

static bool same_spec(const struct dppd_shared_count_spec *left, const struct dppd_shared_count_spec *right)
{
    return left->id == right->id && left->generation == right->generation &&
        left->install_port_id == right->install_port_id && left->domain == right->domain;
}

int dppd_flow_shared_counts_create(struct dppd_admission *admission, uint32_t lease_capacity,
    const struct dppd_flow_shared_count_driver *driver, struct dppd_flow_shared_counts **output)
{
    struct dppd_flow_shared_counts *counts;
    int rc;
    if (admission == NULL || lease_capacity == 0 || lease_capacity > 65536 || driver == NULL ||
        driver->create == NULL || driver->query == NULL || driver->destroy == NULL || output == NULL || *output != NULL)
        return -EINVAL;
    counts = calloc(1, sizeof(*counts));
    if (counts == NULL)
        return -ENOMEM;
    counts->leases = calloc(lease_capacity, sizeof(*counts->leases));
    if (counts->leases == NULL) {
        free(counts);
        return -ENOMEM;
    }
    rc = owner_allocate(&counts->owner);
    if (rc == 0) {
        rc = pthread_mutex_init(&counts->lock, NULL);
        if (rc != 0)
            rc = -rc;
    }
    if (rc != 0) {
        free(counts->leases);
        free(counts);
        return rc;
    }
    counts->admission = admission;
    counts->driver = *driver;
    counts->lease_capacity = lease_capacity;
    counts->serial = counts->generation = 1;
    for (uint32_t i = 0; i < lease_capacity; ++i)
        counts->leases[i].next = i + 1U;
    *output = counts;
    return 0;
}

int dppd_flow_shared_counts_close(struct dppd_flow_shared_counts **output)
{
    if (output == NULL || *output == NULL)
        return -EINVAL;
    struct dppd_flow_shared_counts *counts = *output;
    pthread_mutex_lock(&counts->lock);
    for (unsigned int i = 0; i < 64; ++i)
        if (counts->objects[i].used) {
            pthread_mutex_unlock(&counts->lock);
            return -EBUSY;
        }
    pthread_mutex_unlock(&counts->lock);
    pthread_mutex_destroy(&counts->lock);
    free(counts->leases);
    free(counts);
    *output = NULL;
    return 0;
}

/** 对象和物理计数器一起扣额，失败创建若留下句柄就保留凭据，不把部分失败当作未发生 */
static int add(struct dppd_flow_shared_counts *counts, uint64_t id, uint16_t port, enum dppd_rule_domain domain,
    uint64_t restored_generation, struct dppd_flow_shared_count_info *info)
{
    struct shared_object *object = NULL;
    struct dppd_resource_demand demands[2] = {{DPPD_RESOURCE_SHARED_ACTION, port, 1}, {DPPD_RESOURCE_COUNTER, port, 1}};
    int rc = 0;
    if (counts == NULL || info == NULL || id == 0 || id == UINT64_MAX || port == UINT16_MAX ||
        domain < DPPD_RULE_DOMAIN_INGRESS || domain > DPPD_RULE_DOMAIN_TRANSFER)
        return -EINVAL;
    pthread_mutex_lock(&counts->lock);
    if (counts->isolated) {
        rc = -EUCLEAN;
        goto done;
    }
    if (find(counts, id) != NULL) {
        rc = -EEXIST;
        goto done;
    }
    if (counts->generation == UINT64_MAX) {
        rc = -EOVERFLOW;
        goto done;
    }
    if (restored_generation != 0 && (restored_generation < counts->generation || restored_generation == UINT64_MAX)) {
        rc = -ESTALE;
        goto done;
    }
    for (unsigned int i = 0; i < 64; ++i)
        if (!counts->objects[i].used) {
            object = &counts->objects[i];
            break;
        }
    if (object == NULL) {
        rc = -ENOSPC;
        goto done;
    }
    rc = dppd_admission_reserve(counts->admission, demands, 2, &object->ticket);
    if (rc != 0)
        goto done;
    uint64_t generation = restored_generation == 0 ? counts->generation : restored_generation;
    object->info.spec = (struct dppd_shared_count_spec){id, generation, port, domain};
    object->handle.spec = object->info.spec;
    rc = counts->driver.create(counts->driver.context, &object->info.spec, &object->handle);
    bool had_handle = object->handle.opaque != NULL, cleanup_confirmed = false;
    object->uncertain = (rc == 0 && !had_handle) || !same_spec(&object->handle.spec, &object->info.spec);
    if (rc != 0 && had_handle && !object->uncertain) {
        int cleanup = counts->driver.destroy(counts->driver.context, &object->handle);
        cleanup_confirmed = cleanup == 0 && object->handle.opaque == NULL;
        object->uncertain =
            (cleanup != 0 && object->handle.opaque == NULL) || (cleanup == 0 && object->handle.opaque != NULL);
    }
    if (rc != 0 && !object->uncertain && (!had_handle || cleanup_confirmed)) {
        dppd_admission_release(counts->admission, &object->ticket);
        memset(object, 0, sizeof(*object));
        goto done;
    }
    counts->generation = generation + 1U;
    object->used = true;
    object->info.residual = rc != 0 || object->uncertain;
    if (!object->info.residual && dppd_admission_commit(counts->admission, &object->ticket) != 0)
        object->info.residual = true;
    if (object->info.residual) {
        dppd_admission_mark_residual(counts->admission, &object->ticket);
        counts->isolated = true;
        rc = -EUCLEAN;
    }
    *info = object->info;
done:
    pthread_mutex_unlock(&counts->lock);
    return rc;
}

int dppd_flow_shared_counts_add(struct dppd_flow_shared_counts *counts, uint64_t id, uint16_t port,
    enum dppd_rule_domain domain, struct dppd_flow_shared_count_info *info)
{
    return add(counts, id, port, domain, 0, info);
}

int dppd_flow_shared_counts_replay_add(struct dppd_flow_shared_counts *counts,
    const struct dppd_shared_count_spec *spec, struct dppd_flow_shared_count_info *info)
{
    if (spec == NULL || spec->generation == 0)
        return -EINVAL;
    return add(counts, spec->id, spec->install_port_id, spec->domain, spec->generation, info);
}

int dppd_flow_shared_counts_seed(struct dppd_flow_shared_counts *counts, uint64_t next_generation)
{
    int rc = 0;
    if (counts == NULL || next_generation == 0)
        return -EINVAL;
    pthread_mutex_lock(&counts->lock);
    if (next_generation < counts->generation)
        rc = -ESTALE;
    else
        counts->generation = next_generation;
    pthread_mutex_unlock(&counts->lock);
    return rc;
}

int dppd_flow_shared_counts_get(struct dppd_flow_shared_counts *counts, uint64_t id, uint64_t generation,
    struct dppd_flow_shared_count_info *info)
{
    if (counts == NULL || info == NULL)
        return -EINVAL;
    pthread_mutex_lock(&counts->lock);
    struct shared_object *object = find(counts, id);
    bool valid = exact(object, generation);
    if (valid)
        *info = object->info;
    pthread_mutex_unlock(&counts->lock);
    return valid ? 0 : -ESTALE;
}

int dppd_flow_shared_counts_query(struct dppd_flow_shared_counts *counts, uint64_t id, uint64_t generation,
    uint64_t *hits, uint64_t *bytes)
{
    uint64_t found_hits = 0, found_bytes = 0;
    int rc;
    if (counts == NULL || hits == NULL || bytes == NULL)
        return -EINVAL;
    pthread_mutex_lock(&counts->lock);
    struct shared_object *object = find(counts, id);
    if (!exact(object, generation))
        rc = -ESTALE;
    else if (object->info.residual)
        rc = -EUCLEAN;
    else
        rc = counts->driver.query(counts->driver.context, &object->handle, &found_hits, &found_bytes);
    if (rc == 0) {
        *hits = found_hits;
        *bytes = found_bytes;
    }
    pthread_mutex_unlock(&counts->lock);
    return rc;
}

int dppd_flow_shared_counts_remove(struct dppd_flow_shared_counts *counts, uint64_t id, uint64_t generation)
{
    int rc;
    if (counts == NULL)
        return -EINVAL;
    pthread_mutex_lock(&counts->lock);
    struct shared_object *object = find(counts, id);
    if (!exact(object, generation)) {
        rc = -ESTALE;
        goto done;
    }
    if (object->info.references != 0) {
        rc = -EBUSY;
        goto done;
    }
    if (object->uncertain || object->handle.opaque == NULL) {
        rc = -EUCLEAN;
        goto done;
    }
    rc = counts->driver.destroy(counts->driver.context, &object->handle);
    if (rc != 0 || object->handle.opaque != NULL) {
        object->uncertain = (rc != 0 && object->handle.opaque == NULL) || (rc == 0 && object->handle.opaque != NULL);
        object->info.residual = true;
        counts->isolated = true;
        dppd_admission_mark_residual(counts->admission, &object->ticket);
        rc = -EUCLEAN;
        goto done;
    }
    rc = object->info.residual ? dppd_admission_release_recovered(counts->admission, &object->ticket)
                               : dppd_admission_release(counts->admission, &object->ticket);
    if (rc == 0) {
        memset(object, 0, sizeof(*object));
        counts->isolated = false;
        for (unsigned int i = 0; i < 64; ++i)
            counts->isolated |= counts->objects[i].used && counts->objects[i].info.residual;
    }
done:
    pthread_mutex_unlock(&counts->lock);
    return rc;
}

int dppd_flow_shared_counts_bind(struct dppd_flow_shared_counts *counts, const struct dppd_rule *rule,
    struct dppd_flow_binding *binding, struct dppd_flow_shared_count_lease *lease)
{
    struct dppd_flow_binding resolved = {0};
    struct shared_object *object;
    struct shared_lease *held;
    char error[128];
    uint16_t action;
    uint32_t slot;
    int rc = 0;
    if (rule == NULL || binding == NULL || lease == NULL || dppd_rule_validate(rule, error, sizeof(error)) != 0)
        return -EINVAL;
    for (action = 0; action < rule->nb_actions; ++action)
        if (rule->actions[action].type == DPPD_ACTION_SHARED_COUNT)
            break;
    if (action == rule->nb_actions) {
        *binding = resolved;
        *lease = (struct dppd_flow_shared_count_lease){0};
        return 0;
    }
    if (counts == NULL)
        return -ENOTSUP;
    pthread_mutex_lock(&counts->lock);
    object = find(counts, rule->actions[action].conf.shared_count.id);
    if (counts->isolated)
        rc = -EUCLEAN;
    else if (!exact(object, rule->actions[action].conf.shared_count.generation))
        rc = -ESTALE;
    else if (object->info.spec.install_port_id != rule->install_port_id || object->info.spec.domain != rule->domain)
        rc = -EXDEV;
    else if (counts->free_head == counts->lease_capacity)
        rc = -ENOSPC;
    else if (counts->serial == UINT64_MAX)
        rc = -EOVERFLOW;
    if (rc == 0) {
        slot = counts->free_head;
        held = &counts->leases[slot];
        counts->free_head = held->next;
        held->used = true;
        held->serial = counts->serial++;
        held->object = (uint32_t)(object - counts->objects);
        ++object->info.references;
        resolved.actions[action] = object->handle.opaque;
        *binding = resolved;
        *lease = (struct dppd_flow_shared_count_lease){counts->owner, held->serial, slot};
    }
    pthread_mutex_unlock(&counts->lock);
    return rc;
}

int dppd_flow_shared_counts_unbind(struct dppd_flow_shared_counts *counts,
    const struct dppd_flow_shared_count_lease *lease)
{
    int rc = 0;
    if (lease == NULL)
        return -EINVAL;
    if (lease->owner == 0 && lease->serial == 0 && lease->slot == 0)
        return 0;
    if (counts == NULL)
        return -ESTALE;
    pthread_mutex_lock(&counts->lock);
    if (lease->owner != counts->owner || lease->slot >= counts->lease_capacity) {
        rc = -ESTALE;
        goto done;
    }
    struct shared_lease *held = &counts->leases[lease->slot];
    if (!held->used || held->serial != lease->serial) {
        rc = -ESTALE;
        goto done;
    }
    if (held->object >= 64) {
        rc = -EUCLEAN;
        goto done;
    }
    struct shared_object *object = &counts->objects[held->object];
    if (!object->used || object->info.references == 0)
        rc = -EUCLEAN;
    else {
        --object->info.references;
        held->used = false;
        held->next = counts->free_head;
        counts->free_head = lease->slot;
    }
done:
    pthread_mutex_unlock(&counts->lock);
    return rc;
}
