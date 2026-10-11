#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include "dppd/monitor_channel.h"
#include "dppd/recovery_watch.h"

int __real_dppd_monitor_recovery_open(struct dppd_monitor_client *client, const char *path,
    struct dppd_recovery_guard *guard, uint64_t revision);
int __real_dppd_recovery_watch_check(struct dppd_recovery_watch *watch);

/** 只在独立测试工具取得真实租约后暂停，正式入口不提供暂停或跳过核验的开关 */
int __wrap_dppd_monitor_recovery_open(struct dppd_monitor_client *client, const char *path,
    struct dppd_recovery_guard *guard, uint64_t revision)
{
    int rc = __real_dppd_monitor_recovery_open(client, path, guard, revision);
    if (rc == 0)
        raise(SIGSTOP);
    return rc;
}

/** 只在完整查询窗口返回后暂停，使测试能在最终监护检查前结束监护进程 */
int __wrap_dppd_recovery_watch_check(struct dppd_recovery_watch *watch)
{
    int rc = __real_dppd_recovery_watch_check(watch);
    const char *mode = getenv("DPPD_TEST_HANDOFF_WINDOW");
    if (rc == 0 && mode != NULL && strcmp(mode, "after-query") == 0)
        raise(SIGSTOP);
    return rc;
}
