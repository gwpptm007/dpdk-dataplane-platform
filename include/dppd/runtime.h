#ifndef DPPD_RUNTIME_H
#define DPPD_RUNTIME_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "dppd/config.h"
#include "dppd/device.h"
#include "dppd/pipeline.h"
#include "dppd/stats.h"

struct dppd_runtime;

struct dppd_worker {
    struct dppd_runtime *runtime;
    unsigned int lcore_id;
    uint16_t queue_id;
    bool launched;
    struct dppd_worker_stats stats;
};

struct dppd_runtime {
    struct dppd_config config;
    struct dppd_device_set devices;
    struct dppd_forwarding_snapshot snapshot;
    struct dppd_worker workers[DPPD_MAX_WORKERS];
    uint16_t nb_workers;
    atomic_bool stop_requested;
    bool pdump_initialized;
    bool workers_started;
};

int dppd_runtime_init(struct dppd_runtime *runtime, const struct dppd_config *cfg);
int dppd_runtime_start(struct dppd_runtime *runtime);
void dppd_runtime_request_stop(struct dppd_runtime *runtime);
int dppd_runtime_wait(struct dppd_runtime *runtime);
void dppd_runtime_stats_read(const struct dppd_runtime *runtime,
                             struct dppd_stats_values *total);
void dppd_runtime_stats_dump(const struct dppd_runtime *runtime);
void dppd_runtime_destroy(struct dppd_runtime *runtime);
int dppd_worker_main(void *arg);

#endif
