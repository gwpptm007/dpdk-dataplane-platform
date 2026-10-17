#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int __real_fsync(int fd);

/** 只在测试版 daemon 中消费自有目录的故障标记，正式程序没有故障参数或特殊文件 */
int __wrap_fsync(int fd)
{
    char descriptor[64], path[PATH_MAX], marker[PATH_MAX], mode[16] = {0};
    struct stat info;
    snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
    ssize_t length = readlink(descriptor, path, sizeof(path) - 1);
    if (length <= 0 || fstat(fd, &info) != 0)
        return __real_fsync(fd);
    path[length] = '\0';
    bool directory = S_ISDIR(info.st_mode);
    if (!directory) {
        char *base = strrchr(path, '/');
        if (base == NULL || strncmp(base + 1, "routing.bin.", 12) != 0)
            return __real_fsync(fd);
        *base = '\0';
    }
    if (snprintf(marker, sizeof(marker), "%s/route-store-fault", path) >= (int)sizeof(marker))
        return __real_fsync(fd);
    int fault = open(marker, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fault < 0)
        return __real_fsync(fd);
    ssize_t size = read(fault, mode, sizeof(mode) - 1);
    close(fault);
    if (size > 0 && strcmp(mode, directory ? "directory" : "file") == 0 && unlink(marker) == 0) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}
