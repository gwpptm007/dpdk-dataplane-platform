#include <errno.h>
#include <signal.h>
#include "dppd/recovery_watch.h"

int __real_dppd_recovery_watch_check(struct dppd_recovery_watch *watch);
int __wrap_dppd_recovery_watch_check(struct dppd_recovery_watch *watch);

/** 会话落盘并宣布就绪后暂停，恢复执行时只给监护读取注入事件丢失 */
int __wrap_dppd_recovery_watch_check(struct dppd_recovery_watch *watch)
{
    static unsigned int checks;
    if (++checks == 2) {
        raise(SIGSTOP);
        return watch->error = -ENOBUFS;
    }
    return __real_dppd_recovery_watch_check(watch);
}
