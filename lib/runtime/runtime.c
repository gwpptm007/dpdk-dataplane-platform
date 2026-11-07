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

/**
 * 初始化数据面运行环境，为后续 worker 收包准备资源
 * cfg 提供网口、队列数和 mbuf 数量等配置，初始化结果保存在 runtime 中
 *
 * 初始化顺序
 *   1. 检查并保存配置
 *   2. 分配 worker 使用的 lcore 和队列编号，清零状态与统计
 *   3. 创建 mbuf 池，配置 RX/TX 队列并启动网口
 *   4. 准备邻居发现，按配置启用连接跟踪和抓包服务
 *
 * 此时网口已启动，worker 要等 dppd_runtime_start 调用后才开始收包
 * 成功返回 0，失败返回负错误码
 */
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
    /** worker 进度超时只用于 flow_state_path 启用的管理模式，普通模式将其关闭 */
    if (cfg->flow_state_path[0] == '\0')
        runtime->config.worker_timeout_ms = 0;
    atomic_init(&runtime->stop_requested, false);

    /**
     * candidates 收集 EAL 已启用的非主 lcore 及其 NUMA 节点，不使用未启用的 CPU
     * port_sockets 记录各网口所在的 NUMA 节点；设备返回未知节点时保留负值
     * NUMA 节点用于选择靠近网口的核心，不是 RX/TX 队列编号
     */
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
    /**
     * EAL 已准备线程及其 CPU 亲和性，本循环只记录 worker 的队列与执行 lcore，不重新创建或绑核
     * 例：-l 0-2 --main-lcore 0，应用配置 --queues 2
     *   主线程：lcore 0，对应逻辑 CPU 0，负责初始化、管理和监控
     *   worker 0：queue_id 0，选中 lcore 1，对应逻辑 CPU 1，处理各网口队列 0
     *   worker 1：queue_id 1，选中 lcore 2，对应逻辑 CPU 2，处理各网口队列 1
     * lcore_id 来自 selected，其他配置下可能不连续；queue_id 连续分配，也用作 QSBR reader 编号
     */
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

    rc = dppd_config_uses_routing(cfg) ? 0 : dppd_snapshot_build_port_pairs(cfg, &runtime->snapshot);
    if (rc != 0)
        return rc;
    rc = dppd_devices_init(&runtime->devices, cfg);
    if (rc != 0)
        return rc;

    /**
     * 网口准备完成后，按 cfg->ports 的顺序收集 MAC，每行保存一个网口的 6 字节地址
     * ND 创建函数按端口号找到对应行并复制 MAC，返回后不保留这个局部数组的地址
     */
    uint8_t macs[DPPD_MAX_PORTS][6] = {{0}};
    for (uint16_t i = 0; i < runtime->devices.nb_ports; ++i)
        memcpy(macs[i], runtime->devices.ports[i].mac.addr_bytes, 6);
    /** 转为指向只读 6 字节数组的指针；&runtime->nd6 用于接收新建管理表的地址 */
    rc = dppd_nd6_create(&runtime->nd6, cfg, (const uint8_t(*)[6])macs, dppd_monotonic_ms());
    if (rc != 0) {
        dppd_devices_stop(&runtime->devices);
        return rc;
    }
    runtime->nd6_owned = runtime->nd6 != NULL;
    /**
     * 启用连接跟踪时创建一张会话表，由 runtime 持有，所有 worker 共用
     * 启动只创建空表，报文通过 stateful_decide 处理后才可能产生会话
     * 不启用时 conntrack 保持 NULL，process_batch 直接走普通 routed_decide
     */
    if (cfg->conntrack_capacity != 0) {
        rc = dppd_conntrack_create_dual(&runtime->conntrack, cfg->conntrack_capacity, cfg->conntrack_timeout_s,
            &cfg->snat, &cfg->snat6, cfg->dnat, cfg->dnat_count);
        if (rc != 0) {
            dppd_nd6_destroy(runtime->nd6);
            runtime->nd6 = NULL;
            runtime->nd6_owned = false;
            dppd_devices_stop(&runtime->devices);
            return rc;
        }
        runtime->conntrack_owned = true;
    }

    if (cfg->enable_pdump) {
        rc = rte_pdump_init();
        if (rc != 0) {
            fprintf(stderr, "[dppd] cannot initialize pdump service\n");
            dppd_nd6_destroy(runtime->nd6);
            runtime->nd6 = NULL;
            runtime->nd6_owned = false;
            dppd_conntrack_destroy(runtime->conntrack);
            runtime->conntrack = NULL;
            runtime->conntrack_owned = false;
            dppd_devices_stop(&runtime->devices);
            return -EIO;
        }
        runtime->pdump_initialized = true;
    }
    return 0;
}

void dppd_runtime_set_software_backend(struct dppd_runtime *runtime, struct dppd_software_backend *software_backend)
{
    /** 保存 control.software 的地址供 worker 查表；启动前绑定，运行时不替换此指针 */
    if (runtime != NULL)
        runtime->software_backend = software_backend;
}

void dppd_runtime_set_ipv4_router(struct dppd_runtime *runtime, struct dppd_ipv4_router *router)
{
    if (runtime != NULL && !runtime->workers_started)
        runtime->ipv4_router = router;
}

int dppd_runtime_set_conntrack(struct dppd_runtime *runtime, struct dppd_conntrack *conntrack)
{
    if (runtime == NULL)
        return -EINVAL;
    if (runtime->workers_started || runtime->conntrack_owned)
        return -EBUSY;
    runtime->conntrack = conntrack;
    return 0;
}

int dppd_runtime_set_nd6(struct dppd_runtime *runtime, struct dppd_nd6 *nd6)
{
    if (runtime == NULL)
        return -EINVAL;
    if (runtime->workers_started || runtime->nd6_owned)
        return -EBUSY;
    runtime->nd6 = nd6;
    return 0;
}

int dppd_runtime_rollback_nd6(struct dppd_runtime *runtime, struct dppd_nd6_rx_preparation *preparation)
{
    uint16_t failed = 0;

    if (runtime == NULL || preparation == NULL || preparation->count > DPPD_MAX_PORTS)
        return -EINVAL;
    if (runtime->workers_started)
        return -EBUSY;
    /** 即使一个端口补偿失败，也继续关闭其余本次新增设置，并保留无法确认的端口 */
    for (uint16_t i = 0; i < preparation->count; ++i) {
        uint16_t port_id = preparation->ports[i];
        const struct dppd_port *port = dppd_devices_find(&runtime->devices, port_id);
        int rc = port == NULL || !port->started || atomic_load_explicit(&port->removed, memory_order_acquire) ?
            -ENODEV : rte_eth_allmulticast_disable(port_id);
        if (rc != 0 || rte_eth_allmulticast_get(port_id) != 0)
            preparation->ports[failed++] = port_id;
    }
    preparation->count = failed;
    return failed == 0 ? 0 : -EUCLEAN;
}

int dppd_runtime_prepare_nd6(struct dppd_runtime *runtime, const struct dppd_nd6_policy *policy,
    struct dppd_nd6_rx_preparation *preparation)
{
    uint16_t ports[DPPD_MAX_PORTS], count = 0;
    bool enabled[DPPD_MAX_PORTS];
    int rc;

    if (runtime == NULL || preparation == NULL)
        return -EINVAL;
    memset(preparation, 0, sizeof(*preparation));
    rc = dppd_nd6_policy_validate(policy);
    if (rc != 0)
        return rc;
    if (runtime->workers_started)
        return -EBUSY;
    /** 关闭 ND 的普通管理转发不需要驱动支持组播开关，也不额外访问网卡 */
    if (policy->local_count == 0)
        return 0;
    if (dppd_devices_io_quiesced(&runtime->devices))
        return -ENODEV;
    /** 先检查全部引用端口并读取原状态，避免后面的端口无效时已经改变前面的端口 */
    for (uint32_t i = 0; i < policy->local_count; ++i) {
        uint16_t port_id = policy->locals[i].port_id, index = 0;
        while (index < count && ports[index] != port_id)
            ++index;
        if (index < count)
            continue;
        const struct dppd_port *port = dppd_devices_find(&runtime->devices, port_id);
        if (port == NULL || !port->started || atomic_load_explicit(&port->removed, memory_order_acquire))
            return -ENODEV;
        if (count == DPPD_MAX_PORTS)
            return -E2BIG;
        ports[count++] = port_id;
    }
    for (uint16_t i = 0; i < count; ++i) {
        rc = rte_eth_allmulticast_get(ports[i]);
        if (rc < 0)
            return rc;
        if (rc > 1)
            return -EIO;
        enabled[i] = rc != 0;
    }
    for (uint16_t i = 0; i < count; ++i) {
        if (enabled[i])
            continue;
        rc = rte_eth_allmulticast_enable(ports[i]);
        int state = rte_eth_allmulticast_get(ports[i]);
        /** 成功开启或状态不明时都登记补偿，明确拒绝且仍关闭则不调用不支持的 disable */
        if (rc == 0 || state != 0)
            preparation->ports[preparation->count++] = ports[i];
        if (rc == 0 && state == 1)
            continue;
        if (rc == 0)
            rc = state < 0 ? state : -EIO;
        if (dppd_runtime_rollback_nd6(runtime, preparation) != 0)
            return -EUCLEAN;
        return rc;
    }
    return 0;
}

/**
 * 将已准备好的 worker 工作函数提交到各自的 lcore，开始执行收包任务
 *
 * 清除停止标记 → 逐个设为 STARTING → remote_launch(worker_main, worker, lcore_id)
 *   → 提交成功，记录 launched，主线程继续启动下一个 worker
 *   → 提交失败，请求已启动的 worker 停止，并等待它们返回
 *
 * 全部提交成功返回 0，失败返回负错误码
 * 返回 0 不代表所有 worker 已完成准备，实际可运行状态由 worker 自己发布为 RUNNING
 */
int dppd_runtime_start(struct dppd_runtime *runtime)
{
    uint16_t i;
    int rc;

    if (runtime == NULL || runtime->workers_started)
        return -EINVAL;
    /** 普通停止后可以重新提交收包任务，设备移除后必须重建整个运行实例 */
    if (dppd_devices_io_quiesced(&runtime->devices))
        return dppd_devices_removal_requested(&runtime->devices) ? -ENODEV : -EUCLEAN;

    atomic_store_explicit(&runtime->stop_requested, false, memory_order_release);
    /** 依次启动各队列的 worker；任一提交失败时，下面的错误分支停止并等待已启动者 */
    for (i = 0; i < runtime->nb_workers; ++i) {
        struct dppd_worker *worker = &runtime->workers[i];

        /** 必须在提交线程之前发布 STARTING，避免覆盖线程已经发布的 RUNNING 状态 */
        atomic_store_explicit(&worker->heartbeat_ms, dppd_monotonic_ms(), memory_order_release);
        atomic_store_explicit(&worker->state, DPPD_WORKER_STARTING, memory_order_release);
        /**
         * 通知指定 lcore 的 EAL 工作线程执行收包函数，线程和 CPU 亲和性已由 EAL 准备
         *
         * dppd_worker_main  → 要执行的函数
         * worker            → 原样传给工作函数的 arg，不复制结构体
         * worker->lcore_id  → 执行任务的 lcore
         *
         * 例：worker 0 选中 lcore 1，则该线程执行 dppd_worker_main(&runtime->workers[0])
         * 主线程提交后继续启动下一个 worker，不等待收包循环结束
         */
        rc = rte_eal_remote_launch(dppd_worker_main, worker, worker->lcore_id);
        if (rc != 0) {
            atomic_store_explicit(&worker->state, DPPD_WORKER_FAILED, memory_order_release);
            fprintf(stderr, "[dppd] cannot launch queue %u on lcore %u: %d\n", worker->queue_id, worker->lcore_id, rc);
            dppd_runtime_request_stop(runtime);
            int waited = dppd_runtime_wait(runtime);
            return waited == -ETIMEDOUT ? waited : rc;
        }
        worker->launched = true;
        /** 已有任务提交成功，清理时必须等待其退出；是否进入 RUNNING 由工作线程自行报告 */
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

/** 设置 stop_requested，供各 worker 的循环和收发检查点读取；返回时收包任务可能仍在执行 */
void dppd_runtime_request_stop(struct dppd_runtime *runtime)
{
    if (runtime != NULL)
        atomic_store_explicit(&runtime->stop_requested, true, memory_order_release);
}

/**
 * 等待已提交启动任务的 worker 返回，收集工作函数结果并更新运行状态
 * launched 为 false 的条目无需等待；已返回的任务清除 launched，全部结束后清除 workers_started
 * 成功返回 0，任务失败返回其错误；启用进度超时的模式可能返回 -ETIMEDOUT，此时仍不能释放线程使用的资源
 */
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
        /** 等待该 lcore 上的 dppd_worker_main 返回，并取得其返回值；STOPPED 标记不能代替这一步 */
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

/**
 * 释放 runtime 拥有的资源，成功返回 0，失败返回负错误码
 * 确认 worker 已退出 → 关闭抓包服务 → 释放邻居等待报文和连接会话 → 关闭设备、释放 mbuf 池
 * 软件规则表和路由由调用方清理，runtime 只通过指针使用它们
 */
int dppd_runtime_destroy(struct dppd_runtime *runtime)
{
    if (runtime == NULL)
        return -EINVAL;
    /** 尚有 worker 任务时先通知并等待，确认全部返回后才允许继续释放资源 */
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
    if (runtime->nd6_owned)
        dppd_nd6_destroy(runtime->nd6);
    runtime->nd6 = NULL;
    runtime->nd6_owned = false;
    if (runtime->conntrack_owned)
        dppd_conntrack_destroy(runtime->conntrack);
    runtime->conntrack = NULL;
    runtime->conntrack_owned = false;
    /** 将端口关闭或回调注销错误交给进程入口，避免清理失败却报告正常退出 */
    return dppd_devices_stop(&runtime->devices);
}
