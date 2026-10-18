#include "dppd/software_shared.h"
#include <errno.h>
#include <stdlib.h>

static int create(void *context, const struct dppd_shared_count_spec *spec, struct dppd_shared_count_handle *handle)
{
    (void)context;
    struct dppd_software_shared_counter *counter = calloc(1, sizeof(*counter));
    if (counter == NULL)
        return -ENOMEM;
    atomic_init(&counter->hits, 0);
    atomic_init(&counter->bytes, 0);
    handle->spec = *spec;
    handle->opaque = (struct rte_flow_action_handle *)counter;
    return 0;
}

static int query(void *context, const struct dppd_shared_count_handle *handle, uint64_t *hits, uint64_t *bytes)
{
    (void)context;
    const struct dppd_software_shared_counter *counter = (const void *)handle->opaque;
    if (counter == NULL)
        return -ESTALE;
    *hits = atomic_load_explicit(&counter->hits, memory_order_relaxed);
    *bytes = atomic_load_explicit(&counter->bytes, memory_order_relaxed);
    return 0;
}

static int destroy(void *context, struct dppd_shared_count_handle *handle)
{
    (void)context;
    free(handle->opaque);
    handle->opaque = NULL;
    return 0;
}

struct dppd_flow_shared_count_driver dppd_software_shared_count_driver(void)
{
    return (struct dppd_flow_shared_count_driver){.create = create, .query = query, .destroy = destroy};
}
