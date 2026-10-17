#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/wait.h>
#include "../../lib/control/recovery_scope.c"

/** 新线程必须仍可启动，但它不能留下一个不受业务 pidfd 覆盖的独立子进程 */
static void *thread_probe(void *argument)
{
    bool *done = argument;
    errno = 0;
    assert(fork() == -1 && errno == EPERM);
    *done = true;
    return NULL;
}

static void test_filter(bool bootstrap)
{
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        assert(prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) == 0);
        assert(restrict_process(bootstrap) == 0);
        errno = 0;
        assert(fork() == -1 && errno == EPERM);
        errno = 0;
        assert(syscall(SYS_clone, SIGCHLD, NULL, NULL, NULL, 0) == -1 && errno == EPERM);
        errno = 0;
        assert(syscall(SYS_clone, CLONE_THREAD | CLONE_NEWNET, NULL, NULL, NULL, 0) == -1 && errno == EPERM);
#ifdef SYS_clone3
        errno = 0;
        assert(syscall(SYS_clone3, NULL, 0) == -1 && errno == ENOSYS);
#endif
        errno = 0;
        assert(unshare(CLONE_NEWNET) == -1 && errno == EPERM);
        errno = 0;
        assert(setns(-1, CLONE_NEWNET) == -1 && errno == EPERM);
        bool done = false;
        pthread_t thread;
        assert(pthread_create(&thread, NULL, thread_probe, &done) == 0);
        assert(pthread_join(thread, NULL) == 0 && done);
        if (bootstrap) {
            assert(prctl(PR_SET_DUMPABLE, 0L, 0L, 0L, 0L) == 0);
            execl("/bin/true", "true", NULL);
            _exit(97);
        }
        errno = 0;
        assert(prctl(PR_SET_DUMPABLE, 1L, 0L, 0L, 0L) == -1 && errno == EPERM);
        errno = 0;
        assert(prctl(PR_SET_PDEATHSIG, 0L, 0L, 0L, 0L) == -1 && errno == EPERM);
        errno = 0;
        assert(execl("/bin/true", "true", NULL) == -1 && errno == EPERM);
        _exit(0);
    }
    int status;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/** 构造独立的对端回复，验证坏随机数、命名空间错配及会话切换都不会被后续成功洗掉 */
static void test_exchange(unsigned int mode, int expected)
{
    int pair[2];
    assert(dppd_scope_socketpair(pair) == 0);
    pid_t supervisor = getpid();
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(pair[0]);
        struct dppd_recovery_scope scope = {.fd = pair[1],
            .enabled = true,
            .role = DPPD_SCOPE_MONITOR,
            .supervisor = supervisor};
        if (mode == 3)
            assert(dppd_scope_check(&scope) == 0);
        assert(dppd_scope_check(&scope) == expected);
        if (expected != 0)
            assert(dppd_scope_check(&scope) == expected);
        dppd_scope_close(&scope);
        if (expected == 0)
            assert(dppd_scope_check(&scope) == -ENOTCONN);
        _exit(0);
    }
    close(pair[1]);
    struct stat context;
    assert(stat("/proc/self/ns/net", &context) == 0);
    for (unsigned int request = 0; request < (mode == 3 ? 2U : 1U); ++request) {
        uint64_t now;
        assert(now_ms(&now) == 0 && wait_ready(pair[0], POLLIN, now + 2000) == 0);
        unsigned char bytes[DPPD_SCOPE_MESSAGE_SIZE];
        assert(recv(pair[0], bytes, sizeof(bytes), MSG_TRUNC) == sizeof(bytes));
        bytes[32] = (unsigned char)(1 + request);
        put(bytes + 48, context.st_dev, 8);
        put(bytes + 56, context.st_ino + (mode == 2), 8);
        put(bytes + 64, (uint32_t)supervisor, 4);
        put(bytes + 68, (uint32_t)child, 4);
        put(bytes + 72, geteuid(), 4);
        put(bytes + 76, getegid(), 4);
        if (mode == 1)
            bytes[16] ^= 1;
        if (mode == 4)
            bytes[95] = 1;
        if (mode == 5)
            put(bytes + 12, (uint32_t)-EACCES, 4);
        if (mode == 6)
            put(bytes + 12, 1, 4);
        if (mode == 7)
            assert(poll(NULL, 0, DPPD_SCOPE_TIMEOUT_MS + 100) == 0);
        else
            assert(send(pair[0], bytes, sizeof(bytes), MSG_NOSIGNAL) == sizeof(bytes));
    }
    close(pair[0]);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void)
{
    unsetenv(DPPD_SCOPE_ENV);
    struct dppd_recovery_scope scope = {.fd = -1};
    assert(dppd_scope_open(&scope, DPPD_SCOPE_MONITOR) == 0 && !scope.enabled);
    assert(dppd_scope_check(&scope) == 0);
    assert(dppd_scope_check(NULL) == -EINVAL);
    for (unsigned int i = 0; i < 5; ++i) {
        const char *invalid[] = {"", "-1", "0", "3x", "2147483648"};
        scope = (struct dppd_recovery_scope){.fd = -1};
        assert(setenv(DPPD_SCOPE_ENV, invalid[i], 1) == 0);
        assert(dppd_scope_open(&scope, DPPD_SCOPE_MONITOR) == -EINVAL);
        unsetenv(DPPD_SCOPE_ENV);
        assert(dppd_scope_check(&scope) == -EINVAL);
    }
    int pair[2];
    assert(dppd_scope_socketpair(pair) == 0);
    char fd[32];
    snprintf(fd, sizeof(fd), "%d", pair[1]);
    assert(setenv(DPPD_SCOPE_ENV, fd, 1) == 0);
    scope = (struct dppd_recovery_scope){.fd = -1};
    /* 自己创建的通道不是父进程授权，即使当前测试用户为 root 也不能冒充 */
    assert(dppd_scope_open(&scope, DPPD_SCOPE_MONITOR) == -EACCES);
    close(pair[0]);
    close(pair[1]);
    unsetenv(DPPD_SCOPE_ENV);
    scope = (struct dppd_recovery_scope){.fd = -1, .enabled = true};
    assert(dppd_scope_check(&scope) == -ENOTCONN);
    test_filter(true);
    test_filter(false);
    test_exchange(0, 0);
    test_exchange(1, -EPROTO);
    test_exchange(2, -EXDEV);
    test_exchange(3, -ESTALE);
    test_exchange(4, -EPROTO);
    test_exchange(5, -EACCES);
    test_exchange(6, -EPROTO);
    test_exchange(7, -ETIMEDOUT);
    return 0;
}
