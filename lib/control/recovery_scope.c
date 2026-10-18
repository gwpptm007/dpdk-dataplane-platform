#define _GNU_SOURCE
#include "dppd/recovery_scope.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/**
 * 固定 96 字节小端消息，字段不直接借用结构体布局
 * 0 到 31 保存版本、角色、错误和请求随机数，32 到 63 保存作用域编号与网络命名空间
 * 64 到 79 绑定父子 PID、UID、GID，最后 16 字节必须为零，不能悄悄接受其他版本的字段
 */
static const unsigned char magic[8] = {'D', 'P', 'P', 'S', 'C', 'O', 'P', '1'};
static int restrict_process(bool bootstrap);

static void put(unsigned char *bytes, uint64_t value, unsigned int width)
{
    for (unsigned int i = 0; i < width; ++i)
        bytes[i] = (unsigned char)(value >> (8U * i));
}

static uint64_t get(const unsigned char *bytes, unsigned int width)
{
    uint64_t value = 0;
    for (unsigned int i = 0; i < width; ++i)
        value |= (uint64_t)bytes[i] << (8U * i);
    return value;
}

static bool present(const unsigned char bytes[16])
{
    unsigned char value = 0;
    for (unsigned int i = 0; i < 16; ++i)
        value |= bytes[i];
    return value != 0;
}

/** 随机源失败直接拒绝，不用 PID、时钟或旧会话编号充当授权身份 */
static int random_id(unsigned char bytes[16])
{
    ssize_t size = getrandom(bytes, 16, GRND_NONBLOCK);
    return size == 16 && present(bytes) ? 0 : size < 0 ? -errno : -EIO;
}

static int now_ms(uint64_t *value)
{
    struct timespec now;
    /** 时钟失败必须返回非零错误，不能让未写入的时间被当成成功结果 */
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return errno != 0 ? -errno : -EIO;
    *value = (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
    return 0;
}

static int wait_ready(int fd, short events, uint64_t deadline)
{
    for (;;) {
        uint64_t now;
        int rc = now_ms(&now);
        if (rc != 0 || now >= deadline)
            return rc != 0 ? rc : -ETIMEDOUT;
        struct pollfd item = {.fd = fd, .events = events};
        rc = poll(&item, 1, (int)(deadline - now));
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc <= 0)
            return rc < 0 ? -errno : -ETIMEDOUT;
        return (item.revents & events) != 0 ? 0 : -ECONNRESET;
    }
}

/** exec 前处理 UID 切换会清除的权限和父进程死亡信号，业务只保留一个网络能力 */
int dppd_scope_child_prepare(uid_t uid, gid_t gid, bool writer, pid_t supervisor)
{
    if (geteuid() != 0 || uid == 0 || gid == 0 || supervisor <= 0 || getppid() != supervisor)
        return -EACCES;
    struct ucred peer;
    socklen_t peer_size = sizeof(peer);
    if (getsockopt(DPPD_SCOPE_INHERITED_FD, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) != 0)
        return -errno;
    if (peer_size != sizeof(peer) || peer.uid != 0 || peer.pid != supervisor)
        return -EACCES;
    /* 旧命名空间的 socket 也能携带操作目标，不能把它们一起交给有 NET_ADMIN 的子进程 */
    for (int fd = 0; fd < 3; ++fd) {
        struct stat info;
        if (fstat(fd, &info) != 0)
            return -errno;
        if (S_ISSOCK(info.st_mode))
            return -EACCES;
    }
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 4U, ~0U, 0U) != 0)
        return -errno;
#else
    return -ENOSYS;
#endif
    if (prctl(PR_SET_KEEPCAPS, 1L, 0L, 0L, 0L) != 0 || prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0L, 0L, 0L) != 0)
        return -errno;
    for (int cap = 0; cap < 1024; ++cap) {
        int exists = prctl(PR_CAPBSET_READ, cap, 0L, 0L, 0L);
        if (exists < 0) {
            if (errno == EINVAL)
                break;
            return -errno;
        }
        if ((!writer || cap != CAP_NET_ADMIN) && prctl(PR_CAPBSET_DROP, cap, 0L, 0L, 0L) != 0)
            return -errno;
    }
    if (setgroups(0, NULL) != 0 || setresgid(gid, gid, gid) != 0 || setresuid(uid, uid, uid) != 0)
        return -errno;
    struct __user_cap_header_struct header = {.version = _LINUX_CAPABILITY_VERSION_3};
    struct __user_cap_data_struct caps[2] = {{0}, {0}};
    caps[0].effective = caps[0].permitted = caps[0].inheritable = writer ? (1U << CAP_NET_ADMIN) : 0;
    if (syscall(SYS_capset, &header, caps) != 0)
        return -errno;
    if (writer && prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_NET_ADMIN, 0L, 0L) != 0)
        return -errno;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) != 0 || prctl(PR_SET_PDEATHSIG, (long)SIGKILL, 0L, 0L, 0L) != 0)
        return -errno;
    if (getppid() != supervisor)
        return -EOWNERDEAD;
    /* 动态链接器和构造函数执行前就禁止派生进程，入口核验后再禁止后续 exec */
    return restrict_process(true);
}

/** 限制创建独立进程和执行其他程序，线程仍可启动并继承过滤器，退出证明因此覆盖全部写入线程 */
static int restrict_process(bool bootstrap)
{
#if defined(__x86_64__)
#define SCOPE_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define SCOPE_ARCH AUDIT_ARCH_AARCH64
#else
    (void)bootstrap;
    return -ENOTSUP;
#endif
#ifdef SCOPE_ARCH
#define CHECK_CALL(number, action) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (number), 0, 1), BPF_STMT(BPF_RET | BPF_K, (action))
#define REJECT_CALL(number, error) CHECK_CALL(number, SECCOMP_RET_ERRNO | (error))
    const uint32_t threads = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM |
        CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID;
    struct sock_filter instructions[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SCOPE_ARCH, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        /* x32 与本机 syscall 编号不同，不能用同一套编号检查后放行 */
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x40000000U, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
#ifdef SYS_fork
        REJECT_CALL(SYS_fork, EPERM),
#endif
#ifdef SYS_vfork
        REJECT_CALL(SYS_vfork, EPERM),
#endif
#ifdef SYS_clone3
        /* clone3 的参数在指针后面，不能安全检查，让 libc 回退到可检查的 clone */
        REJECT_CALL(SYS_clone3, ENOSYS),
#endif
        CHECK_CALL(SYS_execve, bootstrap ? SECCOMP_RET_ALLOW : SECCOMP_RET_ERRNO | EPERM),
#ifdef SYS_execveat
        CHECK_CALL(SYS_execveat, bootstrap ? SECCOMP_RET_ALLOW : SECCOMP_RET_ERRNO | EPERM),
#endif
        REJECT_CALL(SYS_unshare, EPERM),
        REJECT_CALL(SYS_setns, EPERM),
        REJECT_CALL(SYS_ptrace, EPERM),
        REJECT_CALL(SYS_process_vm_writev, EPERM),
        REJECT_CALL(SYS_capset, EPERM),
#ifdef SYS_io_uring_setup
        REJECT_CALL(SYS_io_uring_setup, EPERM),
#endif
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone, 0, 10),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])),
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, CLONE_THREAD, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_ALU | BPF_AND | BPF_K, ~threads),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0]) + 4),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_prctl, 0, 10),
        BPF_STMT(BPF_LD | BPF_IMM, bootstrap),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 1, 6, 0),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, PR_SET_DUMPABLE, 5, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, PR_SET_PDEATHSIG, 4, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, PR_SET_KEEPCAPS, 3, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, PR_SET_SECUREBITS, 2, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, PR_CAP_AMBIENT, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program = {.len = sizeof(instructions) / sizeof(instructions[0]), .filter = instructions};
    long rc = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &program);
    return rc == 0 ? 0 : rc < 0 ? -errno : -EBUSY;
#undef REJECT_CALL
#undef CHECK_CALL
#undef SCOPE_ARCH
#endif
}

/** 不只信任环境标记，exec 后再次核对全部身份、capability 与不可恢复的权限上限 */
static int seal_process(bool writer, pid_t supervisor)
{
    uid_t real_uid, effective_uid, saved_uid;
    gid_t real_gid, effective_gid, saved_gid;
    if (getresuid(&real_uid, &effective_uid, &saved_uid) != 0 || getresgid(&real_gid, &effective_gid, &saved_gid) != 0)
        return -errno;
    if (real_uid == 0 || real_uid != effective_uid || real_uid != saved_uid || real_gid == 0 ||
        real_gid != effective_gid || real_gid != saved_gid || getgroups(0, NULL) != 0 ||
        prctl(PR_GET_NO_NEW_PRIVS, 0L, 0L, 0L, 0L) != 1)
        return -EACCES;
    struct __user_cap_header_struct header = {.version = _LINUX_CAPABILITY_VERSION_3};
    struct __user_cap_data_struct caps[2];
    if (syscall(SYS_capget, &header, caps) != 0)
        return -errno;
    uint32_t expected = writer ? 1U << CAP_NET_ADMIN : 0;
    if (caps[0].effective != expected || caps[0].permitted != expected || caps[0].inheritable != expected ||
        caps[1].effective != 0 || caps[1].permitted != 0 || caps[1].inheritable != 0)
        return -EACCES;
    for (int cap = 0; cap < 1024; ++cap) {
        int bound = prctl(PR_CAPBSET_READ, cap, 0L, 0L, 0L);
        if (bound < 0) {
            if (errno == EINVAL)
                break;
            return -errno;
        }
        int ambient = prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, cap, 0L, 0L);
        if (bound != (writer && cap == CAP_NET_ADMIN) || ambient != (writer && cap == CAP_NET_ADMIN))
            return -EACCES;
    }
    if (prctl(PR_SET_DUMPABLE, 0L, 0L, 0L, 0L) != 0 || prctl(PR_SET_PDEATHSIG, (long)SIGKILL, 0L, 0L, 0L) != 0)
        return -errno;
    if (getppid() != supervisor)
        return -EOWNERDEAD;
    return restrict_process(false);
}

int dppd_scope_authority_init(struct dppd_scope_authority *authority, uid_t uid, gid_t gid)
{
    if (authority == NULL || geteuid() != 0 || uid == 0 || gid == 0)
        return -EACCES;
    struct stat current, initial;
    if (stat("/proc/self/ns/net", &current) != 0 || stat("/proc/1/ns/net", &initial) != 0)
        return -errno;
    if (current.st_dev == initial.st_dev && current.st_ino == initial.st_ino)
        return -EXDEV;
    *authority = (struct dppd_scope_authority){.uid = uid,
        .gid = gid,
        .supervisor = getpid(),
        .netns_device = current.st_dev,
        .netns_inode = current.st_ino};
    return random_id(authority->id);
}

int dppd_scope_socketpair(int pair[2])
{
    if (pair == NULL)
        return -EINVAL;
    pair[0] = pair[1] = -1;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, pair) != 0)
        return -errno;
    int enabled = 1;
    if (setsockopt(pair[0], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) != 0) {
        int rc = -errno;
        close(pair[0]);
        close(pair[1]);
        pair[0] = pair[1] = -1;
        return rc;
    }
    return 0;
}

int dppd_scope_reply(int fd, const struct dppd_scope_authority *authority, pid_t child, enum dppd_scope_role role,
    int error)
{
    if (authority == NULL || child <= 0 || role < DPPD_SCOPE_MONITOR || role > DPPD_SCOPE_RECOVERY || error > 0 ||
        error < -4095 || geteuid() != 0 || getpid() != authority->supervisor)
        return -EINVAL;
    unsigned char bytes[DPPD_SCOPE_MESSAGE_SIZE];
    union {
        struct cmsghdr alignment;
        unsigned char bytes[CMSG_SPACE(sizeof(struct ucred)) + CMSG_SPACE(8 * sizeof(int))];
    } control;
    struct iovec vector = {.iov_base = bytes, .iov_len = sizeof(bytes)};
    struct msghdr message = {.msg_iov = &vector,
        .msg_iovlen = 1,
        .msg_control = control.bytes,
        .msg_controllen = sizeof(control.bytes)};
    ssize_t received = recvmsg(fd, &message, MSG_DONTWAIT | MSG_TRUNC | MSG_CMSG_CLOEXEC);
    if (received < 0)
        return -errno;
    struct ucred credentials = {0};
    unsigned int count = 0;
    bool rights = false;
    for (struct cmsghdr *item = CMSG_FIRSTHDR(&message); item != NULL; item = CMSG_NXTHDR(&message, item)) {
        if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_CREDENTIALS &&
            item->cmsg_len == CMSG_LEN(sizeof(credentials))) {
            memcpy(&credentials, CMSG_DATA(item), sizeof(credentials));
            count++;
        } else if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_RIGHTS) {
            rights = true;
            size_t size = item->cmsg_len - CMSG_LEN(0);
            for (size_t i = 0; i + sizeof(int) <= size; i += sizeof(int)) {
                int received_fd;
                memcpy(&received_fd, (unsigned char *)CMSG_DATA(item) + i, sizeof(received_fd));
                close(received_fd);
            }
        } else
            rights = true;
    }
    if (received != sizeof(bytes) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 || count != 1 || rights ||
        credentials.pid != child || credentials.uid != authority->uid || credentials.gid != authority->gid ||
        memcmp(bytes, magic, sizeof(magic)) != 0 || get(bytes + 8, 4) != (uint32_t)role || get(bytes + 12, 4) != 0 ||
        !present(bytes + 16))
        return -EACCES;
    for (size_t i = 32; i < sizeof(bytes); ++i)
        if (bytes[i] != 0)
            return -EPROTO;
    put(bytes + 12, (uint32_t)error, 4);
    memcpy(bytes + 32, authority->id, 16);
    put(bytes + 48, authority->netns_device, 8);
    put(bytes + 56, authority->netns_inode, 8);
    put(bytes + 64, (uint32_t)authority->supervisor, 4);
    put(bytes + 68, (uint32_t)child, 4);
    put(bytes + 72, authority->uid, 4);
    put(bytes + 76, authority->gid, 4);
    ssize_t sent = send(fd, bytes, sizeof(bytes), MSG_DONTWAIT | MSG_NOSIGNAL);
    return sent == sizeof(bytes) ? 0 : sent < 0 ? -errno : -EIO;
}

static int scope_exchange(struct dppd_recovery_scope *scope)
{
    unsigned char bytes[DPPD_SCOPE_MESSAGE_SIZE] = {0}, reply[DPPD_SCOPE_MESSAGE_SIZE];
    memcpy(bytes, magic, sizeof(magic));
    put(bytes + 8, scope->role, 4);
    uint64_t started;
    int rc = now_ms(&started);
    if (rc == 0)
        rc = random_id(bytes + 16);
    if (rc == 0)
        rc = wait_ready(scope->fd, POLLOUT, started + DPPD_SCOPE_TIMEOUT_MS);
    if (rc != 0)
        return rc;
    ssize_t size = send(scope->fd, bytes, sizeof(bytes), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (size != sizeof(bytes))
        return size < 0 ? -errno : -EIO;
    rc = wait_ready(scope->fd, POLLIN, started + DPPD_SCOPE_TIMEOUT_MS);
    if (rc != 0)
        return rc;
    size = recv(scope->fd, reply, sizeof(reply), MSG_TRUNC | MSG_DONTWAIT);
    uint64_t finished;
    if (size != sizeof(reply))
        return size < 0 ? -errno : -EBADMSG;
    rc = now_ms(&finished);
    if (rc != 0 || finished >= started + DPPD_SCOPE_TIMEOUT_MS)
        return rc != 0 ? rc : -ETIMEDOUT;
    if (memcmp(bytes, reply, 12) != 0 || memcmp(bytes + 16, reply + 16, 16) != 0 || !present(reply + 32) ||
        get(reply + 64, 4) != (uint32_t)scope->supervisor || get(reply + 68, 4) != (uint32_t)getpid() ||
        get(reply + 72, 4) != geteuid() || get(reply + 76, 4) != getegid())
        return -EPROTO;
    for (size_t i = 80; i < sizeof(reply); ++i)
        if (reply[i] != 0)
            return -EPROTO;
    int32_t error = (int32_t)get(reply + 12, 4);
    if (error != 0)
        return error < 0 && error >= -4095 ? error : -EPROTO;
    struct stat current;
    if (stat("/proc/self/ns/net", &current) != 0)
        return -errno;
    if ((uint64_t)current.st_dev != get(reply + 48, 8) || (uint64_t)current.st_ino != get(reply + 56, 8))
        return -EXDEV;
    if (present(scope->id) &&
        (memcmp(scope->id, reply + 32, 16) != 0 || scope->netns_device != get(reply + 48, 8) ||
            scope->netns_inode != get(reply + 56, 8)))
        return -ESTALE;
    memcpy(scope->id, reply + 32, 16);
    scope->netns_device = current.st_dev;
    scope->netns_inode = current.st_ino;
    return getppid() == scope->supervisor ? 0 : -EOWNERDEAD;
}

int dppd_scope_check(struct dppd_recovery_scope *scope)
{
    if (scope == NULL)
        return -EINVAL;
    if (scope->error != 0)
        return scope->error;
    if (!scope->enabled)
        return 0;
    if (scope->fd < 0)
        return scope->error = -ENOTCONN;
    return scope->error = scope_exchange(scope);
}

int dppd_scope_open(struct dppd_recovery_scope *scope, enum dppd_scope_role role)
{
    if (scope == NULL || role < DPPD_SCOPE_MONITOR || role > DPPD_SCOPE_RECOVERY)
        return -EINVAL;
    if (scope->enabled || scope->error != 0)
        return -EALREADY;
    const char *text = getenv(DPPD_SCOPE_ENV);
    if (text == NULL)
        return 0;
    scope->enabled = true;
    scope->role = role;
    char *end;
    errno = 0;
    long fd = strtol(text, &end, 10);
    if (text[0] < '0' || text[0] > '9' || *end != '\0' || errno != 0 || fd < 3 || fd > INT32_MAX)
        return scope->error = -EINVAL;
    struct ucred peer;
    socklen_t size = sizeof(peer);
    int type;
    socklen_t type_size = sizeof(type);
    if (getsockopt((int)fd, SOL_SOCKET, SO_TYPE, &type, &type_size) != 0 ||
        getsockopt((int)fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0)
        return scope->error = -errno;
    if (size != sizeof(peer) || type_size != sizeof(type) || type != SOCK_SEQPACKET || peer.uid != 0 || peer.pid <= 0 ||
        peer.pid != getppid())
        return scope->error = -EACCES;
    scope->supervisor = peer.pid;
    scope->fd = fcntl((int)fd, F_DUPFD_CLOEXEC, 3);
    if (scope->fd < 0)
        return scope->error = -errno;
    close((int)fd);
    unsetenv(DPPD_SCOPE_ENV);
    int rc = seal_process(role != DPPD_SCOPE_MONITOR, scope->supervisor);
    if (rc == 0)
        rc = dppd_scope_check(scope);
    return scope->error = rc;
}

void dppd_scope_close(struct dppd_recovery_scope *scope)
{
    if (scope != NULL && scope->enabled && scope->fd >= 0) {
        close(scope->fd);
        scope->fd = -1;
    }
}
