#define _GNU_SOURCE
#include "dppd/recovery_scope.h"
#include "dppd/recovery_guard.h"
#include "dppd/recovery_monitor.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef DPPD_SCOPE_RECOVERY_PROGRAM
#define DPPD_SCOPE_RECOVERY_PROGRAM "dppd-recovery"
#endif
#ifndef DPPD_SCOPE_BUSINESS_PROGRAM
#define DPPD_SCOPE_BUSINESS_PROGRAM "dppd"
#endif

struct actor {
    pid_t pid;
    int fd, pidfd, status;
    bool done;
    unsigned int requests;
    uint64_t replied;
};

/** root 只管理自己创建的空间和进程，业务与清理进程的权限从不同时有效 */
struct manager {
    struct dppd_scope_authority authority;
    struct actor actors[3];
    char directory[4096], programs[4096], recovery[4096], snapshot[4096], session[4096], control[4096];
    int lock, directory_fd;
    uint64_t epoch;
    bool native, monitor_stopping;
};

static volatile sig_atomic_t stopping;

static void stop_requested(int number)
{
    (void)number;
    stopping = 1;
}

static uint64_t now_ms(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0;
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static int number(const char *text, uint32_t *value)
{
    char *end;
    if (*text < '1' || *text > '9')
        return -EINVAL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed >= UINT32_MAX)
        return -EINVAL;
    *value = (uint32_t)parsed;
    return 0;
}

/** 不接受符号链接目录，私有目录和 root 锁都在整个启动器生命周期内保持同一 inode */
static int directory_check(const struct manager *manager)
{
    struct stat held, named, lock, lock_name;
    if (fstat(manager->directory_fd, &held) != 0 || lstat(manager->directory, &named) != 0 ||
        fstat(manager->lock, &lock) != 0 ||
        fstatat(manager->directory_fd, "scope.lock", &lock_name, AT_SYMLINK_NOFOLLOW) != 0)
        return -errno;
    if (!S_ISDIR(named.st_mode) || held.st_dev != named.st_dev || held.st_ino != named.st_ino ||
        held.st_uid != manager->authority.uid || held.st_gid != manager->authority.gid ||
        (held.st_mode & 07777) != 0700 || !S_ISREG(lock.st_mode) || lock.st_uid != 0 || lock.st_nlink != 1 ||
        (lock.st_mode & 07777) != 0600 || lock.st_dev != lock_name.st_dev || lock.st_ino != lock_name.st_ino)
        return -ESTALE;
    return 0;
}

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

static void close_actor(struct actor *actor)
{
    if (actor->fd >= 0)
        close(actor->fd);
    if (actor->pidfd >= 0)
        close(actor->pidfd);
    *actor = (struct actor){.fd = -1, .pidfd = -1, .done = true};
}

static int signal_actor(const struct actor *actor, int number)
{
    if (actor->done || actor->pidfd < 0)
        return 0;
    return syscall(SYS_pidfd_send_signal, actor->pidfd, number, NULL, 0) == 0 ? 0 : -errno;
}

/** 只执行同一构建目录内的固定角色，不能通过命令参数传入实体端口或任意设备 */
static int exec_actor(const struct manager *manager, enum dppd_scope_role role, uint64_t revision)
{
    const char *program = role == DPPD_SCOPE_MONITOR ? "dppd-monitor"
        : role == DPPD_SCOPE_BUSINESS                ? DPPD_SCOPE_BUSINESS_PROGRAM
                                                     : DPPD_SCOPE_RECOVERY_PROGRAM;
    char executable[4096], version[32], lcores[96], prefix[96];
    if (snprintf(executable, sizeof(executable), "%s/%s", manager->programs, program) >= (int)sizeof(executable))
        return -ENAMETOOLONG;
    snprintf(version, sizeof(version), "%" PRIu64, revision);
    snprintf(prefix, sizeof(prefix), "--file-prefix=scope-%u-%" PRIu64, (unsigned int)manager->authority.supervisor,
        manager->epoch);
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
    /* 动态库目录属于管理员信任配置，清除预加载和测试开关等其他继承环境 */
    const char *configured = getenv("LD_LIBRARY_PATH");
    char *libraries = configured != NULL ? strdup(configured) : NULL;
    if (configured != NULL && libraries == NULL)
        return -ENOMEM;
    if (clearenv() != 0 || setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1) != 0 || setenv("LC_ALL", "C", 1) != 0 ||
        setenv(DPPD_SCOPE_ENV, "3", 1) != 0 || setenv("XDG_RUNTIME_DIR", manager->directory, 1) != 0 ||
        (libraries != NULL && setenv("LD_LIBRARY_PATH", libraries, 1) != 0)) {
        free(libraries);
        return -errno;
    }
    free(libraries);
    int rc = dppd_scope_child_prepare(manager->authority.uid, manager->authority.gid, role != DPPD_SCOPE_MONITOR,
        manager->authority.supervisor);
    if (rc != 0)
        return rc;
    if (role == DPPD_SCOPE_MONITOR)
        execl(executable, program, "run", manager->recovery, manager->snapshot, manager->session, "0:dppdscope0",
            "1:dppdscope1", NULL);
    else if (role == DPPD_SCOPE_RECOVERY)
        execl(executable, program, "clean-managed", manager->recovery, version, manager->session, NULL);
    else {
        char *arguments[] = {executable, lcores, "--main-lcore=0", "--no-huge", "--no-pci", "-m", "64",
            "--no-telemetry", prefix, "--vdev=net_tap0,iface=dppdscope0", "--vdev=net_tap1,iface=dppdscope1", "--",
            "--ports", "0,1", "--queues", "1", "--mbufs", "8191", "--cache", "0", "--state-path",
            (char *)manager->snapshot, "--recovery-path", (char *)manager->recovery, "--recovery-monitor",
            (char *)manager->session, "--control-socket", (char *)manager->control, NULL, NULL};
        if (manager->native)
            arguments[sizeof(arguments) / sizeof(arguments[0]) - 2] = "--tap-owner-cookie";
        execv(executable, arguments);
    }
    return -errno;
}

static int spawn(struct manager *manager, enum dppd_scope_role role, uint64_t revision)
{
    struct actor *actor = &manager->actors[role - 1];
    if (!actor->done)
        return -EBUSY;
    close_actor(actor);
    int pair[2];
    int rc = dppd_scope_socketpair(pair);
    if (rc != 0)
        return rc;
    pid_t child = fork();
    if (child == 0) {
        close(pair[0]);
        if (dup2(pair[1], DPPD_SCOPE_INHERITED_FD) < 0 || fcntl(DPPD_SCOPE_INHERITED_FD, F_SETFD, 0) != 0)
            _exit(126);
        rc = exec_actor(manager, role, revision);
        fprintf(stderr, "scope child exec failed role=%u error=%d\n", role, rc);
        _exit(126);
    }
    close(pair[1]);
    if (child < 0) {
        rc = -errno;
        close(pair[0]);
        return rc;
    }
    *actor = (struct actor){.pid = child, .fd = pair[0], .pidfd = (int)syscall(SYS_pidfd_open, child, 0)};
    if (actor->pidfd < 0) {
        rc = -errno;
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        actor->done = true;
        return rc;
    }
    printf("scope-actor role=%u pid=%u epoch=%" PRIu64 "\n", role, (unsigned int)child, manager->epoch);
    fflush(stdout);
    return 0;
}

/** 等待 pidfd 和授权请求放在同一个事件循环，不能等待退出时让活进程的授权超时 */
static int pump(struct manager *manager)
{
    int rc = directory_check(manager);
    if (rc != 0)
        return rc;
    struct pollfd items[6];
    for (unsigned int i = 0; i < 3; ++i) {
        struct actor *actor = &manager->actors[i];
        items[2 * i] = (struct pollfd){.fd = actor->done ? -1 : actor->fd, .events = POLLIN};
        items[2 * i + 1] = (struct pollfd){.fd = actor->done ? -1 : actor->pidfd, .events = POLLIN};
    }
    int ready = poll(items, 6, 50);
    if (ready < 0 && errno != EINTR)
        return -errno;
    /* 先处理整个线程组的退出，再决定是否允许另一个写入角色 */
    for (unsigned int i = 0; i < 3; ++i) {
        struct actor *actor = &manager->actors[i];
        if (actor->done)
            continue;
        pid_t ended = waitpid(actor->pid, &actor->status, WNOHANG);
        if (ended < 0)
            return -errno;
        if (ended != 0) {
            actor->done = true;
            printf("scope-exit role=%u pid=%u status=%d\n", i + 1, (unsigned int)actor->pid, actor->status);
            fflush(stdout);
        }
    }
    struct actor *monitor = &manager->actors[DPPD_SCOPE_MONITOR - 1];
    if (monitor->done && !manager->monitor_stopping)
        return -EOWNERDEAD;
    uint64_t now = now_ms();
    if (now == 0)
        return -EIO;
    for (unsigned int i = 0; i < 3; ++i) {
        struct actor *actor = &manager->actors[i];
        if (actor->done || (items[2 * i].revents & POLLIN) == 0)
            continue;
        /* 正常退出可能先关闭通道，关闭本端后仍须等 pidfd，不能据此宣称线程组已退出 */
        unsigned char peek;
        ssize_t pending = recv(actor->fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
        if (pending == 0) {
            close(actor->fd);
            actor->fd = -1;
            continue;
        }
        if (pending < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            return -errno;
        }
        int allowed = 0;
        if (i != DPPD_SCOPE_MONITOR - 1 &&
            (manager->monitor_stopping || monitor->done || monitor->requests < 2 ||
                now - monitor->replied >= DPPD_SCOPE_TIMEOUT_MS))
            allowed = -EOWNERDEAD;
        if ((i == DPPD_SCOPE_RECOVERY - 1 && !manager->actors[DPPD_SCOPE_BUSINESS - 1].done) ||
            (i == DPPD_SCOPE_BUSINESS - 1 && !manager->actors[DPPD_SCOPE_RECOVERY - 1].done))
            allowed = -EBUSY;
        rc = dppd_scope_reply(actor->fd, &manager->authority, actor->pid, (enum dppd_scope_role)(i + 1), allowed);
        if (rc != 0)
            return rc;
        actor->requests++;
        actor->replied = now_ms();
    }
    return 0;
}

static int wait_actor(struct manager *manager, enum dppd_scope_role role, uint64_t deadline)
{
    while (!manager->actors[role - 1].done) {
        if (now_ms() >= deadline)
            return -ETIMEDOUT;
        int rc = pump(manager);
        if (rc != 0)
            return rc;
    }
    return 0;
}

static int start_monitor(struct manager *manager)
{
    manager->monitor_stopping = false;
    manager->epoch++;
    char identifier[33];
    for (unsigned int i = 0; i < 16; ++i)
        snprintf(identifier + 2 * i, 3, "%02x", manager->authority.id[i]);
    if (snprintf(manager->session, sizeof(manager->session), "%s/session-%s-%" PRIu64, manager->directory, identifier,
            manager->epoch) >= (int)sizeof(manager->session))
        return -ENAMETOOLONG;
    int rc = spawn(manager, DPPD_SCOPE_MONITOR, 0);
    uint64_t deadline = now_ms() + 8000;
    struct dppd_monitor_record record;
    bool locked;
    while (rc == 0 && now_ms() < deadline && !stopping) {
        rc = pump(manager);
        struct actor *monitor = &manager->actors[DPPD_SCOPE_MONITOR - 1];
        if (rc == 0 && monitor->requests >= 2 && dppd_monitor_read(manager->session, &record, &locked) == 0 && locked &&
            record.state == DPPD_MONITOR_ACTIVE && record.pid == (uint32_t)monitor->pid)
            return 0;
    }
    return rc != 0 ? rc : stopping ? -ECANCELED : -ETIMEDOUT;
}

static int stop_monitor(struct manager *manager)
{
    manager->monitor_stopping = true;
    int rc = signal_actor(&manager->actors[DPPD_SCOPE_MONITOR - 1], SIGTERM);
    return rc != 0 ? rc : wait_actor(manager, DPPD_SCOPE_MONITOR, now_ms() + 3000);
}

/** 业务退出后才读保护文件，重试只处理死亡清理进程尚未释放到监护端的共享锁 */
static int recovery_state(struct manager *manager, uint64_t *revision, bool *dirty)
{
    struct dppd_recovery_guard *guard = calloc(1, sizeof(*guard));
    if (guard == NULL)
        return -ENOMEM;
    uint64_t deadline = now_ms() + 3000;
    int rc;
    do {
        rc = dppd_recovery_guard_open(guard, manager->recovery, NULL);
        if (rc != -EBUSY)
            break;
        rc = pump(manager);
    } while (rc == 0 && now_ms() < deadline);
    if (rc == 0 && guard->fd >= 0) {
        *revision = guard->record.revision;
        *dirty = guard->record.count != 0 || guard->record.attempt_count != 0;
    } else if (rc == 0)
        rc = -ETIMEDOUT;
    dppd_recovery_guard_close(guard);
    free(guard);
    return rc;
}

/** 只有有效干净记录才允许新业务，有限重试不会跨越监护死亡或新的网络作用域 */
static int recover(struct manager *manager)
{
    for (unsigned int attempt = 0; attempt < 3 && !stopping; ++attempt) {
        uint64_t revision;
        bool dirty;
        int rc = recovery_state(manager, &revision, &dirty);
        if (rc != 0 || !dirty)
            return rc;
        rc = spawn(manager, DPPD_SCOPE_RECOVERY, revision);
        if (rc == 0)
            rc = wait_actor(manager, DPPD_SCOPE_RECOVERY, now_ms() + 30000);
        if (rc != 0)
            return rc;
        int status = manager->actors[DPPD_SCOPE_RECOVERY - 1].status;
        rc = recovery_state(manager, &revision, &dirty);
        if (rc != 0 || !dirty)
            return rc;
        bool retry =
            (WIFEXITED(status) && WEXITSTATUS(status) == 75) || (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        if (!retry)
            return -EUCLEAN;
        printf("scope-cleanup-retry attempt=%u revision=%" PRIu64 "\n", attempt + 1, revision);
        fflush(stdout);
    }
    return stopping ? -ECANCELED : -EAGAIN;
}

static int remove_control(const struct manager *manager)
{
    struct stat info;
    if (lstat(manager->control, &info) != 0)
        return errno == ENOENT ? 0 : -errno;
    if (!S_ISSOCK(info.st_mode) || info.st_uid != manager->authority.uid)
        return -ESTALE;
    return unlink(manager->control) == 0 ? 0 : -errno;
}

static int run(struct manager *manager)
{
    int rc = start_monitor(manager);
    unsigned int restarts = 0;
    while (rc == 0 && !stopping) {
        rc = remove_control(manager);
        if (rc == 0)
            rc = spawn(manager, DPPD_SCOPE_BUSINESS, 0);
        struct actor *business = &manager->actors[DPPD_SCOPE_BUSINESS - 1];
        bool sent = false;
        uint64_t deadline = 0;
        while (rc == 0 && !business->done) {
            if (stopping && !sent) {
                rc = signal_actor(business, SIGINT);
                sent = true;
                deadline = now_ms() + 5000;
            }
            if (sent && now_ms() >= deadline) {
                rc = -ETIMEDOUT;
                break;
            }
            if (rc == 0)
                rc = pump(manager);
        }
        if (rc != 0 || stopping)
            break;
        if (WIFEXITED(business->status) && WEXITSTATUS(business->status) == 0)
            break;
        if (restarts++ >= 3) {
            rc = -ELOOP;
            break;
        }
        rc = recover(manager);
        if (rc == 0)
            rc = stop_monitor(manager);
        if (rc == 0)
            rc = start_monitor(manager);
        if (rc == 0)
            printf("scope-replay epoch=%" PRIu64 " snapshot=%s\n", manager->epoch, manager->snapshot);
    }
    if (rc == 0)
        rc = stop_monitor(manager);
    return rc;
}

/** 旧作用域丢失时不领养残留，必须先由外部核对旧记录，不能新建空间后补造连续历史 */
static int prepare(struct manager *manager, char **argv)
{
    uint32_t uid, gid;
    if (geteuid() != 0 || number(argv[3], &uid) != 0 || number(argv[4], &gid) != 0)
        return -EACCES;
    if (argv[2][0] != '/' || strlen(argv[2]) > 60 || realpath(argv[2], manager->directory) == NULL ||
        strcmp(argv[2], manager->directory) != 0)
        return -EINVAL;
    manager->authority.uid = uid;
    manager->authority.gid = gid;
    manager->directory_fd = open(manager->directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (manager->directory_fd < 0)
        return -errno;
    manager->lock =
        openat(manager->directory_fd, "scope.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (manager->lock < 0 || flock(manager->lock, LOCK_EX | LOCK_NB) != 0)
        return -errno;
    int rc = directory_check(manager);
    if (rc != 0)
        return rc;
    if (snprintf(manager->recovery, sizeof(manager->recovery), "%s/recovery", manager->directory) >=
            (int)sizeof(manager->recovery) ||
        snprintf(manager->snapshot, sizeof(manager->snapshot), "%s/snapshot", manager->directory) >=
            (int)sizeof(manager->snapshot) ||
        snprintf(manager->control, sizeof(manager->control), "%s/control", manager->directory) >=
            (int)sizeof(manager->control))
        return -ENAMETOOLONG;
    ssize_t size = readlink("/proc/self/exe", manager->programs, sizeof(manager->programs) - 1);
    if (size <= 0 || size >= (ssize_t)sizeof(manager->programs) - 1)
        return -ENAMETOOLONG;
    manager->programs[size] = '\0';
    char *slash = strrchr(manager->programs, '/');
    if (slash == NULL)
        return -EINVAL;
    *slash = '\0';
    struct stat info;
    if (lstat(manager->recovery, &info) == 0) {
        struct dppd_recovery_guard *guard = calloc(1, sizeof(*guard));
        if (guard == NULL)
            return -ENOMEM;
        rc = dppd_recovery_guard_open(guard, manager->recovery, NULL);
        if (rc == 0 &&
            (guard->record.count != 0 || guard->record.attempt_count != 0 ||
                strcmp(guard->record.state_path, manager->snapshot) != 0))
            rc = -EUCLEAN;
        dppd_recovery_guard_close(guard);
        free(guard);
        if (rc != 0)
            return rc;
    } else if (errno != ENOENT)
        return -errno;
    if (unshare(CLONE_NEWNET) != 0)
        return -errno;
    rc = dppd_scope_authority_init(&manager->authority, uid, gid);
    if (rc == 0)
        rc = create_tap("dppdscope0", uid);
    if (rc == 0)
        rc = create_tap("dppdscope1", uid);
    return rc;
}

int main(int argc, char **argv)
{
    if ((argc != 5 && argc != 6) || strcmp(argv[1], "run") != 0 || (argc == 6 && strcmp(argv[5], "--native") != 0)) {
        fprintf(stderr, "usage: %s run PRIVATE_ABSOLUTE_DIRECTORY UID GID [--native] (requires root)\n", argv[0]);
        return EXIT_FAILURE;
    }
    struct manager *manager = calloc(1, sizeof(*manager));
    if (manager == NULL)
        return EXIT_FAILURE;
    manager->lock = manager->directory_fd = -1;
    manager->native = argc == 6;
    for (unsigned int i = 0; i < 3; ++i)
        manager->actors[i] = (struct actor){.fd = -1, .pidfd = -1, .done = true};
    struct sigaction action = {.sa_handler = stop_requested};
    sigemptyset(&action.sa_mask);
    int rc = sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0 ? -errno : 0;
    if (rc == 0)
        rc = prepare(manager, argv);
    if (rc == 0)
        rc = run(manager);
    /* 出错只终止自己的角色，保留原恢复记录和日志供核对，不伪造清理完成 */
    for (unsigned int i = 0; i < 3; ++i)
        signal_actor(&manager->actors[i], SIGKILL);
    for (unsigned int i = 0; i < 3; ++i) {
        struct actor *actor = &manager->actors[i];
        if (!actor->done && actor->pid > 0)
            waitpid(actor->pid, NULL, 0);
        close_actor(actor);
    }
    if (manager->lock >= 0)
        close(manager->lock);
    if (manager->directory_fd >= 0)
        close(manager->directory_fd);
    if (rc != 0)
        fprintf(stderr, "managed scope stopped: %s (%d)\n", strerror(-rc), rc);
    free(manager);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
