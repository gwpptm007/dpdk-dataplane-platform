#ifndef DPPD_RESET_H
#define DPPD_RESET_H

#include <stdbool.h>
#include <stdint.h>

struct dppd_control_service;
struct dppd_reset_plan;

/** 控制计划只记录本地对象进度，真实设备次数由 runtime 单独记录，不能以此证明硬件复位 */
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
/** 正式复位只捕获期望计划并隔离，不调用模拟回调，不修改快照或对象版本 */
int dppd_control_device_reset_prepare(struct dppd_control_service *service);
/** worker 已停止且旧句柄仍有效时调用，按引用顺序清空本进程对象，失败保留计划 */
int dppd_control_device_reset_cleanup(struct dppd_control_service *service);
/** 设备已真实复位并核对身份后重建原计划，不能用此接口绕过设备恢复步骤 */
int dppd_control_device_reset_rebuild(struct dppd_control_service *service);
/** 对象全部重建且 worker 真正就绪后才发布 READY，失败不会丢弃期望计划 */
int dppd_control_device_reset_commit(struct dppd_control_service *service);
bool dppd_control_device_reset_pending(const struct dppd_control_service *service);
/** 非预期设备变化使旧句柄状态未知，封住驱动入口和普通写入，不猜测对象已经清除 */
void dppd_control_device_isolate(struct dppd_control_service *service, int error);
/** 只复制内存中的期望对象数、实际对象数与失败原因，不触碰驱动或快照文件 */
void dppd_control_reset_status(const struct dppd_control_service *service, struct dppd_reset_status *status);

#endif
