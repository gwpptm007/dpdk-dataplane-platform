#include "dppd/flow_template.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct template_entry {
    struct dppd_flow_template_info info;
    struct dppd_resource_ticket ticket;
    uint64_t serial;
    bool used;
};

struct template_lease_entry {
    struct dppd_flow_binding binding;
    struct dppd_flow_shared_count_lease shared;
    uint64_t serial;
    uint32_t table, next;
    bool used, pin;
};

struct dppd_flow_templates {
    pthread_mutex_t lock;
    struct dppd_admission *admission;
    struct dppd_flow_template_driver driver;
    struct template_entry tables[DPPD_FLOW_TEMPLATE_LIMIT];
    struct template_lease_entry *leases;
    uint64_t owner, next_serial;
    uint32_t lease_capacity, free_head;
};

static _Atomic uint64_t next_owner = 1;

/** 进程内身份永不回绕，耗尽后拒绝新实例，不能让旧凭据重新有效 */
static int allocate_owner(uint64_t *owner)
{
    uint64_t current = atomic_load(&next_owner);
    while (current != UINT64_MAX) {
        if (atomic_compare_exchange_weak(&next_owner, &current, current + 1U)) {
            *owner = current;
            return 0;
        }
    }
    return -EOVERFLOW;
}

static bool spec_valid(const struct dppd_flow_template_spec *spec)
{
    char error[128];
    return spec != NULL && spec->capacity != 0 && spec->capacity <= DPPD_FLOW_TEMPLATE_LEASE_LIMIT &&
        dppd_rule_validate(&spec->example, error, sizeof(error)) == 0 &&
        (spec->dynamic_actions >> spec->example.nb_actions) == 0;
}

bool dppd_flow_template_matches(const struct dppd_flow_template_spec *spec, const struct dppd_rule *rule)
{
    struct dppd_rule expected, candidate;
    char error[128];
    uint16_t i;

    if (!spec_valid(spec) || rule == NULL || dppd_rule_validate(rule, error, sizeof(error)) != 0)
        return false;
    expected = spec->example;
    candidate = *rule;
    candidate.id = expected.id;
    /** generation 本就不参与规则语义相等判断，匹配掩码和安装属性则必须相同 */
    for (i = 0; i < candidate.nb_matches && i < expected.nb_matches; ++i) {
        if (candidate.matches[i].type != expected.matches[i].type)
            return false;
        switch (candidate.matches[i].type) {
        case DPPD_MATCH_IPV4:
            candidate.matches[i].spec.ipv4.src_be = expected.matches[i].spec.ipv4.src_be;
            candidate.matches[i].spec.ipv4.dst_be = expected.matches[i].spec.ipv4.dst_be;
            break;
        case DPPD_MATCH_IPV6:
            memcpy(candidate.matches[i].spec.ipv6.src, expected.matches[i].spec.ipv6.src, 16);
            memcpy(candidate.matches[i].spec.ipv6.dst, expected.matches[i].spec.ipv6.dst, 16);
            break;
        case DPPD_MATCH_UDP:
        case DPPD_MATCH_TCP:
            candidate.matches[i].spec.l4.src_be = expected.matches[i].spec.l4.src_be;
            candidate.matches[i].spec.l4.dst_be = expected.matches[i].spec.l4.dst_be;
            break;
        case DPPD_MATCH_IP_PROTOCOL:
            candidate.matches[i].spec.protocol.value = expected.matches[i].spec.protocol.value;
            break;
        case DPPD_MATCH_REPRESENTED_PORT:
            candidate.matches[i].spec.ethdev_port_id = expected.matches[i].spec.ethdev_port_id;
            break;
        default:
            /** 范围和连接状态本身定义匹配集合，首期保留固定值，不猜测其模板掩码 */
            break;
        }
    }
    for (i = 0; i < candidate.nb_actions && i < expected.nb_actions; ++i) {
        if (candidate.actions[i].type != expected.actions[i].type)
            return false;
        if ((spec->dynamic_actions & (1U << i)) != 0)
            candidate.actions[i] = expected.actions[i];
    }
    return dppd_rule_equal(&expected, &candidate);
}

int dppd_flow_templates_create(struct dppd_admission *admission, uint32_t lease_capacity,
    const struct dppd_flow_template_driver *driver, struct dppd_flow_templates **output)
{
    struct dppd_flow_templates *templates;
    uint32_t i;
    int rc;

    if (admission == NULL || output == NULL || *output != NULL || lease_capacity == 0 ||
        lease_capacity > DPPD_FLOW_TEMPLATE_LEASE_LIMIT ||
        (driver != NULL &&
            (driver->create == NULL || driver->destroy == NULL ||
                ((driver->bind == NULL) != (driver->unbind == NULL)))))
        return -EINVAL;
    templates = calloc(1, sizeof(*templates));
    if (templates == NULL)
        return -ENOMEM;
    templates->leases = calloc(lease_capacity, sizeof(*templates->leases));
    if (templates->leases == NULL) {
        free(templates);
        return -ENOMEM;
    }
    rc = allocate_owner(&templates->owner);
    if (rc == 0) {
        rc = pthread_mutex_init(&templates->lock, NULL);
        if (rc != 0)
            rc = -rc;
    }
    if (rc != 0) {
        free(templates->leases);
        free(templates);
        return rc;
    }
    templates->admission = admission;
    templates->lease_capacity = lease_capacity;
    templates->next_serial = 1;
    if (driver != NULL)
        templates->driver = *driver;
    for (i = 0; i < lease_capacity; ++i)
        templates->leases[i].next = i + 1U;
    *output = templates;
    return 0;
}

int dppd_flow_templates_close(struct dppd_flow_templates **output)
{
    struct dppd_flow_templates *templates;
    uint32_t i;
    if (output == NULL || *output == NULL)
        return -EINVAL;
    templates = *output;
    pthread_mutex_lock(&templates->lock);
    for (i = 0; i < DPPD_FLOW_TEMPLATE_LIMIT; ++i)
        if (templates->tables[i].used) {
            pthread_mutex_unlock(&templates->lock);
            return -EBUSY;
        }
    pthread_mutex_unlock(&templates->lock);
    pthread_mutex_destroy(&templates->lock);
    free(templates->leases);
    free(templates);
    *output = NULL;
    return 0;
}

static bool has_parts(const struct dppd_flow_template_parts *parts)
{
    return parts->pattern != NULL || parts->actions != NULL || parts->table != NULL || parts->private_data != NULL;
}

static struct template_entry *lookup(struct dppd_flow_templates *templates, const struct dppd_flow_template_id *id)
{
    struct template_entry *entry;
    if (id == NULL || id->owner != templates->owner || id->slot >= DPPD_FLOW_TEMPLATE_LIMIT)
        return NULL;
    entry = &templates->tables[id->slot];
    return entry->used && entry->serial == id->serial ? entry : NULL;
}

int dppd_flow_template_create(struct dppd_flow_templates *templates, const struct dppd_flow_template_spec *spec,
    struct dppd_flow_template_id *id)
{
    struct template_entry *entry = NULL;
    struct dppd_resource_demand demand;
    uint32_t slot;
    int rc;

    if (templates == NULL || id == NULL || !spec_valid(spec))
        return -EINVAL;
    pthread_mutex_lock(&templates->lock);
    if (templates->next_serial == UINT64_MAX) {
        rc = -EOVERFLOW;
        goto done;
    }
    for (slot = 0; slot < DPPD_FLOW_TEMPLATE_LIMIT; ++slot)
        if (!templates->tables[slot].used) {
            entry = &templates->tables[slot];
            break;
        }
    if (entry == NULL) {
        rc = -ENOSPC;
        goto done;
    }
    demand = (struct dppd_resource_demand){DPPD_RESOURCE_TEMPLATE_TABLE, spec->example.install_port_id, 1};
    rc = dppd_admission_reserve(templates->admission, &demand, 1, &entry->ticket);
    if (rc != 0)
        goto done;
    entry->info.spec = *spec;
    if (templates->driver.create != NULL) {
        rc = templates->driver.create(templates->driver.context, spec, &entry->info.parts);
        if (rc == 0 &&
            (entry->info.parts.pattern == NULL || entry->info.parts.actions == NULL || entry->info.parts.table == NULL))
            rc = -EIO;
        if (rc != 0 && has_parts(&entry->info.parts))
            templates->driver.destroy(templates->driver.context, spec->example.install_port_id, &entry->info.parts);
    }
    if (rc != 0 && !has_parts(&entry->info.parts)) {
        dppd_admission_release(templates->admission, &entry->ticket);
        memset(entry, 0, sizeof(*entry));
        goto done;
    }
    entry->serial = templates->next_serial++;
    entry->used = true;
    entry->info.residual = rc != 0;
    if (entry->info.residual) {
        dppd_admission_mark_residual(templates->admission, &entry->ticket);
        rc = -EUCLEAN;
    } else {
        rc = dppd_admission_commit(templates->admission, &entry->ticket);
        if (rc != 0) {
            entry->info.residual = true;
            dppd_admission_mark_residual(templates->admission, &entry->ticket);
            rc = -EUCLEAN;
        }
    }
    *id = (struct dppd_flow_template_id){templates->owner, entry->serial, slot};
done:
    pthread_mutex_unlock(&templates->lock);
    return rc;
}

int dppd_flow_template_get(struct dppd_flow_templates *templates, const struct dppd_flow_template_id *id,
    struct dppd_flow_template_info *info)
{
    struct template_entry *entry;
    if (templates == NULL || info == NULL)
        return -EINVAL;
    pthread_mutex_lock(&templates->lock);
    entry = lookup(templates, id);
    if (entry != NULL)
        *info = entry->info;
    pthread_mutex_unlock(&templates->lock);
    return entry == NULL ? -ESTALE : 0;
}

int dppd_flow_template_destroy(struct dppd_flow_templates *templates, const struct dppd_flow_template_id *id)
{
    struct template_entry *entry;
    int rc = 0;
    if (templates == NULL)
        return -EINVAL;
    pthread_mutex_lock(&templates->lock);
    entry = lookup(templates, id);
    if (entry == NULL) {
        rc = -ESTALE;
        goto done;
    }
    if (entry->info.references != 0) {
        rc = -EBUSY;
        goto done;
    }
    if (has_parts(&entry->info.parts))
        rc = templates->driver.destroy(templates->driver.context, entry->info.spec.example.install_port_id,
            &entry->info.parts);
    if (rc != 0 || has_parts(&entry->info.parts)) {
        entry->info.residual = true;
        dppd_admission_mark_residual(templates->admission, &entry->ticket);
        rc = -EUCLEAN;
        goto done;
    }
    rc = entry->info.residual ? dppd_admission_release_recovered(templates->admission, &entry->ticket)
                              : dppd_admission_release(templates->admission, &entry->ticket);
    if (rc == 0)
        memset(entry, 0, sizeof(*entry));
done:
    pthread_mutex_unlock(&templates->lock);
    return rc;
}

static int acquire(struct dppd_flow_templates *templates, const struct dppd_flow_template_id *id,
    const struct dppd_rule *rule, bool pin, struct dppd_flow_template_lease *lease)
{
    struct template_entry *entry;
    struct template_lease_entry *held;
    struct dppd_flow_binding binding = {0};
    struct dppd_flow_shared_count_lease shared = {0};
    uint32_t slot;
    int rc = 0;
    if (templates == NULL || lease == NULL)
        return -EINVAL;
    pthread_mutex_lock(&templates->lock);
    entry = lookup(templates, id);
    if (entry == NULL)
        rc = -ESTALE;
    else if (entry->info.residual)
        rc = -EUCLEAN;
    else if (!dppd_flow_template_matches(&entry->info.spec, rule))
        rc = -EINVAL;
    else if ((!pin && entry->info.flows == entry->info.spec.capacity) ||
        templates->free_head == templates->lease_capacity)
        rc = -ENOSPC;
    else if (templates->next_serial == UINT64_MAX)
        rc = -EOVERFLOW;
    if (rc == 0) {
        if (templates->driver.bind != NULL)
            rc = templates->driver.bind(templates->driver.context, rule, &binding, &shared);
        else
            for (uint16_t i = 0; i < rule->nb_actions; ++i)
                if (rule->actions[i].type == DPPD_ACTION_SHARED_COUNT) {
                    rc = -ENOTSUP;
                    break;
                }
    }
    if (rc == 0) {
        slot = templates->free_head;
        held = &templates->leases[slot];
        templates->free_head = held->next;
        held->used = true;
        held->pin = pin;
        held->serial = templates->next_serial++;
        held->table = id->slot;
        held->binding = binding;
        held->shared = shared;
        ++entry->info.references;
        if (!pin)
            ++entry->info.flows;
        *lease = (struct dppd_flow_template_lease){templates->owner, held->serial, slot};
    }
    pthread_mutex_unlock(&templates->lock);
    return rc;
}

int dppd_flow_template_acquire(struct dppd_flow_templates *templates, const struct dppd_flow_template_id *id,
    const struct dppd_rule *rule, struct dppd_flow_template_lease *lease)
{
    return acquire(templates, id, rule, false, lease);
}

int dppd_flow_template_pin(struct dppd_flow_templates *templates, const struct dppd_flow_template_id *id,
    const struct dppd_rule *rule, struct dppd_flow_template_lease *lease)
{
    return acquire(templates, id, rule, true, lease);
}

int dppd_flow_template_release(struct dppd_flow_templates *templates, const struct dppd_flow_template_lease *lease)
{
    struct template_lease_entry *held;
    int rc = 0;
    if (templates == NULL || lease == NULL)
        return -EINVAL;
    pthread_mutex_lock(&templates->lock);
    if (lease->owner != templates->owner || lease->slot >= templates->lease_capacity) {
        rc = -ESTALE;
        goto done;
    }
    held = &templates->leases[lease->slot];
    if (!held->used || held->serial != lease->serial) {
        rc = -ESTALE;
        goto done;
    }
    if (!templates->tables[held->table].used || templates->tables[held->table].info.references == 0) {
        rc = -EUCLEAN;
        goto done;
    }
    if (templates->driver.unbind != NULL) {
        rc = templates->driver.unbind(templates->driver.context, &held->shared);
        if (rc != 0)
            goto done;
    }
    --templates->tables[held->table].info.references;
    if (!held->pin)
        --templates->tables[held->table].info.flows;
    held->used = false;
    held->next = templates->free_head;
    templates->free_head = lease->slot;
done:
    pthread_mutex_unlock(&templates->lock);
    return rc;
}

int dppd_flow_template_binding(struct dppd_flow_templates *templates, const struct dppd_flow_template_lease *lease,
    struct dppd_flow_binding *binding)
{
    int rc = 0;
    if (templates == NULL || lease == NULL || binding == NULL)
        return -EINVAL;
    pthread_mutex_lock(&templates->lock);
    if (lease->owner != templates->owner || lease->slot >= templates->lease_capacity ||
        !templates->leases[lease->slot].used || templates->leases[lease->slot].serial != lease->serial)
        rc = -ESTALE;
    else
        *binding = templates->leases[lease->slot].binding;
    pthread_mutex_unlock(&templates->lock);
    return rc;
}
