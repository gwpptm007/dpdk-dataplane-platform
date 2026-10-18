#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

int __real_fsync(int fd);
int __wrap_fsync(int fd);

/** 仅测试链接的入口在外部标记存在时模拟磁盘故障，正式 daemon 没有这个开关 */
int __wrap_fsync(int fd)
{
    const char *marker = getenv("DPPD_TEST_SYNC_FAILURE");
    if (marker != NULL && access(marker, F_OK) == 0) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}
