#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <sys/wait.h>
#include "../../lib/control/monitor_channel.c"

static int healthy(void *context)
{
    return *(int *)context;
}

static int counted(void *context)
{
    ++*(unsigned int *)context;
    return 0;
}

/** 坏消息只关闭自己的连接，静默占用槽位的客户端到期后释放，正常请求仍先检查事件 */
static void server_boundaries(void)
{
    struct dppd_monitor_record record = {.recovery_device = 2, .recovery_inode = 3};
    assert(getrandom(record.session, sizeof(record.session), 0) == sizeof(record.session));
    struct dppd_monitor_server server = {.fd = -1};
    assert(dppd_monitor_server_open(&server, &record) == 0);
    struct sockaddr_un address;
    socklen_t length = address_for(record.session, &address);
    const unsigned char nonce[16] = {1};
    unsigned int checks = 0;
    for (unsigned int fault = 0; fault <= 7; ++fault) {
        unsigned char request[MESSAGE_SIZE], reply[MESSAGE_SIZE];
        make_request(&record, nonce, request);
        const size_t offsets[] = {0, 8, 12, 16, 32, 48, 56};
        if (fault < 7)
            request[offsets[fault]] ^= 1;
        int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        assert(fd >= 0 && connect(fd, (struct sockaddr *)&address, length) == 0);
        assert(send(fd, request, sizeof(request), MSG_NOSIGNAL) == sizeof(request));
        assert(dppd_monitor_server_poll(&server, &record, counted, &checks) == 0);
        ssize_t received = recv(fd, reply, sizeof(reply), MSG_DONTWAIT);
        assert(received == (fault == 7 ? (ssize_t)sizeof(reply) : 0));
        assert(checks == (fault == 7 ? 1U : 0U));
        close(fd);
    }
    int idle[DPPD_MONITOR_CLIENT_LIMIT];
    for (unsigned int index = 0; index < DPPD_MONITOR_CLIENT_LIMIT; ++index) {
        idle[index] = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        assert(idle[index] >= 0 && connect(idle[index], (struct sockaddr *)&address, length) == 0);
    }
    assert(dppd_monitor_server_poll(&server, &record, counted, &checks) == 0 && checks == 1);
    for (unsigned int index = 0; index < DPPD_MONITOR_CLIENT_LIMIT; ++index) {
        assert(server.clients[index] >= 0);
        server.expires[index] = 0;
    }
    assert(dppd_monitor_server_poll(&server, &record, counted, &checks) == 0 && checks == 1);
    for (unsigned int index = 0; index < DPPD_MONITOR_CLIENT_LIMIT; ++index) {
        assert(server.clients[index] == -1);
        close(idle[index]);
    }
    dppd_monitor_server_close(&server);
}

/** 真正的子进程持有会话锁和 socket，错误回复只在这个测试服务端中构造 */
static void serve(const char *path, struct dppd_monitor_record *record, int ready, int mode)
{
    struct dppd_monitor_store store = {.fd = -1};
    struct dppd_monitor_server server = {.fd = -1};
    assert(dppd_monitor_create(&store, path, record) == 0);
    assert(dppd_monitor_server_open(&server, &store.record) == 0);
    assert(write(ready, "r", 1) == 1);
    close(ready);
    if (mode == 1)
        raise(SIGSTOP);
    int status = mode == 6 ? -ESTALE : 0;
    for (;;) {
        if (mode == 0 || mode == 1 || mode == 6) {
            if (dppd_monitor_server_poll(&server, &store.record, healthy, &status) != 0)
                _exit(0);
        } else {
            int fd = accept4(server.fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd >= 0) {
                unsigned char request[MESSAGE_SIZE + 1];
                uint64_t now;
                assert(milliseconds(&now) == 0);
                assert(wait_ready(fd, POLLIN, now + DPPD_MONITOR_REPLY_MS) == 0);
                assert(recv(fd, request, MESSAGE_SIZE, 0) == MESSAGE_SIZE);
                size_t length = MESSAGE_SIZE;
                if (mode == 2)
                    request[32] ^= 1;
                if (mode == 3)
                    request[16] ^= 1;
                if (mode == 4)
                    length--;
                if (mode == 5)
                    request[length++] = 0;
                assert(send(fd, request, length, MSG_NOSIGNAL) == (ssize_t)length);
                close(fd);
            }
        }
        usleep(1000);
    }
}

static void run_case(int mode)
{
    char directory[] = "/tmp/dppd-channel-unit-XXXXXX", guard_path[256], state[256], path[256];
    assert(mkdtemp(directory) != NULL);
    snprintf(guard_path, sizeof(guard_path), "%s/recovery", directory);
    snprintf(state, sizeof(state), "%s/state", directory);
    snprintf(path, sizeof(path), "%s/session", directory);
    struct dppd_recovery_guard guard = {.fd = -1};
    struct dppd_monitor_client client = {.fd = -1};
    assert(dppd_recovery_guard_open(&guard, guard_path, state) == 0);
    struct stat info;
    assert(fstat(guard.fd, &info) == 0);
    struct dppd_monitor_record record = {.count = 1, .recovery_device = info.st_dev, .recovery_inode = info.st_ino};
    strcpy(record.recovery_path, guard.path);
    record.ports[0].port_id = 3;
    record.ports[0].identity = (struct dppd_recovery_identity){.ifindex = 17,
        .ifname = "owned",
        .boot_id = "6c307404-452c-475d-83b2-e5aaf3ae0102",
        .netns_device = 4,
        .netns_inode = 5};
    struct dppd_recovery_record ports = {.count = 1};
    ports.ports[0].port_id = 3;
    ports.ports[0].identity = record.ports[0].identity;
    int ready[2];
    assert(pipe(ready) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(ready[0]);
        close(guard.fd);
        serve(path, &record, ready[1], mode);
        _exit(0);
    }
    close(ready[1]);
    char byte;
    assert(read(ready[0], &byte, 1) == 1);
    close(ready[0]);
    uint64_t started, finished;
    assert(milliseconds(&started) == 0);
    int rc = dppd_monitor_client_open(&client, path, &guard, &ports);
    assert(milliseconds(&finished) == 0 && finished - started < 2500);
    if (mode != 0) {
        int expected = mode == 1 ? -ETIMEDOUT : mode == 2 || mode == 3 ? -EPROTO : -EBADMSG;
        assert(mode == 6 ? rc < 0 : rc == expected);
        assert(guard.record.format == 5 && guard.record.revision == 0 && !guard.monitor_bound);
        assert(client.fd == -1 && dppd_monitor_client_check(&client) == rc);
    } else {
        assert(rc == 0 && guard.record.format == 6 && guard.monitor_bound && guard.record.revision == 1);
        assert(memcmp(guard.record.monitor_session, client.record.session, 16) == 0);
        for (unsigned int index = 0; index < 3; ++index)
            assert(dppd_monitor_client_check(&client) == 0);
        /* 同一服务端被暂停仍持有文件锁，但新请求必须超时，恢复后也不能清除已锁存的错误 */
        assert(kill(child, SIGSTOP) == 0);
        int status;
        assert(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status));
        assert(dppd_monitor_client_check(&client) == -ETIMEDOUT);
        assert(kill(child, SIGCONT) == 0);
        assert(exchange(&client.record) == 0);
        assert(dppd_monitor_client_check(&client) == -ETIMEDOUT);
        /* 新客户端只能在新的干净基线上启动，不能复用首次绑定之前的起点 */
        struct dppd_monitor_client other = {.fd = -1};
        assert(dppd_monitor_client_open(&other, path, &guard, &ports) == -EXDEV);
        assert(dppd_recovery_guard_clean(&guard) == 0 && !guard.monitor_bound);
        assert(guard.record.format == 6 && !dppd_tap_cookie_present(guard.record.monitor_session));
        assert(dppd_recovery_guard_prepare_identity(&guard, 3, "net_tap0", "net_tap", 1, 1,
                   &record.ports[0].identity) == -EACCES);
    }
    dppd_monitor_client_close(&client);
    if (kill(child, SIGKILL) != 0)
        assert(errno == ESRCH);
    assert(waitpid(child, NULL, 0) == child);
    dppd_recovery_guard_close(&guard);
    assert(dppd_recovery_guard_open(&guard, guard_path, state) == 0);
    assert(guard.record.format == (mode == 0 ? 6U : 5U));
    dppd_recovery_guard_close(&guard);
    assert(unlink(path) == 0 && unlink(guard_path) == 0 && rmdir(directory) == 0);
}

/** 相同长度也不能伪造身份，未终止的输入必须在字符串比较前拒绝 */
static void identities(void)
{
    struct dppd_monitor_client client = {.fd = 1, .record.count = 1};
    client.record.ports[0].port_id = 3;
    struct dppd_recovery_identity identity = {.ifindex = 17, .ifname = "owned", .boot_id = "boot", .netns_inode = 2};
    client.record.ports[0].identity = identity;
    assert(dppd_monitor_client_identity(&client, 3, &identity) == 0);
    identity.netns_inode++;
    assert(dppd_monitor_client_identity(&client, 3, &identity) == -EXDEV);
    identity.netns_inode--;
    assert(dppd_monitor_client_identity(&client, 3, &identity) == -EXDEV);
    client.error = 0;
    memset(identity.ifname, 'x', sizeof(identity.ifname));
    assert(dppd_monitor_client_identity(&client, 3, &identity) == -EXDEV);
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) == 0);
    assert(peer_matches(sockets[0], (uint32_t)getpid()) == 0);
    assert(peer_matches(sockets[0], (uint32_t)getpid() + 1) == -EACCES);
    close(sockets[0]);
    close(sockets[1]);
}

int main(void)
{
    server_boundaries();
    identities();
    for (int mode = 0; mode <= 6; ++mode)
        run_case(mode);
    return 0;
}
