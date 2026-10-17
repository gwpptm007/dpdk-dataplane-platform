#ifndef DPPD_RESET_INTERNAL_H
#define DPPD_RESET_INTERNAL_H

struct dppd_control_service;

/** 只在实际后端清理成功后销毁内存恢复计划，删除失败时仍保留重试线索 */
void dppd_control_reset_plan_destroy(struct dppd_control_service *service);

#endif
