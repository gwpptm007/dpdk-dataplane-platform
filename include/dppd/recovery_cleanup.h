#ifndef DPPD_RECOVERY_CLEANUP_H
#define DPPD_RECOVERY_CLEANUP_H

#include "dppd/recovery_scope.h"
#include "dppd/recovery_guard.h"

/** 只在活着的受控作用域和原监护租约中删除，结果保存到独立日志，最后才清除恢复保护 */
int dppd_recovery_cleanup(struct dppd_recovery_guard *guard, const char *session, struct dppd_recovery_scope *scope);
/** 精确删除一个已核验的本地 flower，对端仍须具备恢复角色的实时授权 */
int dppd_recovery_delete_filter(struct dppd_recovery_scope *scope, const struct dppd_recovery_port *port,
    const struct dppd_recovery_filter *filter);
bool dppd_recovery_cleanup_retryable(int error);

#endif
