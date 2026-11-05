#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int __real_fsync(int fd);
int __wrap_fsync(int fd);

/** 仅测试链接的入口在外部标记存在时模拟磁盘故障，正式 daemon 没有这个开关 */
int __wrap_fsync(int fd)
{
    static unsigned int observed;
    const char *marker = getenv("DPPD_TEST_SYNC_FAILURE");
    if (marker != NULL && access(marker, F_OK) == 0) {
        /** 空文件仍然立即失败，after=N 允许先保存 pending，再让实际发布后的保存失败 */
        unsigned int allowed = 0;
        FILE *input = fopen(marker, "r");
        if (input != NULL) {
            if (fscanf(input, "after=%u", &allowed) != 1 || allowed > 16)
                allowed = 0;
            fclose(input);
        }
        if (observed < allowed) {
            ++observed;
            return __real_fsync(fd);
        }
        errno = EIO;
        return -1;
    }
    /** 移除标记后重新计数，同一个测试进程可以开始另一轮独立故障 */
    observed = 0;
    return __real_fsync(fd);
}
