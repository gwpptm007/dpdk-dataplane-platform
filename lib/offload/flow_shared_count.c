#include "dppd/flow_shared_count.h"
#include "dppd/offload.h"
#include "native_flow_lock.h"
#include <stddef.h>

static int native_create(void *context, const struct dppd_shared_count_spec *spec,
    struct dppd_shared_count_handle *handle)
{
    (void)context;
    dppd_native_flow_lock();
    int rc = dppd_flow_shared_count_create(spec, handle, NULL);
    dppd_native_flow_unlock();
    return rc;
}

static int native_query(void *context, const struct dppd_shared_count_handle *handle, uint64_t *hits, uint64_t *bytes)
{
    (void)context;
    dppd_native_flow_lock();
    int rc = dppd_flow_shared_count_query(handle, hits, bytes, NULL);
    dppd_native_flow_unlock();
    return rc;
}

static int native_destroy(void *context, struct dppd_shared_count_handle *handle)
{
    (void)context;
    dppd_native_flow_lock();
    int rc = dppd_flow_shared_count_destroy(handle, NULL);
    dppd_native_flow_unlock();
    return rc;
}

struct dppd_flow_shared_count_driver dppd_native_flow_shared_count_driver(void)
{
    return (struct dppd_flow_shared_count_driver){NULL, native_create, native_query, native_destroy};
}
