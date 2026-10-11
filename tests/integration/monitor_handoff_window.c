#include <signal.h>
#include "dppd/monitor_channel.h"

int __real_dppd_monitor_recovery_open(struct dppd_monitor_client *client, const char *path,
    struct dppd_recovery_guard *guard, uint64_t revision);

/** 只在独立测试工具取得真实租约后暂停，正式入口不提供暂停或跳过核验的开关 */
int __wrap_dppd_monitor_recovery_open(struct dppd_monitor_client *client, const char *path,
    struct dppd_recovery_guard *guard, uint64_t revision)
{
    int rc = __real_dppd_monitor_recovery_open(client, path, guard, revision);
    if (rc == 0)
        raise(SIGSTOP);
    return rc;
}
