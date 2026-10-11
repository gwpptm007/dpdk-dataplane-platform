#include <errno.h>
#include <linux/rtnetlink.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "dppd/recovery_inspect.h"

int __real_dppd_recovery_inspect(const struct dppd_recovery_port *, struct dppd_recovery_inspection *);
ssize_t __real_recvmsg(int, struct msghdr *, int);

/** 仅测试入口在首次读取后暂停，方便脚本精确制造查询中途的变化 */
int __wrap_dppd_recovery_inspect(const struct dppd_recovery_port *port, struct dppd_recovery_inspection *inspection)
{
    static bool paused;
    int rc = __real_dppd_recovery_inspect(port, inspection);
    if (!paused && getenv("DPPD_TEST_PLAN_WINDOW") != NULL) {
        paused = true;
        raise(SIGSTOP);
    }
    return rc;
}

/** 只对事件订阅 socket 注入丢包，普通内核 GET 仍走真实接收路径 */
ssize_t __wrap_recvmsg(int fd, struct msghdr *message, int flags)
{
    const char *mode = getenv("DPPD_TEST_PLAN_WINDOW");
    struct sockaddr_nl address = {0};
    socklen_t size = sizeof(address);
    if (mode != NULL && strcmp(mode, "lost") == 0 && getsockname(fd, (struct sockaddr *)&address, &size) == 0 &&
        size == sizeof(address) && address.nl_family == AF_NETLINK && (address.nl_groups & RTMGRP_TC) != 0) {
        errno = ENOBUFS;
        return -1;
    }
    return __real_recvmsg(fd, message, flags);
}

/* 共用正式 CLI，暂停和故障包装只链接到此测试程序 */
#include "../../app/dppd_recovery/main.c"
