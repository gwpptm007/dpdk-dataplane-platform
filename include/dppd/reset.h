#ifndef DPPD_RESET_H
#define DPPD_RESET_H

#include <stdbool.h>
#include <stdint.h>

struct dppd_control_service;
struct dppd_reset_plan;

/** 只记录独立模拟的恢复过程，不把它当作真实设备已经完成复位或重建的证据 */
struct dppd_reset_status {
    uint64_t resets, attempts, recoveries;
    uint32_t desired_rules, desired_shared, actual_rules, actual_shared;
    int last_error, cleanup_error;
    bool pending;
};

/** 仅支持显式提供模拟回调的后端，真实驱动默认拒绝，成功后保持隔离直到显式恢复 */
int dppd_control_simulate_reset(struct dppd_control_service *service);
/** 核对原端点身份，先重建共享对象再重建同版本规则，失败保留计划供下次重试 */
int dppd_control_reset_recover(struct dppd_control_service *service);
/** 只复制内存中的期望对象数、实际对象数与失败原因，不触碰驱动或快照文件 */
void dppd_control_reset_status(const struct dppd_control_service *service, struct dppd_reset_status *status);

#endif
