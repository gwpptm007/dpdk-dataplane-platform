#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/file.h>
#include "../../lib/control/monitor_channel.c"

static int healthy(void *context)
{
    (void)context;
    return 0;
}

static void ready(int fd)
{
    assert(write(fd, "r", 1) == 1);
    close(fd);
}

static void await_ready(int fd)
{
    char byte;
    assert(read(fd, &byte, 1) == 1);
    close(fd);
}

/** 测试业务故意先放开文件锁，但仍保持进程存活，用来验证不能仅凭锁空闲交接 */
static void business(const char *path, const char *state, const char *session, const struct dppd_recovery_record *ports,
    int notification)
{
    struct dppd_recovery_guard guard = {.fd = -1};
    struct dppd_monitor_client client = {.fd = -1};
    assert(dppd_recovery_guard_open(&guard, path, state) == 0);
    assert(dppd_monitor_client_open(&client, session, &guard, ports) == 0);
    assert(
        dppd_recovery_guard_prepare_identity(&guard, 3, "net_tap0", "net_tap", 1, 1, &ports->ports[0].identity) == 0);
    uint64_t attempt;
    assert(dppd_recovery_guard_begin(&guard, 3, 1, 1, &attempt) == 0);
    assert(dppd_monitor_client_check(&client) == 0);
    dppd_recovery_guard_close(&guard);
    ready(notification);
    for (;;)
        pause();
}

int main(void)
{
    char directory[] = "/tmp/dppd-handoff-unit-XXXXXX", path[256], state[256], session[256];
    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/recovery", directory);
    snprintf(state, sizeof(state), "%s/state", directory);
    snprintf(session, sizeof(session), "%s/session", directory);
    struct dppd_recovery_guard guard = {.fd = -1};
    assert(dppd_recovery_guard_open(&guard, path, state) == 0);
    struct stat info;
    assert(fstat(guard.fd, &info) == 0);
    struct dppd_monitor_record record = {.count = 1, .recovery_device = info.st_dev, .recovery_inode = info.st_ino};
    strcpy(record.recovery_path, guard.path);
    record.ports[0].port_id = 3;
    struct stat namespace;
    assert(stat("/proc/self/ns/net", &namespace) == 0);
    record.ports[0].identity = (struct dppd_recovery_identity){.ifindex = 17,
        .ifname = "owned",
        .boot_id = "6c307404-452c-475d-83b2-e5aaf3ae0102",
        .netns_device = namespace.st_dev,
        .netns_inode = namespace.st_ino};
    record.ports[0].identity.netns_inode++;
    assert(same_network_namespace(&record) == -EXDEV);
    record.ports[0].identity.netns_inode--;
    struct dppd_recovery_record ports = {.count = 1};
    ports.ports[0].port_id = 3;
    ports.ports[0].identity = record.ports[0].identity;
    dppd_recovery_guard_close(&guard);
    int notification[2];
    assert(pipe(notification) == 0);
    pid_t monitor = fork();
    assert(monitor >= 0);
    if (monitor == 0) {
        close(notification[0]);
        struct dppd_monitor_store store = {.fd = -1};
        struct dppd_monitor_server server = {.fd = -1};
        assert(dppd_monitor_create(&store, session, &record) == 0);
        assert(dppd_monitor_server_open(&server, &store.record) == 0);
        ready(notification[1]);
        for (;;) {
            assert(dppd_monitor_server_poll(&server, &store.record, healthy, NULL) == 0);
            usleep(1000);
        }
    }
    close(notification[1]);
    await_ready(notification[0]);
    bool locked;
    assert(dppd_monitor_read(session, &record, &locked) == 0 && locked);
    /* 未持锁、只读描述符、其他文件和缺失描述符均不能登记，也不能提前取得恢复权限 */
    int unlocked = open(path, O_RDWR | O_CLOEXEC);
    int readonly = open(path, O_RDONLY | O_CLOEXEC);
    int unrelated = open("/dev/null", O_RDWR | O_CLOEXEC);
    assert(unlocked >= 0 && readonly >= 0 && unrelated >= 0);
    assert(exchange_request(&record, BUSINESS_REGISTER, -1, 0, NULL) < 0);
    assert(exchange_request(&record, BUSINESS_REGISTER, unlocked, 0, NULL) < 0);
    assert(exchange_request(&record, BUSINESS_REGISTER, readonly, 0, NULL) < 0);
    assert(exchange_request(&record, BUSINESS_REGISTER, unrelated, 0, NULL) < 0);
    assert(exchange_request(&record, RECOVERY_ACQUIRE, unlocked, 0, NULL) < 0);
    close(unlocked);
    close(readonly);
    close(unrelated);
    assert(pipe(notification) == 0);
    pid_t writer = fork();
    assert(writer >= 0);
    if (writer == 0) {
        close(notification[0]);
        business(path, state, session, &ports, notification[1]);
        _exit(0);
    }
    close(notification[1]);
    await_ready(notification[0]);
    assert(dppd_recovery_guard_open(&guard, path, NULL) == 0);
    struct dppd_recovery_record before = guard.record;
    uint64_t revision = before.revision;
    int independent = open(path, O_RDWR | O_CLOEXEC);
    struct dppd_recovery_record untrusted;
    assert(independent >= 0 && dppd_recovery_guard_read_locked(independent, path, &untrusted) == -EWOULDBLOCK);
    close(independent);
    struct dppd_monitor_client client = {.fd = -1};
    assert(dppd_monitor_recovery_open(&client, session, &guard, revision + 1) == -ESTALE);
    assert(dppd_monitor_recovery_open(&client, session, &guard, revision) < 0);
    assert(exchange_request(&record, BUSINESS_REGISTER, guard.fd, revision, NULL) < 0);
    assert(exchange_request(&record, BUSINESS_CHECK, -1, 0, NULL) < 0);
    assert(kill(writer, SIGSTOP) == 0);
    int status;
    assert(waitpid(writer, &status, WUNTRACED) == writer && WIFSTOPPED(status));
    client = (struct dppd_monitor_client){.fd = -1};
    assert(dppd_monitor_recovery_open(&client, session, &guard, revision) < 0);
    assert(kill(writer, SIGKILL) == 0);
    /* 尚未回收的僵尸也已经退出，不能依赖 kill(pid, 0) 的存在性判断 */
    siginfo_t death;
    assert(waitid(P_PID, writer, &death, WEXITED | WNOWAIT) == 0);
    assert(exchange_request(&record, RECOVERY_ACQUIRE, guard.fd, revision + 1, NULL) < 0);
    client = (struct dppd_monitor_client){.fd = -1};
    assert(dppd_monitor_recovery_open(&client, session, &guard, revision) == 0);
    assert(waitpid(writer, NULL, 0) == writer);
    assert(dppd_monitor_client_check(&client) == 0);
    client.recovery_revision++;
    assert(dppd_monitor_client_check(&client) < 0);
    client.recovery_revision--;
    assert(dppd_monitor_client_check(&client) < 0);
    dppd_monitor_client_close(&client);
    assert(exchange(&record) == 0);
    client = (struct dppd_monitor_client){.fd = -1};
    assert(dppd_monitor_recovery_open(&client, session, &guard, revision) == 0);
    struct dppd_monitor_client other = {.fd = -1};
    assert(dppd_monitor_recovery_open(&other, session, &guard, revision) < 0);
    struct dppd_recovery_record observed;
    assert(dppd_recovery_guard_read_locked(guard.fd, path, &observed) == 0);
    assert(memcmp(&before, &observed, sizeof(before)) == 0);
    /* 监护借用同一文件描述维持租约，调用方提前关闭不会让另一恢复工具穿过 */
    dppd_recovery_guard_close(&guard);
    assert(dppd_recovery_guard_open(&guard, path, NULL) == -EBUSY);
    assert(dppd_monitor_client_check(&client) == 0);
    dppd_monitor_client_close(&client);
    /* 已绑定客户端关闭后不能被误当成默认禁用状态，也不能复用旧结构继续交接 */
    assert(dppd_monitor_client_check(&client) == -ENOTCONN);
    for (unsigned int retry = 0;; ++retry) {
        int rc = dppd_recovery_guard_open(&guard, path, NULL);
        if (rc == 0)
            break;
        assert(rc == -EBUSY && retry < 1000);
        usleep(1000);
    }
    assert(dppd_monitor_recovery_open(&client, session, &guard, revision) == -EALREADY);
    client = (struct dppd_monitor_client){.fd = -1};
    assert(dppd_monitor_recovery_open(&client, session, &guard, revision) == 0);
    /* 主动解锁使租约失效，重新加锁或监护恢复都不能让旧客户端自动恢复 */
    assert(flock(guard.fd, LOCK_UN) == 0);
    assert(dppd_monitor_client_check(&client) < 0);
    assert(flock(guard.fd, LOCK_EX | LOCK_NB) == 0);
    assert(dppd_monitor_client_check(&client) < 0);
    dppd_monitor_client_close(&client);
    assert(exchange(&record) == 0);
    client = (struct dppd_monitor_client){.fd = -1};
    assert(dppd_monitor_recovery_open(&client, session, &guard, revision) == 0);
    assert(kill(monitor, SIGSTOP) == 0);
    assert(waitpid(monitor, &status, WUNTRACED) == monitor && WIFSTOPPED(status));
    assert(dppd_monitor_client_check(&client) == -ETIMEDOUT);
    assert(kill(monitor, SIGCONT) == 0 && exchange(&record) == 0);
    assert(dppd_monitor_client_check(&client) == -ETIMEDOUT);
    dppd_monitor_client_close(&client);
    assert(exchange(&record) == 0);
    /* 交接工具被强杀后监护释放借用的锁，下一个工具仍须重新完成整套核验 */
    assert(pipe(notification) == 0);
    pid_t recovery = fork();
    assert(recovery >= 0);
    if (recovery == 0) {
        close(notification[0]);
        struct dppd_monitor_client lease = {.fd = -1};
        assert(dppd_monitor_recovery_open(&lease, session, &guard, revision) == 0);
        ready(notification[1]);
        for (;;)
            pause();
    }
    close(notification[1]);
    await_ready(notification[0]);
    dppd_recovery_guard_close(&guard);
    assert(dppd_recovery_guard_open(&guard, path, NULL) == -EBUSY);
    assert(kill(recovery, SIGKILL) == 0 && waitpid(recovery, NULL, 0) == recovery);
    for (unsigned int retry = 0;; ++retry) {
        int rc = dppd_recovery_guard_open(&guard, path, NULL);
        if (rc == 0)
            break;
        assert(rc == -EBUSY && retry < 1000);
        usleep(1000);
    }
    client = (struct dppd_monitor_client){.fd = -1};
    assert(dppd_monitor_recovery_open(&client, session, &guard, revision) == 0);
    dppd_monitor_client_close(&client);
    assert(kill(monitor, SIGKILL) == 0 && waitpid(monitor, NULL, 0) == monitor);
    dppd_recovery_guard_close(&guard);
    assert(dppd_recovery_guard_open(&guard, path, NULL) == 0);
    assert(memcmp(&before, &guard.record, sizeof(before)) == 0);
    dppd_recovery_guard_close(&guard);
    assert(unlink(path) == 0 && unlink(session) == 0 && rmdir(directory) == 0);
    return 0;
}
