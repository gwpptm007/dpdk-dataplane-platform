#include "dppd/managed.h"
#include "dppd/flow_management.h"
#include "dppd/runtime.h"
#include "dppd/production.h"

#include <errno.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <rte_ethdev.h>

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

/** EAL 已发现端口后读取真实设备名和驱动，TAP 还要核对当前命名空间中的接口名 */
static int routing_bindings(const struct dppd_config *config, struct dppd_routing_binding *bindings)
{
    memset(bindings, 0, DPPD_MAX_PORTS * sizeof(*bindings));
    for (uint16_t i = 0; i < config->nb_ports; ++i) {
        struct rte_eth_dev_info information;
        char name[RTE_ETH_NAME_MAX_LEN] = {0};
        int rc = rte_eth_dev_info_get(config->ports[i], &information);
        if (rc == 0)
            rc = rte_eth_dev_get_name_by_port(config->ports[i], name);
        if (rc != 0)
            return rc;
        if (information.driver_name == NULL || information.driver_name[0] == '\0' || name[0] == '\0' ||
            strnlen(information.driver_name, sizeof(bindings[i].driver)) >= sizeof(bindings[i].driver) ||
            strnlen(name, sizeof(name)) == sizeof(name) ||
            strnlen(name, sizeof(name)) >= sizeof(bindings[i].device))
            return -ENODEV;
        bindings[i].port_id = config->ports[i];
        strcpy(bindings[i].device, name);
        strcpy(bindings[i].driver, information.driver_name);
        if (strcmp(information.driver_name, "net_tap") == 0 &&
            if_indextoname(information.if_index, bindings[i].ifname) == NULL)
            return -errno;
    }
    return 0;
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
    /** 已保存的路由模式不能因为漏写启动参数而退回端口配对，否则同一配置会产生不同的报文路径 */
    if (!config->managed_routing && dppd_flow_store_router(store) != NULL) {
        rc = -EINVAL;
        goto finish;
    }
    if (config->managed_routing) {
        struct dppd_routing_binding bindings[DPPD_MAX_PORTS];
        rc = routing_bindings(config, bindings);
        if (rc == 0)
            rc = dppd_flow_store_routing_bind(store, bindings, config->nb_ports);
        if (rc != 0)
            goto finish;
    }
    /** 取得独占写锁并完成恢复后再配置网卡，启动失败不会留下第二个控制面 */
    rc = dppd_runtime_init(&runtime, config);
    if (rc != 0)
        goto finish;
    if (config->managed_routing) {
        /** 设备配置可能改变接口状态，开放收包前再次核对身份并检查可用于三层改写的源 MAC */
        struct dppd_routing_binding bindings[DPPD_MAX_PORTS];
        rc = routing_bindings(config, bindings);
        for (uint16_t i = 0; rc == 0 && i < runtime.devices.nb_ports; ++i)
            if (!rte_is_valid_assigned_ether_addr(&runtime.devices.ports[i].mac))
                rc = -ENODEV;
        if (rc == 0)
            rc = dppd_flow_store_routing_bind(store, bindings, config->nb_ports);
        struct dppd_ipv4_router *router = dppd_flow_store_router(store);
        if (rc != 0 || router == NULL) {
            if (rc == 0)
                rc = -EUCLEAN;
            goto destroy;
        }
        dppd_runtime_set_ipv4_router(&runtime, router);
        /** store 始终提供稳定容器，当前关闭也保留地址，在线启用不需要替换 worker 引用 */
        struct dppd_conntrack *conntrack = dppd_flow_store_conntrack(store);
        rc = conntrack == NULL ? -EUCLEAN : dppd_runtime_set_conntrack(&runtime, conntrack);
        if (rc != 0)
            goto destroy;
        /** MAC 只能在设备准备后取得，重启恢复本机配置而不是保存上次的设备 MAC */
        struct dppd_nd6_port nd_ports[DPPD_MAX_PORTS] = {0};
        for (uint16_t i = 0; i < runtime.devices.nb_ports; ++i) {
            nd_ports[i].port_id = runtime.devices.ports[i].port_id;
            memcpy(nd_ports[i].mac, runtime.devices.ports[i].mac.addr_bytes, 6);
        }
        rc = dppd_flow_store_nd6_bind(store, nd_ports, runtime.devices.nb_ports, dppd_monotonic_ms());
        /** 恢复的 ND 地址也需要真实网卡接收组播，准备失败时不能启动 DAD 和报文转发 */
        struct dppd_flow_nd6_view nd_view;
        struct dppd_nd6_rx_preparation nd_preparation;
        if (rc == 0)
            rc = dppd_flow_store_nd6_get(store, &nd_view);
        if (rc == 0)
            rc = dppd_runtime_prepare_nd6(&runtime, &nd_view.policy, &nd_preparation);
        if (rc == 0)
            rc = dppd_runtime_set_nd6(&runtime, dppd_flow_store_nd6(store));
        if (rc != 0)
            goto destroy;
    }
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
            /** runtime 查软件规则表所用的指针不变，旧读者已全部注销，显式修复成功后允许重新启动软件转发 */
            rc = dppd_runtime_start(&runtime);
            timeout_exit(rc, store);
        }
        /** 即使没有新报文也逐步回收过期会话，固定预算避免管理循环被整表扫描拖住 */
        if (rc == 0)
            dppd_conntrack_gc(runtime.conntrack, dppd_monotonic_ms(), 256);
        if (rc == 0) {
            rc = dppd_management_poll(&server);
            /** 在线 ND 更新需要等待 worker 退出，超时线程仍借用共享表，不能继续常规清理 */
            timeout_exit(rc, store);
        }
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
    /** dirty 时 store 会拒绝关闭，先归还其等待包，不能让这些包留到 runtime 释放缓冲池之后 */
    dppd_nd6_discard_pending(runtime.nd6);
    /** store 即将释放借出的容器，先清掉已停止 runtime 中的引用，避免清理路径留下悬空指针 */
    if (!runtime.conntrack_owned)
        (void)dppd_runtime_set_conntrack(&runtime, NULL);
    if (!runtime.nd6_owned)
        (void)dppd_runtime_set_nd6(&runtime, NULL);
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
