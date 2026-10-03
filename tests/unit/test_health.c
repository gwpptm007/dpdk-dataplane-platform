#include <assert.h>
#include <errno.h>
#include <string.h>
#include "dppd/management.h"
#include "dppd/runtime.h"

/**
 * 构造两个端口和两个实际运行的队列，端口编号故意不连续，避免把编号当作数组下标
 * 测试只使用已发布的缓存状态，不初始化 EAL 或访问任何真实网卡
 */
static void reset_runtime(struct dppd_runtime *runtime)
{
    memset(runtime, 0, sizeof(*runtime));
    runtime->config.nb_ports = runtime->devices.nb_ports = 2;
    runtime->config.nb_queues = runtime->nb_workers = 2;
    runtime->workers_started = true;
    atomic_init(&runtime->stop_requested, false);
    atomic_init(&runtime->devices.removal_requested, false);
    for (uint16_t i = 0; i < 2; ++i) {
        struct dppd_port *port = &runtime->devices.ports[i];

        port->port_id = i == 0 ? 5 : 9;
        port->configured = port->started = true;
        atomic_init(&port->removed, false);
        atomic_init(&port->link_state, DPPD_LINK_UP);
        runtime->workers[i].runtime = runtime;
        runtime->workers[i].queue_id = i;
        atomic_init(&runtime->workers[i].state, DPPD_WORKER_RUNNING);
    }
}

/** 经过正式协议分发器查询，确认业务上未就绪不会被错误包装成传输或协议失败 */
static struct dppd_management_health query(struct dppd_control_service *control,
                                           const struct dppd_runtime *runtime)
{
    const struct dppd_management_request request = {
        .version = DPPD_MANAGEMENT_VERSION,
        .size = sizeof(request),
        .operation = DPPD_MANAGEMENT_HEALTH_GET,
        .request_id = 42,
    };
    struct dppd_management_response response;

    assert(dppd_management_handle(control, runtime == NULL ? NULL : &runtime->devices,
                                  runtime, &request, &response) == 0);
    assert(response.status == 0 && response.request_id == 42);
    return response.payload.health;
}

/** 覆盖健康基线、链路恢复、线程状态、移除、持久化失败和隔离查询的组合语义 */
int main(void)
{
    const struct dppd_topology topology = {0};
    struct dppd_control_service control;
    struct dppd_runtime runtime;
    struct dppd_management_health health;
    struct dppd_software_backend unavailable_backend = {0};

    assert(dppd_control_init(&control, &topology, 2, NULL) == 0);
    reset_runtime(&runtime);
    health = query(&control, &runtime);
    assert(health.ready && health.blockers == 0);
    assert(health.workers_running == 2 && health.workers_expected == 2);
    assert(health.ports_started == 2 && health.ports_expected == 2);
    assert(health.links_up == 2 && !health.persistence_enabled);
    assert(dppd_runtime_check_workers(&runtime) == 0);

    /** 缺少 runtime 仍可响应存活查询，但不能宣布转发服务已就绪 */
    health = query(&control, NULL);
    assert(!health.ready && health.blockers == DPPD_HEALTH_NO_RUNTIME);
    assert(dppd_runtime_check_workers(NULL) == -EINVAL);

    /** 同时保留 down 和 unknown 原因，恢复两条链路后才重新就绪 */
    atomic_store(&runtime.devices.ports[0].link_state, DPPD_LINK_DOWN);
    atomic_store(&runtime.devices.ports[1].link_state, DPPD_LINK_UNKNOWN);
    health = query(&control, &runtime);
    assert(!health.ready && health.links_down == 1 && health.links_unknown == 1);
    assert(health.blockers == (DPPD_HEALTH_LINK_DOWN | DPPD_HEALTH_LINK_UNKNOWN));
    atomic_store(&runtime.devices.ports[0].link_state, DPPD_LINK_UP);
    atomic_store(&runtime.devices.ports[1].link_state, DPPD_LINK_UNSUPPORTED);
    health = query(&control, &runtime);
    assert(health.ready && health.links_up == 1 && health.links_unsupported == 1);
    reset_runtime(&runtime);

    /** 启动请求已经提交但线程尚未完成初始化时，不允许就绪，也不当作线程故障 */
    atomic_store(&runtime.workers[1].state, DPPD_WORKER_STARTING);
    health = query(&control, &runtime);
    assert(!health.ready && health.workers_running == 1);
    assert(health.blockers == DPPD_HEALTH_WORKERS_NOT_RUNNING);
    assert(dppd_runtime_check_workers(&runtime) == 0);
    runtime.workers_started = false;
    atomic_store(&runtime.workers[1].state, DPPD_WORKER_RUNNING);
    assert(!query(&control, &runtime).ready);
    reset_runtime(&runtime);

    /** 执行正式 worker 的注册失败路径，必须发布 FAILED 并被周期监控发现 */
    runtime.software_backend = &unavailable_backend;
    assert(dppd_worker_main(&runtime.workers[1]) == -1);
    assert(atomic_load(&runtime.workers[1].state) == DPPD_WORKER_FAILED);
    health = query(&control, &runtime);
    assert(!health.ready && health.workers_failed == 1);
    assert((health.blockers & DPPD_HEALTH_WORKER_FAILED) != 0);
    assert(dppd_runtime_check_workers(&runtime) == -EIO);
    atomic_store(&runtime.workers[1].state, DPPD_WORKER_STOPPED);
    assert(dppd_runtime_check_workers(&runtime) == -EIO);
    reset_runtime(&runtime);

    /** 普通停止会阻止就绪；线程按请求返回时不应被监控再次归类为意外退出 */
    dppd_runtime_request_stop(&runtime);
    assert(dppd_worker_main(&runtime.workers[0]) == 0);
    assert(atomic_load(&runtime.workers[0].state) == DPPD_WORKER_STOPPED);
    health = query(&control, &runtime);
    assert(!health.ready && (health.blockers & DPPD_HEALTH_STOP_REQUESTED) != 0);
    assert(dppd_runtime_check_workers(&runtime) == 0);
    reset_runtime(&runtime);

    /** 端口未完成配置或设备数量不足都会阻止就绪，不能仅检查链路为 up */
    runtime.devices.ports[1].configured = false;
    health = query(&control, &runtime);
    assert(!health.ready && health.ports_started == 1);
    assert(health.blockers == DPPD_HEALTH_PORTS_NOT_STARTED);
    reset_runtime(&runtime);
    runtime.devices.nb_ports = 1;
    assert(!query(&control, &runtime).ready);
    reset_runtime(&runtime);
    runtime.nb_workers = 1;
    assert(!query(&control, &runtime).ready);
    reset_runtime(&runtime);

    /** 任一移除标记即使伴随历史 up 状态也必须阻止就绪，组标记也能独立起作用 */
    atomic_store(&runtime.devices.ports[1].removed, true);
    health = query(&control, &runtime);
    assert(!health.ready && health.blockers == DPPD_HEALTH_DEVICE_REMOVED);
    atomic_store(&runtime.devices.ports[1].removed, false);
    atomic_store(&runtime.devices.removal_requested, true);
    atomic_store(&runtime.workers[0].state, DPPD_WORKER_STOPPED);
    assert(!query(&control, &runtime).ready);
    assert(dppd_runtime_check_workers(&runtime) == 0);
    reset_runtime(&runtime);

    /** 用正式 attach 绑定保存路径，dirty 时不能就绪，也要保留内存与磁盘的版本差距 */
    assert(dppd_control_persistence_attach(&control, "/tmp/dppd-health-unused.bin") == 0);
    control.persistence_dirty = true;
    control.persistence_last_error = -ENOSPC;
    control.rules.generation = 3;
    control.persisted_generation = 2;
    health = query(&control, &runtime);
    assert(!health.ready && health.blockers == DPPD_HEALTH_PERSISTENCE_DIRTY);
    assert(health.repository_generation == 3 && health.persisted_generation == 2);
    assert(health.persistence_enabled && health.persistence_last_error == -ENOSPC);

    /** 恢复隔离仍允许只读健康查询，并同时报告存储问题；普通 ping 继续被拒绝 */
    control.recovery_state = DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED;
    control.recovery_last_error = -EFAULT;
    health = query(&control, &runtime);
    assert(!health.ready && health.recovery_last_error == -EFAULT);
    assert(health.blockers == (DPPD_HEALTH_RECOVERY_REQUIRED | DPPD_HEALTH_PERSISTENCE_DIRTY));
    {
        struct dppd_management_request request = {
            .version = DPPD_MANAGEMENT_VERSION,
            .size = sizeof(request),
            .operation = DPPD_MANAGEMENT_PING,
        };
        struct dppd_management_response response;

        assert(dppd_management_handle(&control, &runtime.devices, &runtime,
                                      &request, &response) == 0);
        assert(response.status == -EUCLEAN);
        /** 上一版客户端必须明确收到 EPROTO，不能按旧协议解释新的健康操作 */
        request.version = 9;
        request.operation = DPPD_MANAGEMENT_HEALTH_GET;
        assert(dppd_management_handle(&control, &runtime.devices, &runtime,
                                      &request, &response) == 0);
        assert(response.status == -EPROTO);
    }
    control.persistence_dirty = false;
    control.persisted_generation = control.rules.generation;
    control.recovery_state = DPPD_CONTROL_RECOVERY_RESTART_REQUIRED;
    health = query(&control, &runtime);
    assert(!health.ready && health.blockers == DPPD_HEALTH_RECOVERY_REQUIRED);
    control.recovery_state = DPPD_CONTROL_RECOVERY_READY;
    assert(query(&control, &runtime).ready);
    assert(dppd_control_fini(&control) == 0);
    return 0;
}
