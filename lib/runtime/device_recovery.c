#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <rte_cycles.h>
#include "dppd/control.h"
#include "dppd/device_safety.h"
#include "dppd/runtime.h"

static void increment(uint64_t *value)
{
    if (*value != UINT64_MAX)
        ++*value;
}

static void observe(struct dppd_runtime *runtime, struct dppd_device_event_snapshot *event)
{
    dppd_devices_event_snapshot(&runtime->devices, event);
    struct dppd_device_recovery_status *status = &runtime->device_recovery.status;
    status->sequence = event->sequence;
    status->reset_event_ports = event->reset_ports;
    status->removal_event_ports = event->removal_ports;
    status->destroy_event_ports = event->destroy_ports;
}

/** 这里只读原子事件并封住本地仓库，不在回调或事务内部执行 stop/reset */
static int driver_ready(void *context)
{
    struct dppd_runtime *runtime = context;
    struct dppd_device_event_snapshot event;
    observe(runtime, &event);
    if (!event.enabled || event.overflowed || event.callbacks_in_flight != 0 || event.removal_ports != 0 ||
        event.destroy_ports != 0 || dppd_devices_removal_requested(&runtime->devices) ||
        event.sequence != runtime->device_recovery.status.approved_sequence) {
        dppd_control_device_isolate(runtime->device_recovery.control, -EUCLEAN);
        dppd_runtime_request_stop(runtime);
        return -EUCLEAN;
    }
    return 0;
}

int dppd_runtime_device_bind(struct dppd_runtime *runtime, struct dppd_control_service *control)
{
    if (runtime == NULL || control == NULL || runtime->workers_started || runtime->device_recovery.status.bound)
        return -EINVAL;
    struct dppd_device_recovery *recovery = &runtime->device_recovery;
    recovery->control = control;
    control->rte_flow.driver_ready = driver_ready;
    control->rte_flow.driver_context = runtime;
    int rc = dppd_devices_enable_event_tracking(&runtime->devices);
    if (rc != 0) {
        dppd_control_device_isolate(control, rc);
        return rc;
    }
    recovery->status.bound = true;
    struct dppd_device_event_snapshot event;
    observe(runtime, &event);
    /** 登记期间同步到达的事件不能在绑定时被当作已经恢复的基线 */
    if (event.quiesced || event.sequence != 0 || event.callbacks_in_flight != 0 || event.overflowed)
        return driver_ready(runtime) != 0 ? -EUCLEAN : -EBUSY;
    recovery->status.approved_sequence = 0;
    return 0;
}

int dppd_runtime_device_poll(struct dppd_runtime *runtime)
{
    if (runtime == NULL)
        return -EINVAL;
    struct dppd_device_recovery *recovery = &runtime->device_recovery;
    if (!recovery->status.bound)
        return 0;
    struct dppd_device_event_snapshot event;
    observe(runtime, &event);
    if (event.sequence == recovery->status.approved_sequence && event.callbacks_in_flight == 0 && !event.overflowed &&
        event.removal_ports == 0 && event.destroy_ports == 0 && !dppd_devices_removal_requested(&runtime->devices))
        return 0;
    bool first = recovery->status.state != DPPD_DEVICE_RECOVERY_ISOLATED;
    int error = event.overflowed ? -EOVERFLOW
        : event.removal_ports != 0 || event.destroy_ports != 0 || dppd_devices_removal_requested(&runtime->devices)
        ? -ENODEV
        : -EUCLEAN;
    dppd_control_device_isolate(recovery->control, error);
    dppd_runtime_request_stop(runtime);
    int rc = dppd_runtime_wait(runtime);
    recovery->status.state = DPPD_DEVICE_RECOVERY_ISOLATED;
    recovery->status.workers_stopped = !runtime->workers_started;
    if (!recovery->status.planned || recovery->status.last_error == 0)
        recovery->status.last_error = rc != 0 ? rc : error;
    if (first)
        fprintf(stderr,
            "[dppd] device event isolated: sequence=%" PRIu64 " reset=0x%x removal=0x%x destroy=0x%x error=%d\n",
            event.sequence, event.reset_ports, event.removal_ports, event.destroy_ports, error);
    return rc;
}

/** 任何失败都保留计划和实际对象，只有事件证明句柄未知时才锁住整个驱动仓库 */
static int failed(struct dppd_runtime *runtime, int error)
{
    struct dppd_device_recovery *recovery = &runtime->device_recovery;
    dppd_runtime_request_stop(runtime);
    int stopped = dppd_runtime_wait(runtime);
    struct dppd_device_event_snapshot event;
    observe(runtime, &event);
    if (event.sequence != recovery->status.approved_sequence || event.callbacks_in_flight != 0 || event.overflowed ||
        event.removal_ports != 0 || event.destroy_ports != 0 || dppd_devices_removal_requested(&runtime->devices))
        dppd_control_device_isolate(recovery->control, error);
    else {
        recovery->control->recovery_state = DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED;
        recovery->control->recovery_last_error = error;
    }
    (void)dppd_devices_pause_io(&runtime->devices, event.sequence);
    recovery->status.state = DPPD_DEVICE_RECOVERY_ISOLATED;
    recovery->status.last_error = error;
    recovery->status.cleanup_error = stopped;
    recovery->status.workers_stopped = !runtime->workers_started;
    return stopped != 0 ? -EUCLEAN : error;
}

static int stable_event(struct dppd_runtime *runtime, uint64_t expected_sequence)
{
    struct dppd_device_event_snapshot event;
    observe(runtime, &event);
    if (event.overflowed)
        return -EOVERFLOW;
    if (event.removal_ports != 0 || event.destroy_ports != 0 || dppd_devices_removal_requested(&runtime->devices))
        return -ENODEV;
    if (!event.enabled || event.callbacks_in_flight != 0)
        return -EBUSY;
    return event.sequence == expected_sequence ? 0 : -ESTALE;
}

/** 启动请求成功还不够，等所有 worker 完成实际登记再发布控制面 READY */
static int wait_workers_ready(struct dppd_runtime *runtime)
{
    uint64_t started = rte_get_timer_cycles(), budget = rte_get_timer_hz() * 2U;
    do {
        if (dppd_devices_io_quiesced(&runtime->devices))
            return -EUCLEAN;
        bool ready = true;
        for (uint16_t i = 0; i < runtime->nb_workers; ++i) {
            unsigned int state = atomic_load_explicit(&runtime->workers[i].state, memory_order_acquire);
            if (state == DPPD_WORKER_FAILED || state == DPPD_WORKER_STOPPED)
                return -EIO;
            ready &= state == DPPD_WORKER_RUNNING;
        }
        if (ready)
            return 0;
        const struct timespec delay = {.tv_nsec = 1000000};
        (void)nanosleep(&delay, NULL);
    } while (rte_get_timer_cycles() - started < budget);
    return -ETIMEDOUT;
}

/** 端口重新 start 不等于物理链路已恢复，只有可发送状态才继续重建业务对象 */
static int wait_links_ready(struct dppd_runtime *runtime, uint64_t sequence)
{
    uint64_t started = rte_get_timer_cycles(), budget = rte_get_timer_hz() * 2U;
    do {
        int rc = stable_event(runtime, sequence);
        if (rc == 0)
            rc = dppd_devices_poll_links(&runtime->devices);
        if (rc == 0)
            rc = stable_event(runtime, sequence);
        if (rc != 0)
            return rc;
        bool ready = true;
        for (uint16_t i = 0; i < runtime->devices.nb_ports; ++i)
            ready &= dppd_port_tx_available(&runtime->devices.ports[i]);
        if (ready)
            return 0;
        const struct timespec delay = {.tv_nsec = 10000000};
        (void)nanosleep(&delay, NULL);
    } while (rte_get_timer_cycles() - started < budget);
    return -ETIMEDOUT;
}

int dppd_runtime_device_reset(struct dppd_runtime *runtime, uint64_t expected_sequence)
{
    if (runtime == NULL || !runtime->device_recovery.status.bound)
        return -ENOTSUP;
    struct dppd_device_recovery *recovery = &runtime->device_recovery;
    struct dppd_control_service *control = recovery->control;
    int rc = stable_event(runtime, expected_sequence);
    if (rc != 0)
        return rc;
    /** 未确认旧对象的异常复位没有可移植清理手段，禁止靠再次 reset 或遗忘指针解除 */
    if (dppd_rte_flow_backend_is_quarantined(&control->rte_flow) &&
        (!recovery->status.planned || !recovery->cleanup_complete ||
            dppd_rte_flow_backend_count(&control->rte_flow) != 0 ||
            dppd_rte_flow_backend_shared_count_count(&control->rte_flow) != 0))
        return -EUCLEAN;
    rc = dppd_devices_check_reset_safety(&runtime->devices);
    if (rc != 0)
        return rc;
    rc = dppd_devices_reset_identity_ready(&runtime->devices, &runtime->config);
    if (rc != 0)
        return rc;
    struct dppd_topology candidate;
    rc = dppd_topology_rediscover_verified(&runtime->config, &runtime->devices.topology, &candidate);
    if (rc != 0)
        return rc;
    if (!recovery->status.planned) {
        rc = dppd_control_device_reset_prepare(control);
        if (rc != 0)
            return rc;
        recovery->status.planned = true;
    } else if (!dppd_control_device_reset_pending(control))
        return -EUCLEAN;
    increment(&recovery->status.attempts);
    recovery->status.state = DPPD_DEVICE_RECOVERY_QUIESCING;
    rc = dppd_devices_pause_io(&runtime->devices, expected_sequence);
    if (rc != 0)
        return failed(runtime, rc);
    dppd_runtime_request_stop(runtime);
    rc = dppd_runtime_wait(runtime);
    recovery->status.workers_stopped = !runtime->workers_started;
    if (rc != 0)
        return failed(runtime, rc);
    rc = stable_event(runtime, expected_sequence);
    if (rc != 0)
        return failed(runtime, rc);
    recovery->status.state = DPPD_DEVICE_RECOVERY_CLEANING;
    if (!recovery->cleanup_complete) {
        rc = dppd_control_device_reset_cleanup(control);
        if (rc != 0)
            return failed(runtime, rc);
        recovery->cleanup_complete = true;
    }
    /** 仓库清空后再隔离，复位失败也不会出现拿未知旧句柄清理的窗口 */
    dppd_rte_flow_backend_quarantine(&control->rte_flow);
    recovery->status.state = DPPD_DEVICE_RECOVERY_RESETTING;
    struct dppd_device_reset_result result = {0};
    rc = dppd_devices_reset_reconfigure(&runtime->devices, &runtime->config, expected_sequence, &result);
    recovery->status.device_phase = result.phase;
    recovery->status.stopped_ports = result.stopped_ports;
    recovery->status.reset_ports = result.reset_ports;
    recovery->status.configured_ports = result.configured_ports;
    recovery->status.started_ports = result.started_ports;
    if (rc != 0)
        return failed(runtime, rc);
    increment(&recovery->status.resets);
    rc = wait_links_ready(runtime, result.sequence_after);
    if (rc == 0)
        rc = dppd_topology_rediscover_verified(&runtime->config, &runtime->devices.topology, &candidate);
    if (rc == 0)
        rc = stable_event(runtime, result.sequence_after);
    if (rc != 0)
        return failed(runtime, rc);
    runtime->devices.topology = candidate;
    recovery->status.approved_sequence = result.sequence_after;
    rc = dppd_rte_flow_backend_resume_empty(&control->rte_flow);
    if (rc != 0)
        return failed(runtime, rc);
    recovery->status.state = DPPD_DEVICE_RECOVERY_REBUILDING;
    recovery->cleanup_complete = false;
    rc = dppd_control_device_reset_rebuild(control);
    if (rc != 0)
        return failed(runtime, rc);
    rc = dppd_devices_resume_after_event(&runtime->devices, result.sequence_after);
    if (rc == 0)
        rc = dppd_runtime_start(runtime);
    if (rc == 0)
        rc = wait_workers_ready(runtime);
    if (rc == 0)
        rc = dppd_control_device_reset_commit(control);
    if (rc != 0)
        return failed(runtime, rc);
    recovery->status.planned = false;
    recovery->status.state = DPPD_DEVICE_RECOVERY_RUNNING;
    recovery->status.last_error = recovery->status.cleanup_error = 0;
    recovery->status.workers_stopped = false;
    increment(&recovery->status.recoveries);
    return 0;
}

void dppd_runtime_device_status(const struct dppd_runtime *runtime, struct dppd_device_recovery_status *status)
{
    if (status == NULL)
        return;
    memset(status, 0, sizeof(*status));
    if (runtime == NULL)
        return;
    *status = runtime->device_recovery.status;
    status->workers_stopped = !runtime->workers_started;
    status->nb_ports = runtime->devices.nb_ports;
    for (uint16_t i = 0; i < status->nb_ports && i < DPPD_MAX_PORTS; ++i)
        status->ports[i] = runtime->devices.ports[i].port_id;
    struct dppd_device_event_snapshot event;
    dppd_devices_event_snapshot(&runtime->devices, &event);
    status->sequence = event.sequence;
    status->reset_event_ports = event.reset_ports;
    status->removal_event_ports = event.removal_ports;
    status->destroy_event_ports = event.destroy_ports;
    const struct dppd_control_service *control = runtime->device_recovery.control;
    if (control != NULL) {
        struct dppd_reset_status plan;
        dppd_control_reset_status(control, &plan);
        status->desired_rules = plan.desired_rules;
        status->desired_shared = plan.desired_shared;
        status->actual_rules = plan.actual_rules;
        status->actual_shared = plan.actual_shared;
        status->quarantined = dppd_rte_flow_backend_is_quarantined(&control->rte_flow);
    }
}
