#include "dppd/telemetry.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <rte_telemetry.h>
#include "dppd/runtime.h"

static _Atomic(const struct dppd_runtime *) active_runtime;
static bool command_registered;

static int stats_callback(const char *command,
                          const char *parameters,
                          struct rte_tel_data *data)
{
    const struct dppd_runtime *runtime;
    struct dppd_stats_values stats;

    (void)command;
    if (parameters != NULL && parameters[0] != '\0')
        return -EINVAL;
    runtime = atomic_load_explicit(&active_runtime, memory_order_acquire);
    if (runtime == NULL)
        return -EAGAIN;

    dppd_runtime_stats_read(runtime, &stats);
    rte_tel_data_start_dict(data);
#define ADD_TELEMETRY_FIELD(field) \
    if (rte_tel_data_add_dict_u64(data, #field, stats.field) != 0) return -ENOSPC;
    DPPD_STATS_FIELDS(ADD_TELEMETRY_FIELD)
#undef ADD_TELEMETRY_FIELD
    return 0;
}

int dppd_telemetry_register(const struct dppd_runtime *runtime)
{
    int rc = 0;

    if (runtime == NULL)
        return -EINVAL;
    if (!command_registered) {
        rc = rte_telemetry_register_cmd("/dppd/stats",
                                        stats_callback,
                                        "Returns aggregate dppd dataplane counters. Takes no parameters.");
        if (rc != 0)
            return rc;
        command_registered = true;
    }
    atomic_store_explicit(&active_runtime, runtime, memory_order_release);
    return 0;
}

void dppd_telemetry_unregister_runtime(void)
{
    atomic_store_explicit(&active_runtime, NULL, memory_order_release);
}
