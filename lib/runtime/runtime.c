#include "dppd/runtime.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_pdump.h>

int dppd_runtime_init(struct dppd_runtime *runtime, const struct dppd_config *cfg)
{
    char error[256];
    unsigned int lcore_id;
    uint16_t worker_index = 0;
    int rc;

    if (runtime == NULL || cfg == NULL)
        return -EINVAL;
    if (dppd_config_validate(cfg, error, sizeof(error)) != 0) {
        fprintf(stderr, "[dppd] invalid configuration: %s\n", error);
        return -EINVAL;
    }

    memset(runtime, 0, sizeof(*runtime));
    runtime->config = *cfg;
    atomic_init(&runtime->stop_requested, false);

    /*
     * queue_id 按 worker_index 连续编号，不直接使用稀疏的 lcore_id。除便于 queue 配置外，
     * 这也满足软件 backend 对 QSBR reader id 落在 [0, DPPD_MAX_WORKERS) 的要求。
     */
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        struct dppd_worker *worker;

        if (worker_index == cfg->nb_queues)
            break;
        worker = &runtime->workers[worker_index];
        worker->runtime = runtime;
        worker->lcore_id = lcore_id;
        worker->queue_id = worker_index;
        dppd_stats_init(&worker->stats);
        for (uint16_t port_index = 0; port_index < cfg->nb_ports; ++port_index)
            dppd_stats_init(&worker->port_stats[port_index]);
        worker_index++;
    }
    if (worker_index != cfg->nb_queues) {
        fprintf(stderr,
                "[dppd] queues=%u requires %u worker lcores, only %u are enabled\n",
                cfg->nb_queues, cfg->nb_queues, worker_index);
        return -ENOSPC;
    }
    runtime->nb_workers = worker_index;

    rc = dppd_snapshot_build_port_pairs(cfg, &runtime->snapshot);
    if (rc != 0)
        return rc;
    rc = dppd_devices_init(&runtime->devices, cfg);
    if (rc != 0)
        return rc;

    if (cfg->enable_pdump) {
        rc = rte_pdump_init();
        if (rc != 0) {
            fprintf(stderr, "[dppd] cannot initialize pdump service\n");
            dppd_devices_stop(&runtime->devices);
            return -EIO;
        }
        runtime->pdump_initialized = true;
    }
    return 0;
}

void dppd_runtime_set_software_backend(
    struct dppd_runtime *runtime,
    struct dppd_software_backend *software_backend)
{
    /* 不加锁是刻意的：调用契约要求只在 runtime_start 前绑定，运行期禁止热替换。 */
    if (runtime != NULL)
        runtime->software_backend = software_backend;
}

int dppd_runtime_start(struct dppd_runtime *runtime)
{
    uint16_t i;
    int rc;

    if (runtime == NULL || runtime->workers_started)
        return -EINVAL;
    if (dppd_devices_removal_requested(&runtime->devices))
        return -ENODEV;

    atomic_store_explicit(&runtime->stop_requested, false, memory_order_release);
    /* worker 失败时先置 stop，再 wait 已经启动的 lcore，确保其 QSBR reader 都能注销。 */
    for (i = 0; i < runtime->nb_workers; ++i) {
        struct dppd_worker *worker = &runtime->workers[i];

        rc = rte_eal_remote_launch(dppd_worker_main, worker, worker->lcore_id);
        if (rc != 0) {
            fprintf(stderr, "[dppd] cannot launch queue %u on lcore %u: %d\n",
                    worker->queue_id, worker->lcore_id, rc);
            dppd_runtime_request_stop(runtime);
            (void)dppd_runtime_wait(runtime);
            return rc;
        }
        worker->launched = true;
    }
    runtime->workers_started = true;
    return 0;
}

void dppd_runtime_request_stop(struct dppd_runtime *runtime)
{
    if (runtime != NULL)
        atomic_store_explicit(&runtime->stop_requested, true, memory_order_release);
}

int dppd_runtime_wait(struct dppd_runtime *runtime)
{
    uint16_t i;
    int result = 0;

    if (runtime == NULL)
        return -EINVAL;
    for (i = 0; i < runtime->nb_workers; ++i) {
        struct dppd_worker *worker = &runtime->workers[i];
        int rc;

        if (!worker->launched)
            continue;
        rc = rte_eal_wait_lcore(worker->lcore_id);
        worker->launched = false;
        if (rc != 0 && result == 0)
            result = rc;
    }
    runtime->workers_started = false;
    return result;
}

void dppd_runtime_stats_read(const struct dppd_runtime *runtime,
                             struct dppd_stats_values *total)
{
    uint16_t i;

    if (runtime == NULL || total == NULL)
        return;
    memset(total, 0, sizeof(*total));
    for (i = 0; i < runtime->nb_workers; ++i) {
        struct dppd_stats_values values;

        dppd_stats_read(&runtime->workers[i].stats, &values);
        dppd_stats_accumulate(total, &values);
    }
}

int dppd_runtime_stats_query(const struct dppd_runtime *runtime,
                              uint16_t port_id, uint16_t queue_id,
                              struct dppd_stats_values *total)
{
    uint16_t i, j;
    bool has_port = port_id == DPPD_STATS_ALL;
    bool has_queue = queue_id == DPPD_STATS_ALL;

    if (runtime == NULL || total == NULL)
        return -EINVAL;
    memset(total, 0, sizeof(*total));
    for (j = 0; j < runtime->devices.nb_ports; ++j)
        has_port |= runtime->devices.ports[j].port_id == port_id;
    for (i = 0; i < runtime->nb_workers; ++i)
        has_queue |= runtime->workers[i].queue_id == queue_id;
    if (!has_port || !has_queue)
        return -ENOENT;
    for (i = 0; i < runtime->nb_workers; ++i) {
        const struct dppd_worker *worker = &runtime->workers[i];

        if (queue_id != DPPD_STATS_ALL && worker->queue_id != queue_id)
            continue;
        for (j = 0; j < runtime->devices.nb_ports; ++j) {
            struct dppd_stats_values values;

            if (port_id != DPPD_STATS_ALL && runtime->devices.ports[j].port_id != port_id)
                continue;
            dppd_stats_read(&worker->port_stats[j], &values);
            dppd_stats_accumulate(total, &values);
        }
    }
    return 0;
}

void dppd_runtime_stats_dump(const struct dppd_runtime *runtime)
{
    struct dppd_stats_values total;

    dppd_runtime_stats_read(runtime, &total);
    printf("[dppd] rx=%" PRIu64 " (%" PRIu64 " bytes) tx=%" PRIu64
           " (%" PRIu64 " bytes) malformed=%" PRIu64 " unsupported=%" PRIu64
           " policy-drop=%" PRIu64 " tx-drop=%" PRIu64 "\n",
           total.rx_packets, total.rx_bytes, total.tx_packets, total.tx_bytes,
           total.rx_malformed, total.rx_unsupported, total.policy_drops, total.tx_drops);
}

int dppd_runtime_destroy(struct dppd_runtime *runtime)
{
    if (runtime == NULL)
        return -EINVAL;
    /* 先停止 worker，再销毁设备；否则 worker 仍可能访问已停止 port 或软件 backend。 */
    if (runtime->workers_started) {
        dppd_runtime_request_stop(runtime);
        (void)dppd_runtime_wait(runtime);
    }
    if (runtime->pdump_initialized) {
        (void)rte_pdump_uninit();
        runtime->pdump_initialized = false;
    }
    return dppd_devices_stop(&runtime->devices);
}
