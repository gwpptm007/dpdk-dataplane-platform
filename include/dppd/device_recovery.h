#ifndef DPPD_DEVICE_RECOVERY_H
#define DPPD_DEVICE_RECOVERY_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/config.h"

struct dppd_runtime;
struct dppd_control_service;

enum dppd_device_recovery_state {
    DPPD_DEVICE_RECOVERY_RUNNING = 0,
    DPPD_DEVICE_RECOVERY_QUIESCING,
    DPPD_DEVICE_RECOVERY_CLEANING,
    DPPD_DEVICE_RECOVERY_RESETTING,
    DPPD_DEVICE_RECOVERY_REBUILDING,
    DPPD_DEVICE_RECOVERY_ISOLATED,
};

/** 只读诊断保留真实事件与已批准代次，状态为 running 也不代表已经取得实机验收证据 */
struct dppd_device_recovery_status {
    uint64_t sequence, approved_sequence, attempts, resets, recoveries;
    uint32_t state, device_phase;
    uint32_t reset_event_ports, removal_event_ports, destroy_event_ports;
    uint32_t stopped_ports, reset_ports, configured_ports, started_ports;
    uint32_t desired_rules, desired_shared, actual_rules, actual_shared;
    uint16_t nb_ports, ports[DPPD_MAX_PORTS];
    int32_t last_error, cleanup_error;
    bool bound, planned, workers_stopped, quarantined;
};

/** context 只由串行主线程读写，设备回调不访问 control 或这些指针 */
struct dppd_device_recovery {
    struct dppd_control_service *control;
    struct dppd_device_recovery_status status;
    bool cleanup_complete;
};

/** 在快照重放与 worker 启动前绑定，正式注册设备事件并接通每次驱动调用的门禁 */
int dppd_runtime_device_bind(struct dppd_runtime *runtime, struct dppd_control_service *control);
/** 主循环先处理设备事件，再查询驱动、AGE 或管理请求，未知旧句柄保持隔离 */
int dppd_runtime_device_poll(struct dppd_runtime *runtime);
/** 完整端口组的主动复位，精确事件代次防止针对旧设备观察发起破坏性操作 */
int dppd_runtime_device_reset(struct dppd_runtime *runtime, uint64_t expected_sequence);
void dppd_runtime_device_status(const struct dppd_runtime *runtime, struct dppd_device_recovery_status *status);

static inline const char *dppd_device_recovery_state_name(uint32_t state)
{
    switch (state) {
    case DPPD_DEVICE_RECOVERY_RUNNING:
        return "running";
    case DPPD_DEVICE_RECOVERY_QUIESCING:
        return "quiescing";
    case DPPD_DEVICE_RECOVERY_CLEANING:
        return "cleaning";
    case DPPD_DEVICE_RECOVERY_RESETTING:
        return "resetting";
    case DPPD_DEVICE_RECOVERY_REBUILDING:
        return "rebuilding";
    default:
        return "isolated";
    }
}

#endif
