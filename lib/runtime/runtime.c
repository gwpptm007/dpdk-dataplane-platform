#include "dppd/runtime.h"
#include "dppd/numa.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_pdump.h>

int dppd_runtime_init(struct dppd_runtime *runtime, const struct dppd_config *cfg)
{
    char error[256];
    unsigned int lcore_id;
    struct dppd_numa_cpu candidates[RTE_MAX_LCORE], selected[DPPD_MAX_WORKERS];
    int port_sockets[DPPD_MAX_PORTS];
    uint32_t candidate_count = 0;
    int rc;

    if (runtime == NULL || cfg == NULL)
        return -EINVAL;
    if (dppd_config_validate(cfg, error, sizeof(error)) != 0) {
        fprintf(stderr, "[dppd] invalid configuration: %s\n", error);
        return -EINVAL;
    }

    memset(runtime, 0, sizeof(*runtime));
    runtime->config = *cfg;
    /** 直接构造配置的调用者也遵守入口边界，旧模式不能误走超时后无限等待的清理路径 */
    if (cfg->flow_state_path[0] == '\0')
        runtime->config.worker_timeout_ms = 0;
    atomic_init(&runtime->stop_requested, false);

    /** 只在用户启用的非主核中选布局，网卡未知节点保留未知，不猜成节点零 */
    RTE_LCORE_FOREACH_WORKER (lcore_id) {
        candidates[candidate_count++] = (struct dppd_numa_cpu){lcore_id, (int)rte_lcore_to_socket_id(lcore_id)};
    }
    for (uint16_t i = 0; i < cfg->nb_ports; ++i)
        port_sockets[i] = rte_eth_dev_socket_id(cfg->ports[i]);
    rc = dppd_numa_select(candidates, candidate_count, port_sockets, cfg->nb_ports, cfg->nb_queues, selected);
    if (rc != 0) {
        fprintf(stderr, "[dppd] queues=%u cannot select worker lcores from %u enabled candidates: %d\n", cfg->nb_queues,
            candidate_count, rc);
        return rc;
    }
    /** 队列与 QSBR 编号仍连续分配，NUMA 选择只改变队列使用哪个允许的核 */
    for (uint16_t worker_index = 0; worker_index < cfg->nb_queues; ++worker_index) {
        struct dppd_worker *worker = &runtime->workers[worker_index];
        worker->runtime = runtime;
        worker->lcore_id = selected[worker_index].lcore_id;
        worker->socket_id = selected[worker_index].socket_id;
        worker->queue_id = worker_index;
        atomic_init(&worker->state, DPPD_WORKER_IDLE);
        atomic_init(&worker->heartbeat_ms, 0);
        dppd_stats_init(&worker->stats);
        for (uint16_t port_index = 0; port_index < cfg->nb_ports; ++port_index)
            dppd_stats_init(&worker->port_stats[port_index]);
    }
    runtime->nb_workers = cfg->nb_queues;

    rc = cfg->routing_path[0] == '\0' ? dppd_snapshot_build_port_pairs(cfg, &runtime->snapshot) : 0;
    if (rc != 0)
        return rc;
    rc = dppd_devices_init(&runtime->devices, cfg);
    if (rc != 0)
        return rc;

    uint8_t macs[DPPD_MAX_PORTS][6] = {{0}};
    for (uint16_t i = 0; i < runtime->devices.nb_ports; ++i)
        memcpy(macs[i], runtime->devices.ports[i].mac.addr_bytes, 6);
    rc = dppd_nd6_create(&runtime->nd6, cfg, (const uint8_t(*)[6])macs, dppd_monotonic_ms());
    if (rc != 0) {
        dppd_devices_stop(&runtime->devices);
        return rc;
    }
    if (cfg->conntrack_capacity != 0) {
        rc = dppd_conntrack_create_dual(&runtime->conntrack, cfg->conntrack_capacity, cfg->conntrack_timeout_s,
            &cfg->snat, &cfg->snat6, cfg->dnat, cfg->dnat_count);
        if (rc != 0) {
            dppd_nd6_destroy(runtime->nd6);
            runtime->nd6 = NULL;
            dppd_devices_stop(&runtime->devices);
            return rc;
        }
    }

    if (cfg->enable_pdump) {
        rc = rte_pdump_init();
        if (rc != 0) {
            fprintf(stderr, "[dppd] cannot initialize pdump service\n");
            dppd_nd6_destroy(runtime->nd6);
            runtime->nd6 = NULL;
            dppd_conntrack_destroy(runtime->conntrack);
            runtime->conntrack = NULL;
            dppd_devices_stop(&runtime->devices);
            return -EIO;
        }
        runtime->pdump_initialized = true;
    }
    return 0;
}

void dppd_runtime_set_software_backend(struct dppd_runtime *runtime, struct dppd_software_backend *software_backend)
{
    /* 不加锁是刻意的：调用契约要求只在 runtime_start 前绑定，运行期禁止热替换 */
    if (runtime != NULL)
        runtime->software_backend = software_backend;
}

void dppd_runtime_set_ipv4_router(struct dppd_runtime *runtime, struct dppd_ipv4_router *router)
{
    if (runtime != NULL && !runtime->workers_started)
        runtime->ipv4_router = router;
}

int dppd_runtime_start(struct dppd_runtime *runtime)
{
    uint16_t i;
    int rc;

    if (runtime == NULL || runtime->workers_started)
        return -EINVAL;
    /** 普通停止后可以重新启动线程，但设备移除后必须重建整个运行实例 */
    if (dppd_devices_io_quiesced(&runtime->devices))
        return dppd_devices_removal_requested(&runtime->devices) ? -ENODEV : -EUCLEAN;

    atomic_store_explicit(&runtime->stop_requested, false, memory_order_release);
    /* worker 失败时先置 stop，再 wait 已经启动的 lcore，确保其 QSBR reader 都能注销 */
    for (i = 0; i < runtime->nb_workers; ++i) {
        struct dppd_worker *worker = &runtime->workers[i];

        /** 必须在提交线程之前发布 STARTING，避免覆盖线程已经发布的 RUNNING 状态 */
        atomic_store_explicit(&worker->heartbeat_ms, dppd_monotonic_ms(), memory_order_release);
        atomic_store_explicit(&worker->state, DPPD_WORKER_STARTING, memory_order_release);
        rc = rte_eal_remote_launch(dppd_worker_main, worker, worker->lcore_id);
        if (rc != 0) {
            atomic_store_explicit(&worker->state, DPPD_WORKER_FAILED, memory_order_release);
            fprintf(stderr, "[dppd] cannot launch queue %u on lcore %u: %d\n", worker->queue_id, worker->lcore_id, rc);
            dppd_runtime_request_stop(runtime);
            int waited = dppd_runtime_wait(runtime);
            return waited == -ETIMEDOUT ? waited : rc;
        }
        worker->launched = true;
        /** 部分启动失败也保留活跃线程身份，清理方不能提前释放它仍然借用的资源 */
        runtime->workers_started = true;
    }
    runtime->workers_started = true;
    return 0;
}

/**
 * 此检查不等待线程，也不调用网卡接口，因此可放在主线程的周期循环中
 * STARTING 是正常短暂阶段；在没有停止或移除请求时结束的线程必须按故障处理
 */
int dppd_runtime_check_workers(const struct dppd_runtime *runtime)
{
    if (runtime == NULL)
        return -EINVAL;
    if (!runtime->workers_started || atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) ||
        dppd_devices_io_quiesced(&runtime->devices))
        return 0;
    for (uint16_t i = 0; i < runtime->nb_workers; ++i) {
        const unsigned int state = atomic_load_explicit(&runtime->workers[i].state, memory_order_acquire);

        if (state != DPPD_WORKER_STARTING && state != DPPD_WORKER_RUNNING) {
            /**
             * 初次检查后可能才收到停止或移除请求，线程会先看到请求并发布结束状态
             * 读到结束状态后再次检查原因，避免把按通知结束的线程误报为独立故障
             */
            if (atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) ||
                dppd_devices_io_quiesced(&runtime->devices))
                return 0;
            return -EIO;
        }
        if (dppd_runtime_worker_stalled(runtime, i, dppd_monotonic_ms()))
            return -ETIMEDOUT;
    }
    return 0;
}

bool dppd_runtime_worker_stalled(const struct dppd_runtime *runtime, uint16_t worker, uint64_t now_ms)
{
    if (runtime == NULL || worker >= runtime->nb_workers || !runtime->workers_started ||
        runtime->config.worker_timeout_ms == 0 ||
        atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) ||
        dppd_devices_io_quiesced(&runtime->devices))
        return false;
    uint64_t heartbeat = atomic_load_explicit(&runtime->workers[worker].heartbeat_ms, memory_order_acquire);
    /** 读者采样之后 worker 可能更新心跳，略晚于 now 的值仍表示线程正在前进 */
    return heartbeat == 0 || (now_ms >= heartbeat && now_ms - heartbeat > runtime->config.worker_timeout_ms);
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
    uint64_t started = dppd_monotonic_ms();
    for (i = 0; i < runtime->nb_workers; ++i) {
        struct dppd_worker *worker = &runtime->workers[i];
        int rc;

        if (!worker->launched)
            continue;
        /** 管理模式停机也有时限，已请求停止的卡死线程不能绕过主循环心跳检查后永久等待 */
        while (runtime->config.worker_timeout_ms != 0 && rte_eal_get_lcore_state(worker->lcore_id) == RUNNING) {
            if (dppd_monotonic_ms() - started > runtime->config.worker_timeout_ms)
                return -ETIMEDOUT;
            struct timespec pause = {.tv_nsec = 1000000L};
            nanosleep(&pause, NULL);
        }
        rc = rte_eal_wait_lcore(worker->lcore_id);
        worker->launched = false;
        /** 等待结果是最终依据，保留失败状态供清理前的诊断读取 */
        atomic_store_explicit(&worker->state, rc == 0 ? DPPD_WORKER_STOPPED : DPPD_WORKER_FAILED, memory_order_release);
        if (rc != 0 && result == 0)
            result = rc;
    }
    runtime->workers_started = false;
    return result;
}

void dppd_runtime_stats_read(const struct dppd_runtime *runtime, struct dppd_stats_values *total)
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

int dppd_runtime_stats_query(const struct dppd_runtime *runtime, uint16_t port_id, uint16_t queue_id,
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
    printf("[dppd] rx=%" PRIu64 " (%" PRIu64 " bytes) tx=%" PRIu64 " (%" PRIu64 " bytes) malformed=%" PRIu64
           " unsupported=%" PRIu64 " policy-drop=%" PRIu64 " tx-drop=%" PRIu64 "\n",
        total.rx_packets, total.rx_bytes, total.tx_packets, total.tx_bytes, total.rx_malformed, total.rx_unsupported,
        total.policy_drops, total.tx_drops);
}

int dppd_runtime_destroy(struct dppd_runtime *runtime)
{
    if (runtime == NULL)
        return -EINVAL;
    /* 先停止 worker，再销毁设备；否则 worker 仍可能访问已停止 port 或软件 backend */
    if (runtime->workers_started) {
        dppd_runtime_request_stop(runtime);
        int waited = dppd_runtime_wait(runtime);
        /** 未返回的线程仍借用设备和规则，拒绝清理，让正式入口走失败退出 */
        if (runtime->workers_started)
            return waited != 0 ? waited : -EBUSY;
    }
    if (runtime->pdump_initialized) {
        (void)rte_pdump_uninit();
        runtime->pdump_initialized = false;
    }
    dppd_nd6_destroy(runtime->nd6);
    runtime->nd6 = NULL;
    dppd_conntrack_destroy(runtime->conntrack);
    runtime->conntrack = NULL;
    /** 将端口关闭或回调注销错误交给进程入口，避免清理失败却报告正常退出 */
    return dppd_devices_stop(&runtime->devices);
}
