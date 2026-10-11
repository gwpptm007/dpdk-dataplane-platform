#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dppd/recovery_inspect.h"
#include "dppd/recovery_monitor.h"
#include "dppd/recovery_watch.h"
#include "dppd/monitor_channel.h"

static volatile sig_atomic_t stopping;

static void stop_requested(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static int usage(const char *program)
{
    fprintf(stderr, "usage: %s run RECOVERY_PATH STATE_PATH SESSION_PATH PORT:IFNAME [PORT:IFNAME ...]\n", program);
    fprintf(stderr, "       %s show SESSION_PATH\n", program);
    return EXIT_FAILURE;
}

/** 显式给出业务端口到预先创建的 TAP 映射，监护工具本身不创建或删除接口 */
static int parse_ports(int count, char **arguments, struct dppd_recovery_record *ports)
{
    ports->count = (uint32_t)count;
    for (int index = 0; index < count; ++index) {
        char *end;
        const char *text = arguments[index];
        if (text[0] < '0' || text[0] > '9')
            return -EINVAL;
        errno = 0;
        unsigned long id = strtoul(text, &end, 10);
        if (errno != 0 || id > UINT16_MAX || *end != ':' || end[1] == '\0' ||
            strlen(end + 1) >= DPPD_RECOVERY_IFNAME_SIZE)
            return -EINVAL;
        struct dppd_recovery_port *port = &ports->ports[index];
        port->port_id = (uint16_t)id;
        strcpy(port->identity.ifname, end + 1);
        port->identity.ifindex = if_nametoindex(end + 1);
        if (port->identity.ifindex == 0)
            return -ENODEV;
        for (int other = 0; other < index; ++other)
            if (ports->ports[other].port_id == port->port_id ||
                ports->ports[other].identity.ifindex == port->identity.ifindex)
                return -EINVAL;
    }
    return 0;
}

/** 保留原文件的描述符防止 inode 被回收复用，但释放安装锁让独立业务进程可以启动 */
static int recovery_file_unchanged(int fd, const struct dppd_monitor_record *record)
{
    struct stat held, named;
    if (fstat(fd, &held) != 0 || lstat(record->recovery_path, &named) != 0)
        return -errno;
    if (!S_ISREG(named.st_mode) || held.st_nlink != 1 || (held.st_mode & 077) != 0 ||
        (uint64_t)held.st_dev != record->recovery_device || (uint64_t)held.st_ino != record->recovery_inode ||
        held.st_dev != named.st_dev || held.st_ino != named.st_ino)
        return -ESTALE;
    return 0;
}

struct monitor_context {
    int binding_fd;
    struct dppd_recovery_watch *watch;
    const struct dppd_monitor_record *record;
    enum dppd_monitor_reason reason;
};

/** 空闲轮询和每次握手共用检查，不能先回复成功再处理已经收到的删除通知 */
static int check_session(void *context)
{
    struct monitor_context *monitor = context;
    int rc = recovery_file_unchanged(monitor->binding_fd, monitor->record);
    if (rc != 0) {
        monitor->reason = DPPD_MONITOR_RECORD_REPLACED;
        return rc;
    }
    rc = dppd_recovery_watch_check(monitor->watch);
    if (rc != 0)
        monitor->reason = monitor->watch->error != 0 ? DPPD_MONITOR_OBSERVER_LOST : DPPD_MONITOR_LINK_CHANGED;
    return rc;
}

/** 此进程不依赖业务进程的父子关系，业务退出后仍持续接收接口生命周期事件 */
static int run_monitor(int argc, char **argv)
{
    struct dppd_recovery_guard guard = {.fd = -1};
    struct dppd_recovery_record ports = {0};
    struct dppd_recovery_watch watch = {.fd = -1};
    struct dppd_monitor_store store = {.fd = -1};
    struct dppd_monitor_server server = {.fd = -1};
    struct dppd_monitor_record record = {0};
    int binding_fd = -1;
    int rc = parse_ports(argc - 5, argv + 5, &ports);
    if (rc != 0)
        return rc;
    rc = dppd_recovery_guard_open(&guard, argv[2], argv[3]);
    if (rc != 0)
        return rc;
    /* 不能在已有未确认活动后补开新会话，让缺失的历史看起来重新连续 */
    if (guard.record.count != 0 || guard.record.attempt_count != 0) {
        rc = -EUCLEAN;
        goto done;
    }
    rc = dppd_recovery_watch_open_lifetime(&watch, &ports);
    if (rc != 0)
        goto done;
    record.count = ports.count;
    for (uint32_t index = 0; index < ports.count; ++index) {
        record.ports[index].port_id = ports.ports[index].port_id;
        rc = dppd_recovery_tap_identity(ports.ports[index].identity.ifindex, &record.ports[index].identity);
        if (rc == 0 && strcmp(record.ports[index].identity.ifname, ports.ports[index].identity.ifname) != 0)
            rc = -EXDEV;
        if (rc != 0)
            goto done;
    }
    rc = dppd_recovery_watch_check(&watch);
    if (rc != 0)
        goto done;
    struct stat info;
    if (fstat(guard.fd, &info) != 0) {
        rc = -errno;
        goto done;
    }
    record.recovery_device = (uint64_t)info.st_dev;
    record.recovery_inode = (uint64_t)info.st_ino;
    record.baseline_revision = guard.record.revision;
    record.baseline_attempt = guard.record.last_attempt;
    strcpy(record.recovery_path, guard.path);
    binding_fd = fcntl(guard.fd, F_DUPFD_CLOEXEC, 3);
    if (binding_fd < 0) {
        rc = -errno;
        goto done;
    }
    rc = dppd_monitor_create(&store, argv[4], &record);
    if (rc != 0)
        goto done;
    rc = dppd_monitor_server_open(&server, &store.record);
    if (rc != 0) {
        dppd_monitor_finish(&store, DPPD_MONITOR_OBSERVER_LOST, rc);
        goto done;
    }
    if (flock(guard.fd, LOCK_UN) != 0) {
        rc = -errno;
        dppd_monitor_finish(&store, DPPD_MONITOR_OBSERVER_LOST, rc);
        goto done;
    }
    dppd_recovery_guard_close(&guard);
    printf("monitor ready session=");
    for (unsigned int byte = 0; byte < sizeof(store.record.session); ++byte)
        printf("%02x", store.record.session[byte]);
    printf(" ports=%u baseline-revision=%" PRIu64 " mode=interface-lifetime writer-exclusion=no executable=no\n",
        record.count, record.baseline_revision);
    fflush(stdout);

    struct monitor_context context = {.binding_fd = binding_fd,
        .watch = &watch,
        .record = &store.record,
        .reason = DPPD_MONITOR_STOP_REQUESTED};
    while (!stopping) {
        struct pollfd events[2 + DPPD_MONITOR_CLIENT_LIMIT] = {{.fd = watch.fd, .events = POLLIN},
            {.fd = server.fd, .events = POLLIN}};
        bool available = false;
        for (unsigned int index = 0; index < DPPD_MONITOR_CLIENT_LIMIT; ++index) {
            events[index + 2] = (struct pollfd){.fd = server.clients[index], .events = POLLIN};
            available |= server.clients[index] < 0;
        }
        /* 槽位已满时等待现有连接或到期检查，避免监听队列反复唤醒形成空转 */
        if (!available)
            events[1].events = 0;
        int ready = poll(events, 2 + DPPD_MONITOR_CLIENT_LIMIT, 250);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || (events[0].revents & (POLLNVAL | POLLHUP)) != 0 ||
            (events[1].revents & (POLLNVAL | POLLHUP | POLLERR)) != 0) {
            rc = ready < 0 ? -errno : -EIO;
            context.reason = DPPD_MONITOR_OBSERVER_LOST;
            break;
        }
        rc = check_session(&context);
        if (rc == 0 && (events[0].revents & POLLERR) != 0)
            rc = watch.error = -EIO;
        if (rc == 0)
            rc = dppd_monitor_server_poll(&server, &store.record, check_session, &context);
        if (rc != 0) {
            if (context.reason == DPPD_MONITOR_STOP_REQUESTED)
                context.reason = DPPD_MONITOR_OBSERVER_LOST;
            break;
        }
    }
    int saved = dppd_monitor_finish(&store, context.reason, rc != 0 ? rc : -ECANCELED);
    if (saved != 0)
        rc = saved;
done:
    dppd_monitor_server_close(&server);
    dppd_monitor_close(&store);
    dppd_recovery_watch_close(&watch);
    dppd_recovery_guard_close(&guard);
    if (binding_fd >= 0)
        close(binding_fd);
    return rc;
}

/** 磁盘活动标记必须结合当前文件锁解释，PID 和旧记录本身都不是存活证明 */
static int show_monitor(const char *path)
{
    struct dppd_monitor_record record;
    bool locked;
    int rc = dppd_monitor_read(path, &record, &locked);
    if (rc != 0)
        return rc;
    const char *state = "invalid";
    if (record.state == DPPD_MONITOR_ACTIVE)
        state = locked ? "active" : "interrupted";
    else if (record.state == DPPD_MONITOR_STOPPED)
        state = "stopped";
    bool interrupted = record.state == DPPD_MONITOR_ACTIVE && !locked;
    printf("monitor state=%s sequence=%" PRIu64
           " reason=%s error=%d writer-lock=%s freshness=unverified executable=no\n",
        state, record.sequence, interrupted ? "writer-missing" : dppd_monitor_reason_name(record.reason),
        interrupted ? -EOWNERDEAD : record.error, locked ? "held" : "free");
    printf("session=");
    for (unsigned int byte = 0; byte < sizeof(record.session); ++byte)
        printf("%02x", record.session[byte]);
    printf(" recorded-pid=%u started-ns=%" PRIu64 " updated-ns=%" PRIu64 "\n", record.pid, record.started_ns,
        record.updated_ns);
    printf("recovery=%s device=%" PRIu64 " inode=%" PRIu64 " baseline-revision=%" PRIu64 " baseline-attempt=%" PRIu64
           "\n",
        record.recovery_path, record.recovery_device, record.recovery_inode, record.baseline_revision,
        record.baseline_attempt);
    for (uint32_t index = 0; index < record.count; ++index) {
        const struct dppd_recovery_identity *identity = &record.ports[index].identity;
        printf("port=%u ifindex=%u ifname=%s boot=%s netns-device=%" PRIu64 " netns-inode=%" PRIu64 "\n",
            record.ports[index].port_id, identity->ifindex, identity->ifname, identity->boot_id, identity->netns_device,
            identity->netns_inode);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int rc;
    if (argc == 3 && strcmp(argv[1], "show") == 0)
        rc = show_monitor(argv[2]);
    else if (argc >= 6 && argc <= 5 + (int)DPPD_MAX_PORTS && strcmp(argv[1], "run") == 0) {
        struct sigaction action = {.sa_handler = stop_requested};
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0)
            rc = -errno;
        else
            rc = run_monitor(argc, argv);
    } else
        return usage(argv[0]);
    if (rc != 0)
        fprintf(stderr, "recovery monitor failed: %s (%d)\n", strerror(-rc), rc);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
