#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <poll.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "dppd/recovery_scope.h"
#include "dppd/recovery_guard.h"

/** 测试始终先新建网络命名空间，只创建命名空间里的 TAP，不接管任何宿主机设备 */
static int create_tap(const char *name, uid_t uid)
{
    int fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    struct ifreq request = {.ifr_flags = IFF_TAP | IFF_NO_PI | IFF_MULTI_QUEUE};
    snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", name);
    int rc = 0;
    if (ioctl(fd, TUNSETIFF, &request) != 0 || ioctl(fd, TUNSETOWNER, uid) != 0 || ioctl(fd, TUNSETPERSIST, 1) != 0)
        rc = -errno;
    close(fd);
    return rc;
}

static int child_exec(char **argv, enum dppd_scope_role role, const struct passwd *user, pid_t supervisor)
{
    char executable[4096], recovery[4096], snapshot[4096], session[4096], control[4096], lcores[128];
    const char *name = role == DPPD_SCOPE_MONITOR ? "dppd-monitor"
        : role == DPPD_SCOPE_BUSINESS             ? "dppd"
                                                  : "dppd-recovery";
    if (snprintf(executable, sizeof(executable), "%s/%s", argv[2], name) >= (int)sizeof(executable) ||
        snprintf(recovery, sizeof(recovery), "%s/recovery", argv[3]) >= (int)sizeof(recovery) ||
        snprintf(snapshot, sizeof(snapshot), "%s/snapshot", argv[3]) >= (int)sizeof(snapshot) ||
        snprintf(session, sizeof(session), "%s/session", argv[3]) >= (int)sizeof(session) ||
        snprintf(control, sizeof(control), "%s/control", argv[3]) >= (int)sizeof(control))
        return -ENAMETOOLONG;
    cpu_set_t available;
    if (sched_getaffinity(0, sizeof(available), &available) != 0)
        return -errno;
    int first = -1, second = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &available))
            continue;
        if (first < 0)
            first = cpu;
        else {
            second = cpu;
            break;
        }
    }
    if (second < 0)
        return -ENOSPC;
    snprintf(lcores, sizeof(lcores), "--lcores=0@%d,1@%d", first, second);
    int rc = dppd_scope_child_prepare(user->pw_uid, user->pw_gid, role != DPPD_SCOPE_MONITOR, supervisor);
    if (rc != 0)
        return rc;
    errno = 0;
    if (fcntl(64, F_GETFD) != -1 || errno != EBADF)
        return -EACCES;
    if (role == DPPD_SCOPE_MONITOR)
        execl(executable, name, "run", recovery, snapshot, session, "0:dppdscope0", "1:dppdscope1", NULL);
    else if (role == DPPD_SCOPE_RECOVERY)
        execl(executable, name, "plan", recovery, "0", NULL);
    else
        execl(executable, name, lcores, "--main-lcore=0", "--no-huge", "--no-pci", "-m", "64", "--no-telemetry",
            "--file-prefix=dppd-scope-fixture", "--vdev=net_tap0,iface=dppdscope0", "--vdev=net_tap1,iface=dppdscope1",
            "--", "--ports", "0,1", "--queues", "1", "--mbufs", "8191", "--cache", "0", "--duration", "1",
            "--state-path", snapshot, "--control-socket", control, NULL);
    return -errno;
}

int main(int argc, char **argv)
{
    if (argc != 5 || geteuid() != 0) {
        fprintf(stderr, "usage: %s ROLE BUILD_DIRECTORY PRIVATE_DIRECTORY MODE (requires root)\n", argv[0]);
        return 2;
    }
    enum dppd_scope_role role = strcmp(argv[1], "monitor") == 0 ? DPPD_SCOPE_MONITOR
        : strcmp(argv[1], "business") == 0                      ? DPPD_SCOPE_BUSINESS
        : strcmp(argv[1], "recovery") == 0                      ? DPPD_SCOPE_RECOVERY
                                                                : 0;
    const struct passwd *user = getpwnam("nobody");
    if (role == 0 || user == NULL || user->pw_uid == 0 || user->pw_gid == 0 || unshare(CLONE_NEWNET) != 0)
        return 3;
    struct dppd_scope_authority authority;
    if (dppd_scope_authority_init(&authority, user->pw_uid, user->pw_gid) != 0 ||
        create_tap("dppdscope0", user->pw_uid) != 0 || create_tap("dppdscope1", user->pw_uid) != 0)
        return 4;
    if (chown(argv[3], user->pw_uid, user->pw_gid) != 0)
        return 5;
    if (role == DPPD_SCOPE_RECOVERY) {
        char path[4096], state[4096];
        if (snprintf(path, sizeof(path), "%s/recovery", argv[3]) >= (int)sizeof(path) ||
            snprintf(state, sizeof(state), "%s/snapshot", argv[3]) >= (int)sizeof(state))
            return 6;
        struct dppd_recovery_guard guard = {.fd = -1};
        if (dppd_recovery_guard_open(&guard, path, state) != 0)
            return 7;
        dppd_recovery_guard_close(&guard);
        if (chown(path, user->pw_uid, user->pw_gid) != 0)
            return 8;
    }
    int pair[2];
    if (dppd_scope_socketpair(pair) != 0)
        return 9;
    /* 故意留下一个未设置 CLOEXEC 的描述符，核对降权入口确实清理继承资源 */
    int inherited = open("/dev/null", O_RDONLY);
    if (inherited < 0 || dup2(inherited, 64) < 0)
        return 9;
    close(inherited);
    pid_t supervisor = getpid();
    pid_t child = fork();
    if (child < 0)
        return 10;
    if (child == 0) {
        close(pair[0]);
        if (dup2(pair[1], 3) < 0 || fcntl(3, F_SETFD, 0) != 0 || setenv(DPPD_SCOPE_ENV, "3", 1) != 0)
            _exit(11);
        if (pair[1] != 3)
            close(pair[1]);
        int rc = child_exec(argv, role, user, supervisor);
        fprintf(stderr, "scope fixture exec failed: %d\n", rc);
        _exit(12);
    }
    close(pair[1]);
    close(64);
    printf("scope-fixture child=%u role=%u supervisor=%u\n", (unsigned int)child, role, (unsigned int)getpid());
    fflush(stdout);
    if (strcmp(argv[4], "wrong-namespace") == 0)
        authority.netns_inode++;
    unsigned int requests = 0;
    time_t started = time(NULL);
    int status = 0;
    for (;;) {
        pid_t finished = waitpid(child, &status, WNOHANG);
        if (finished == child)
            break;
        if (finished < 0 || time(NULL) - started > 15) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            close(pair[0]);
            return 13;
        }
        struct pollfd item = {.fd = pair[0], .events = POLLIN};
        int ready = poll(&item, 1, 50);
        if (ready > 0 && (item.revents & POLLIN) != 0) {
            int error = strcmp(argv[4], "denied") == 0 ? -EACCES : 0;
            enum dppd_scope_role expected = strcmp(argv[4], "wrong-role") == 0
                ? (role == DPPD_SCOPE_MONITOR ? DPPD_SCOPE_BUSINESS : DPPD_SCOPE_MONITOR)
                : role;
            int rc = dppd_scope_reply(pair[0], &authority, child, expected, error);
            if (rc != 0 && rc != -EAGAIN) {
                close(pair[0]);
                pair[0] = -1;
            }
            if (rc == 0 && ++requests == 3 && role == DPPD_SCOPE_MONITOR && strcmp(argv[4], "hold") != 0)
                kill(child, SIGTERM);
        }
    }
    close(pair[0]);
    printf("scope-fixture requests=%u status=%d\n", requests, status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
