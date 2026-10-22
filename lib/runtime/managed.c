#include "dppd/managed.h"
#include "dppd/flow_management.h"
#include "dppd/runtime.h"
#include "dppd/production.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static volatile sig_atomic_t stopped;

/** 信号处理器不访问配置、驱动或锁，退出清理由主线程按依赖顺序完成 */
static void stop(int signal_number)
{
    (void)signal_number;
    stopped = 1;
}

/** 只有确认线程进度或停止等待超时才强制退出，其他设备错误仍执行正常的可确认清理 */
static void timeout_exit(int rc, struct dppd_flow_store *store)
{
    if (rc != -ETIMEDOUT)
        return;
    struct dppd_flow_store_status status = {0};
    dppd_flow_store_status(store, &status);
    dppd_production_log(DPPD_PRODUCTION_WORKER_TIMEOUT, rc, status.service.revision);
    _Exit(EXIT_FAILURE);
}

static int wait_workers(struct dppd_runtime *runtime, struct dppd_flow_store *store)
{
    int rc = dppd_runtime_wait(runtime);
    timeout_exit(rc, store);
    return rc;
}

int dppd_managed_run(const struct dppd_config *config, struct dppd_recovery_scope *scope)
{
    struct dppd_flow_state *state = NULL;
    struct dppd_flow_store *store = NULL;
    struct dppd_production *production = NULL;
    struct dppd_runtime runtime;
    struct dppd_management_server server = {.socket_fd = -1};
    struct dppd_flow_management *flows = calloc(1, sizeof(*flows));
    const char *other_paths[] = {config->control_socket};
    int rc = flows == NULL ? -ENOMEM : dppd_routing_check_paths(config->flow_state_path, other_paths, 1);
    if (rc == 0)
        rc = dppd_flow_state_load(config->flow_state_path, &state);
    if (rc != 0)
        goto finish;
    /** 正式入口没有独立的硬件清空证明，原生配置必须明确拒绝，不能靠重启猜测 */
    bool port_present = false;
    for (uint16_t i = 0; i < config->nb_ports; ++i)
        port_present |= config->ports[i] == state->config.port_id;
    if (state->config.mode != DPPD_FLOW_STATE_SOFTWARE || !port_present) {
        rc = -ENOTSUP;
        goto finish;
    }
    dppd_flow_state_free(&state);
    rc = dppd_flow_store_open(config->flow_state_path, NULL, &store);
    if (rc != 0)
        goto finish;
    struct dppd_flow_state_config restored;
    dppd_flow_store_config(store, &restored);
    port_present = false;
    for (uint16_t i = 0; i < config->nb_ports; ++i)
        port_present |= config->ports[i] == restored.port_id;
    if (restored.mode != DPPD_FLOW_STATE_SOFTWARE || !port_present) {
        rc = -ENOTSUP;
        goto finish;
    }
    /** 取得独占写锁并完成恢复后再配置网卡，启动失败不会留下第二个控制面 */
    rc = dppd_runtime_init(&runtime, config);
    if (rc != 0)
        goto finish;
    rc = dppd_flow_management_init(flows, store);
    if (rc != 0)
        goto destroy;
    rc = dppd_devices_enable_event_tracking(&runtime.devices);
    if (rc != 0)
        goto destroy;
    struct dppd_software_backend *software = dppd_flow_store_software(store);
    if (software == NULL) {
        rc = -EUCLEAN;
        goto destroy;
    }
    dppd_runtime_set_software_backend(&runtime, software);
    rc = dppd_management_start_flow(&server, flows, &runtime, config->control_socket);
    if (rc != 0)
        goto destroy;
    rc = dppd_production_start(&runtime, flows, &production);
    if (rc != 0)
        goto destroy;
    stopped = 0;
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    rc = dppd_runtime_start(&runtime);
    timeout_exit(rc, store);
    uint64_t started = dppd_monotonic_ms();
    while (rc == 0 && !stopped) {
        rc = dppd_scope_check(scope);
        if (rc == 0)
            rc = dppd_devices_poll_links(&runtime.devices);
        if (rc == 0) {
            rc = dppd_runtime_check_workers(&runtime);
            /** 卡死线程仍可能持有读者和报文，立即退出交给监护重启，不等待或释放它借用的资源 */
            timeout_exit(rc, store);
        }
        if (rc != 0)
            break;
        /** 任务在主循环推进，任何发布后的写盘失败都先停止读者，再开放恢复命令 */
        (void)dppd_flow_store_process(store, dppd_monotonic_ms() * UINT64_C(1000000), 64);
        struct dppd_flow_store_status status;
        dppd_flow_store_status(store, &status);
        if (!status.ready && runtime.workers_started) {
            dppd_runtime_request_stop(&runtime);
            rc = wait_workers(&runtime, store);
        } else if (status.ready && !runtime.workers_started) {
            /** 后端地址不变，旧读者已全部注销，显式修复成功后允许重新启动软件转发 */
            rc = dppd_runtime_start(&runtime);
            timeout_exit(rc, store);
        }
        if (rc == 0)
            rc = dppd_management_poll(&server);
        if (rc == 0)
            rc = dppd_production_poll(production, dppd_monotonic_ms());
        if (status.ready && config->duration_s != 0 &&
            dppd_monotonic_ms() - started >= (uint64_t)config->duration_s * 1000U)
            break;
        struct timespec pause = {.tv_nsec = 10000000L};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR && !stopped)
            ;
    }
    dppd_management_stop(&server);
    dppd_runtime_request_stop(&runtime);
    if (wait_workers(&runtime, store) != 0)
        rc = -EIO;
    dppd_runtime_stats_dump(&runtime);
    int drained = dppd_flow_management_drain(flows);
    if (drained != 0)
        rc = drained;
destroy:
    dppd_management_stop(&server);
    /** 先让所有查询回调失去借用入口，再等待线程并销毁其引用的状态 */
    dppd_production_stop(&production);
    /** worker 必须先停止并注销 QSBR，才能清空 classifier 和回收实际流 */
    dppd_runtime_request_stop(&runtime);
    if (wait_workers(&runtime, store) != 0)
        rc = -EIO;
    if (store != NULL) {
        int closed = dppd_flow_store_close(&store);
        if (closed != 0)
            rc = closed;
    }
    if (dppd_runtime_destroy(&runtime) != 0)
        rc = -EIO;
finish:
    if (store != NULL && dppd_flow_store_close(&store) != 0)
        rc = -EUCLEAN;
    dppd_flow_state_free(&state);
    free(flows);
    if (rc != 0)
        fprintf(stderr, "[dppd] managed mode failed: %d; inspect configuration before recovery\n", rc);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
