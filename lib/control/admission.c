#include "dppd/admission.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct ticket_demand {
    uint64_t amount;
    enum dppd_resource_kind kind;
    uint16_t port_slot;
};

struct admission_ticket {
    uint64_t serial;
    uint32_t count, next_free;
    bool committed, residual;
    struct ticket_demand demands[DPPD_ADMISSION_MAX_DEMANDS];
};

struct dppd_admission {
    pthread_mutex_t lock;
    struct dppd_admission_snapshot state;
    struct admission_ticket *tickets;
    uint64_t owner, next_serial;
    uint32_t free_head;
};

static atomic_uint_fast64_t owners = ATOMIC_VAR_INIT(0);

/** 实例编号也不允许回绕，否则旧实例的凭据可能在新实例中重新变成有效编号 */
static int allocate_owner(uint64_t *output)
{
    uint_fast64_t previous = atomic_load_explicit(&owners, memory_order_relaxed);
    do {
        if (previous == UINT64_MAX)
            return -EOVERFLOW;
    } while (!atomic_compare_exchange_weak_explicit(&owners, &previous, previous + 1U, memory_order_relaxed,
        memory_order_relaxed));
    *output = previous + 1U;
    return 0;
}

static bool valid_config(const struct dppd_admission_config *config)
{
    if (config == NULL || config->ticket_capacity == 0 || config->ticket_capacity > DPPD_ADMISSION_MAX_TICKETS ||
        config->port_count > DPPD_MAX_PORTS)
        return false;
    for (uint32_t i = 0; i < config->port_count; ++i) {
        if (config->ports[i].port_id == DPPD_ADMISSION_GLOBAL_PORT)
            return false;
        for (uint32_t j = 0; j < i; ++j)
            if (config->ports[i].port_id == config->ports[j].port_id)
                return false;
    }
    return true;
}

int dppd_admission_create(const struct dppd_admission_config *config, struct dppd_admission **output)
{
    if (!valid_config(config) || output == NULL)
        return -EINVAL;
    if (*output != NULL)
        return -EALREADY;
    struct dppd_admission *admission = calloc(1, sizeof(*admission));
    if (admission == NULL)
        return -ENOMEM;
    admission->tickets = calloc(config->ticket_capacity, sizeof(*admission->tickets));
    if (admission->tickets == NULL) {
        free(admission);
        return -ENOMEM;
    }
    int rc = pthread_mutex_init(&admission->lock, NULL);
    if (rc != 0) {
        free(admission->tickets);
        free(admission);
        return -rc;
    }
    rc = allocate_owner(&admission->owner);
    if (rc != 0) {
        pthread_mutex_destroy(&admission->lock);
        free(admission->tickets);
        free(admission);
        return rc;
    }
    admission->state.revision = 1;
    admission->state.port_count = config->port_count;
    admission->state.ticket_capacity = config->ticket_capacity;
    memcpy(admission->state.global.limits, config->limits, sizeof(config->limits));
    for (uint32_t i = 0; i < config->port_count; ++i) {
        admission->state.ports[i].port_id = config->ports[i].port_id;
        memcpy(admission->state.ports[i].usage.limits, config->ports[i].limits, sizeof(config->ports[i].limits));
    }
    for (uint32_t i = 0; i < config->ticket_capacity; ++i)
        admission->tickets[i].next_free = i + 1U;
    admission->tickets[config->ticket_capacity - 1U].next_free = UINT32_MAX;
    admission->next_serial = 1;
    *output = admission;
    return 0;
}

int dppd_admission_close(struct dppd_admission **pointer)
{
    if (pointer == NULL || *pointer == NULL)
        return -EINVAL;
    struct dppd_admission *admission = *pointer;
    pthread_mutex_lock(&admission->lock);
    bool busy = admission->state.held_tickets != 0;
    pthread_mutex_unlock(&admission->lock);
    if (busy)
        return -EBUSY;
    pthread_mutex_destroy(&admission->lock);
    free(admission->tickets);
    free(admission);
    *pointer = NULL;
    return 0;
}

static int port_slot(const struct dppd_admission *admission, uint16_t port)
{
    if (port == DPPD_ADMISSION_GLOBAL_PORT)
        return DPPD_MAX_PORTS;
    for (uint32_t i = 0; i < admission->state.port_count; ++i)
        if (admission->state.ports[i].port_id == port)
            return (int)i;
    return -ENOENT;
}

static bool add_amount(uint64_t *total, uint64_t amount)
{
    if (*total > UINT64_MAX - amount)
        return false;
    *total += amount;
    return true;
}

static bool fits(const struct dppd_quota_usage *usage, const uint64_t demand[DPPD_RESOURCE_KIND_COUNT])
{
    for (unsigned int i = 0; i < DPPD_RESOURCE_KIND_COUNT; ++i)
        if (usage->committed[i] > usage->limits[i] || usage->reserved[i] > usage->limits[i] - usage->committed[i] ||
            demand[i] > usage->limits[i] - usage->committed[i] - usage->reserved[i])
            return false;
    return true;
}

/** 同类需求先合并再比较，不能把每份单独放行后才发现合计超额或整数回绕 */
static int reserve_locked(struct dppd_admission *admission, const struct dppd_resource_demand *demands, uint32_t count,
    struct dppd_resource_ticket *output)
{
    uint64_t total[DPPD_RESOURCE_KIND_COUNT] = {0};
    uint64_t ports[DPPD_MAX_PORTS + 1U][DPPD_RESOURCE_KIND_COUNT] = {{0}};
    for (uint32_t i = 0; i < count; ++i) {
        if (demands[i].kind < 0 || demands[i].kind >= DPPD_RESOURCE_KIND_COUNT || demands[i].amount == 0)
            return -EINVAL;
        int slot = port_slot(admission, demands[i].port_id);
        if (slot < 0)
            return slot;
        if (!add_amount(&ports[slot][demands[i].kind], demands[i].amount) ||
            !add_amount(&total[demands[i].kind], demands[i].amount))
            return -EOVERFLOW;
    }
    if (!fits(&admission->state.global, total) || admission->free_head == UINT32_MAX)
        return -ENOSPC;
    for (uint32_t i = 0; i < admission->state.port_count; ++i)
        if (!fits(&admission->state.ports[i].usage, ports[i]))
            return -EDQUOT;
    if (admission->next_serial == UINT64_MAX)
        return -EOVERFLOW;
    uint32_t slot = admission->free_head;
    struct admission_ticket *ticket = &admission->tickets[slot];
    admission->free_head = ticket->next_free;
    memset(ticket, 0, sizeof(*ticket));
    ticket->serial = admission->next_serial++;
    for (unsigned int port = 0; port <= DPPD_MAX_PORTS; ++port)
        for (unsigned int kind = 0; kind < DPPD_RESOURCE_KIND_COUNT; ++kind) {
            if (ports[port][kind] == 0)
                continue;
            ticket->demands[ticket->count++] = (struct ticket_demand){.amount = ports[port][kind],
                .kind = (enum dppd_resource_kind)kind,
                .port_slot = (uint16_t)port};
        }
    for (unsigned int kind = 0; kind < DPPD_RESOURCE_KIND_COUNT; ++kind) {
        admission->state.global.reserved[kind] += total[kind];
        for (uint32_t port = 0; port < admission->state.port_count; ++port)
            admission->state.ports[port].usage.reserved[kind] += ports[port][kind];
    }
    ++admission->state.held_tickets;
    if (admission->state.reservations != UINT64_MAX)
        ++admission->state.reservations;
    *output = (struct dppd_resource_ticket){.owner = admission->owner, .serial = ticket->serial, .slot = slot};
    return 0;
}

int dppd_admission_reserve(struct dppd_admission *admission, const struct dppd_resource_demand *demands, uint32_t count,
    struct dppd_resource_ticket *ticket)
{
    if (admission == NULL || demands == NULL || ticket == NULL || count == 0 || count > DPPD_ADMISSION_MAX_DEMANDS)
        return -EINVAL;
    pthread_mutex_lock(&admission->lock);
    int rc = reserve_locked(admission, demands, count, ticket);
    if (rc != 0 && admission->state.rejections != UINT64_MAX)
        ++admission->state.rejections;
    pthread_mutex_unlock(&admission->lock);
    return rc;
}

static struct admission_ticket *find_ticket(struct dppd_admission *admission, const struct dppd_resource_ticket *ticket)
{
    if (ticket->owner != admission->owner || ticket->slot >= admission->state.ticket_capacity || ticket->serial == 0)
        return NULL;
    struct admission_ticket *found = &admission->tickets[ticket->slot];
    return found->serial == ticket->serial ? found : NULL;
}

/** 预留和提交之间只搬账，不腾出额度，避免另一个生产者抢到仍在安装的资源 */
static void move_to_committed(struct dppd_admission *admission, struct admission_ticket *ticket)
{
    for (uint32_t i = 0; i < ticket->count; ++i) {
        const struct ticket_demand *demand = &ticket->demands[i];
        struct dppd_quota_usage *global = &admission->state.global;
        global->reserved[demand->kind] -= demand->amount;
        global->committed[demand->kind] += demand->amount;
        if (demand->port_slot != DPPD_MAX_PORTS) {
            struct dppd_quota_usage *port = &admission->state.ports[demand->port_slot].usage;
            port->reserved[demand->kind] -= demand->amount;
            port->committed[demand->kind] += demand->amount;
        }
    }
    ticket->committed = true;
}

int dppd_admission_commit(struct dppd_admission *admission, const struct dppd_resource_ticket *identity)
{
    if (admission == NULL || identity == NULL)
        return -EINVAL;
    pthread_mutex_lock(&admission->lock);
    struct admission_ticket *ticket = find_ticket(admission, identity);
    int rc = ticket == NULL ? -ESTALE : ticket->residual ? -EUCLEAN : ticket->committed ? -EALREADY : 0;
    if (rc == 0)
        move_to_committed(admission, ticket);
    pthread_mutex_unlock(&admission->lock);
    return rc;
}

static void release_locked(struct dppd_admission *admission, struct admission_ticket *ticket, uint32_t slot)
{
    for (uint32_t i = 0; i < ticket->count; ++i) {
        const struct ticket_demand *demand = &ticket->demands[i];
        struct dppd_quota_usage *global = &admission->state.global;
        uint64_t *total = ticket->committed ? global->committed : global->reserved;
        total[demand->kind] -= demand->amount;
        if (demand->port_slot != DPPD_MAX_PORTS) {
            struct dppd_quota_usage *port = &admission->state.ports[demand->port_slot].usage;
            uint64_t *used = ticket->committed ? port->committed : port->reserved;
            used[demand->kind] -= demand->amount;
        }
    }
    if (ticket->residual)
        --admission->state.residual_tickets;
    --admission->state.held_tickets;
    memset(ticket, 0, sizeof(*ticket));
    ticket->next_free = admission->free_head;
    admission->free_head = slot;
}

static int release_ticket(struct dppd_admission *admission, const struct dppd_resource_ticket *identity, bool recovered)
{
    if (admission == NULL || identity == NULL)
        return -EINVAL;
    pthread_mutex_lock(&admission->lock);
    struct admission_ticket *ticket = find_ticket(admission, identity);
    int rc = ticket == NULL              ? -ESTALE
        : recovered && !ticket->residual ? -EINVAL
        : !recovered && ticket->residual ? -EUCLEAN
                                         : 0;
    if (rc == 0)
        release_locked(admission, ticket, identity->slot);
    pthread_mutex_unlock(&admission->lock);
    return rc;
}

int dppd_admission_release(struct dppd_admission *admission, const struct dppd_resource_ticket *ticket)
{
    return release_ticket(admission, ticket, false);
}

int dppd_admission_release_recovered(struct dppd_admission *admission, const struct dppd_resource_ticket *ticket)
{
    return release_ticket(admission, ticket, true);
}

int dppd_admission_mark_residual(struct dppd_admission *admission, const struct dppd_resource_ticket *identity)
{
    if (admission == NULL || identity == NULL)
        return -EINVAL;
    pthread_mutex_lock(&admission->lock);
    struct admission_ticket *ticket = find_ticket(admission, identity);
    int rc = ticket == NULL ? -ESTALE : 0;
    if (ticket != NULL && !ticket->residual) {
        ticket->residual = true;
        ++admission->state.residual_tickets;
    }
    pthread_mutex_unlock(&admission->lock);
    return rc;
}

int dppd_admission_snapshot(struct dppd_admission *admission, struct dppd_admission_snapshot *snapshot)
{
    if (admission == NULL || snapshot == NULL)
        return -EINVAL;
    pthread_mutex_lock(&admission->lock);
    *snapshot = admission->state;
    pthread_mutex_unlock(&admission->lock);
    return 0;
}

int dppd_admission_set_limits(struct dppd_admission *admission, const struct dppd_admission_config *config,
    uint64_t expected_revision)
{
    if (admission == NULL || !valid_config(config))
        return -EINVAL;
    pthread_mutex_lock(&admission->lock);
    int rc = 0;
    if (expected_revision != admission->state.revision)
        rc = -ESTALE;
    else if (config->port_count != admission->state.port_count ||
        config->ticket_capacity != admission->state.ticket_capacity)
        rc = -EINVAL;
    else if (admission->state.revision == UINT64_MAX)
        rc = -EOVERFLOW;
    for (uint32_t port = 0; rc == 0 && port < config->port_count; ++port)
        if (config->ports[port].port_id != admission->state.ports[port].port_id)
            rc = -EINVAL;
    for (uint32_t port = 0; rc == 0 && port <= config->port_count; ++port) {
        const struct dppd_quota_usage *usage =
            port == config->port_count ? &admission->state.global : &admission->state.ports[port].usage;
        const uint64_t *limits = port == config->port_count ? config->limits : config->ports[port].limits;
        for (unsigned int kind = 0; kind < DPPD_RESOURCE_KIND_COUNT; ++kind)
            if (usage->committed[kind] > limits[kind] ||
                usage->reserved[kind] > limits[kind] - usage->committed[kind]) {
                rc = -EBUSY;
                break;
            }
    }
    if (rc == 0) {
        memcpy(admission->state.global.limits, config->limits, sizeof(config->limits));
        for (uint32_t port = 0; port < config->port_count; ++port)
            memcpy(admission->state.ports[port].usage.limits, config->ports[port].limits,
                sizeof(config->ports[port].limits));
        ++admission->state.revision;
    }
    pthread_mutex_unlock(&admission->lock);
    return rc;
}
