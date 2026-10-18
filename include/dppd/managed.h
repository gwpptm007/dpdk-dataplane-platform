#ifndef DPPD_MANAGED_H
#define DPPD_MANAGED_H

#include "dppd/config.h"
#include "dppd/recovery_scope.h"

/** EAL 初始化后进入独占管理模式，返回退出码，EAL 清理由正式入口统一负责 */
int dppd_managed_run(const struct dppd_config *config, struct dppd_recovery_scope *scope);

#endif
