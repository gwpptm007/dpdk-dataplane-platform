#define _GNU_SOURCE
#include "dppd/monitor_channel.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MESSAGE_SIZE 72U
enum request_type { PROBE = 1, BUSINESS_REGISTER, BUSINESS_CHECK, RECOVERY_ACQUIRE, RECOVERY_CHECK };
static const unsigned char channel_magic[8] = {'D', 'P', 'P', 'M', 'I', 'P', 'C', '2'};

static void write_number(unsigned char *bytes, uint64_t value, unsigned int width)
{
    for (unsigned int index = 0; index < width; ++index)
        bytes[index] = (unsigned char)(value >> (8U * index));
}

static uint64_t read_number(const unsigned char *bytes, unsigned int width)
{
    uint64_t value = 0;
    for (unsigned int index = 0; index < width; ++index)
        value |= (uint64_t)bytes[index] << (8U * index);
    return value;
}

static int milliseconds(uint64_t *value)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -errno;
    *value = (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
    return 0;
}

/** 抽象 socket 跟随进程关闭，不留下可被下次启动误用的磁盘 socket 路径 */
static socklen_t address_for(const uint8_t session[16], struct sockaddr_un *address)
{
    *address = (struct sockaddr_un){.sun_family = AF_UNIX};
    memcpy(address->sun_path + 1, "dppd-monitor-", 13);
    for (unsigned int index = 0; index < 16; ++index)
        snprintf(address->sun_path + 14 + index * 2, 3, "%02x", session[index]);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 46);
}

/** 只支持相同有效用户，身份来自内核连接凭据，不能相信请求里自报的 PID */
static int peer_matches(int fd, uint32_t pid)
{
    struct ucred peer;
    socklen_t size = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0)
        return -errno;
    if (size != sizeof(peer) || peer.uid != geteuid() || (pid != 0 && (uint32_t)peer.pid != pid))
        return -EACCES;
    return 0;
}

/** 整个往返共用绝对截止时间，分段接收或进程暂停不会重新获得一秒预算 */
static int wait_ready(int fd, short events, uint64_t deadline)
{
    for (;;) {
        uint64_t now;
        int rc = milliseconds(&now);
        if (rc != 0)
            return rc;
        if (now >= deadline)
            return -ETIMEDOUT;
        struct pollfd item = {.fd = fd, .events = events};
        rc = poll(&item, 1, (int)(deadline - now));
        /* 普通信号可以打断等待，但不能重置截止时间，也不应被误报为监护失联 */
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc < 0)
            return -errno;
        if (rc == 0)
            return -ETIMEDOUT;
        return (item.revents & events) != 0 ? 0 : -ECONNRESET;
    }
}

static void make_request(const struct dppd_monitor_record *record, const unsigned char nonce[16],
    unsigned char bytes[MESSAGE_SIZE])
{
    memset(bytes, 0, MESSAGE_SIZE);
    memcpy(bytes, channel_magic, 8);
    write_number(bytes + 8, 1, 4);
    memcpy(bytes + 16, record->session, 16);
    memcpy(bytes + 32, nonce, 16);
    write_number(bytes + 48, record->recovery_device, 8);
    write_number(bytes + 56, record->recovery_inode, 8);
}

/** 每次请求使用新随机数，旧回复或其他会话的回复即使格式正确也不能通过 */
static int exchange_request(const struct dppd_monitor_record *record, enum request_type type, int guard_fd,
    uint64_t revision, int *lease_fd)
{
    unsigned char request[MESSAGE_SIZE], response[MESSAGE_SIZE], nonce[16];
    uint64_t started, finished;
    int rc = milliseconds(&started);
    if (rc != 0)
        return rc;
    ssize_t received = getrandom(nonce, sizeof(nonce), GRND_NONBLOCK);
    if (received != sizeof(nonce))
        return received < 0 ? -errno : -EIO;
    if (!dppd_tap_cookie_present(nonce))
        return -EIO;
    make_request(record, nonce, request);
    write_number(request + 8, type, 4);
    write_number(request + 64, revision, 8);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -errno;
    struct sockaddr_un address;
    socklen_t length = address_for(record->session, &address);
    if (connect(fd, (struct sockaddr *)&address, length) != 0) {
        rc = -errno;
        goto done;
    }
    rc = peer_matches(fd, record->pid);
    if (rc == 0)
        rc = wait_ready(fd, POLLOUT, started + DPPD_MONITOR_REPLY_MS);
    if (rc != 0)
        goto done;
    struct iovec payload = {.iov_base = request, .iov_len = sizeof(request)};
    union {
        struct cmsghdr alignment;
        unsigned char bytes[CMSG_SPACE(sizeof(int))];
    } control = {0};
    struct msghdr message = {.msg_iov = &payload, .msg_iovlen = 1};
    if (guard_fd >= 0) {
        message.msg_control = control.bytes;
        message.msg_controllen = sizeof(control.bytes);
        struct cmsghdr *item = CMSG_FIRSTHDR(&message);
        item->cmsg_level = SOL_SOCKET;
        item->cmsg_type = SCM_RIGHTS;
        item->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(item), &guard_fd, sizeof(guard_fd));
    }
    ssize_t sent = sendmsg(fd, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (sent != sizeof(request)) {
        rc = sent < 0 ? -errno : -EIO;
        goto done;
    }
    rc = wait_ready(fd, POLLIN, started + DPPD_MONITOR_REPLY_MS);
    if (rc != 0)
        goto done;
    received = recv(fd, response, sizeof(response), MSG_TRUNC | MSG_DONTWAIT);
    if (received != sizeof(response)) {
        rc = received < 0 ? -errno : -EBADMSG;
        goto done;
    }
    rc = milliseconds(&finished);
    if (rc == 0 && finished >= started + DPPD_MONITOR_REPLY_MS)
        rc = -ETIMEDOUT;
    if (rc == 0 && memcmp(request, response, sizeof(request)) != 0)
        rc = -EPROTO;
done:
    if (rc == 0 && lease_fd != NULL)
        *lease_fd = fd;
    else
        close(fd);
    return rc;
}

static int exchange(const struct dppd_monitor_record *record)
{
    return exchange_request(record, PROBE, -1, 0, NULL);
}

int dppd_monitor_server_open(struct dppd_monitor_server *server, const struct dppd_monitor_record *record)
{
    if (server == NULL || record == NULL || !dppd_tap_cookie_present(record->session))
        return -EINVAL;
    if (server->fd >= 0)
        return -EALREADY;
    *server = (struct dppd_monitor_server){.fd = -1,
        .business_pidfd = -1,
        .recovery_fd = -1,
        .recovery_guard = -1,
        .recovery_pidfd = -1};
    for (unsigned int index = 0; index < DPPD_MONITOR_CLIENT_LIMIT; ++index)
        server->clients[index] = -1;
    server->fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (server->fd < 0)
        return -errno;
    struct sockaddr_un address;
    socklen_t length = address_for(record->session, &address);
    if (bind(server->fd, (struct sockaddr *)&address, length) != 0 ||
        listen(server->fd, DPPD_MONITOR_CLIENT_LIMIT) != 0) {
        int rc = -errno;
        dppd_monitor_server_close(server);
        return rc;
    }
    return 0;
}

/** 只接受一个恢复文件描述符，截断或多余描述符全部关闭，防止坏请求耗尽监护资源 */
static ssize_t receive_request(int fd, unsigned char request[MESSAGE_SIZE], int *guard_fd)
{
    union {
        struct cmsghdr alignment;
        unsigned char bytes[CMSG_SPACE(4 * sizeof(int))];
    } control = {0};
    struct iovec payload = {.iov_base = request, .iov_len = MESSAGE_SIZE};
    struct msghdr message = {.msg_iov = &payload,
        .msg_iovlen = 1,
        .msg_control = control.bytes,
        .msg_controllen = sizeof(control.bytes)};
    ssize_t size = recvmsg(fd, &message, MSG_TRUNC | MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    *guard_fd = -1;
    unsigned int count = 0;
    for (struct cmsghdr *item = CMSG_FIRSTHDR(&message); size >= 0 && item != NULL;
         item = CMSG_NXTHDR(&message, item)) {
        if (item->cmsg_level != SOL_SOCKET || item->cmsg_type != SCM_RIGHTS)
            continue;
        size_t bytes = item->cmsg_len - CMSG_LEN(0);
        for (size_t offset = 0; offset + sizeof(int) <= bytes; offset += sizeof(int)) {
            int received;
            memcpy(&received, (unsigned char *)CMSG_DATA(item) + offset, sizeof(received));
            if (count++ == 0)
                *guard_fd = received;
            else
                close(received);
        }
    }
    if (count > 1 || (message.msg_flags & MSG_CTRUNC) != 0) {
        if (*guard_fd >= 0)
            close(*guard_fd);
        *guard_fd = -1;
        return 0;
    }
    return size;
}

/** pidfd 可读才表示登记的进程已经退出，暂停、失联和 PID 数字存在与否都不能代替 */
static int process_exited(int fd)
{
    if (fd < 0)
        return -ENODATA;
    struct pollfd process = {.fd = fd, .events = POLLIN};
    int ready = poll(&process, 1, 0);
    if (ready < 0)
        return -errno;
    if ((process.revents & (POLLNVAL | POLLERR)) != 0)
        return -EIO;
    return (process.revents & POLLIN) != 0 ? 1 : 0;
}

static int open_process(pid_t pid)
{
#ifdef SYS_pidfd_open
    int fd = (int)syscall(SYS_pidfd_open, pid, 0);
    return fd >= 0 ? fd : -errno;
#else
    (void)pid;
    return -ENOSYS;
#endif
}

static bool same_identity(const struct dppd_recovery_identity *left, const struct dppd_recovery_identity *right);

/** 恢复端必须处于监护启动时登记的网络命名空间，抽象会话地址也按命名空间隔离 */
static int same_network_namespace(const struct dppd_monitor_record *record)
{
    struct stat current;
    if (stat("/proc/self/ns/net", &current) != 0)
        return -errno;
    for (uint32_t index = 0; index < record->count; ++index) {
        const struct dppd_recovery_identity *identity = &record->ports[index].identity;
        if (identity->ifindex == 0 || (uint64_t)current.st_dev != identity->netns_device ||
            (uint64_t)current.st_ino != identity->netns_inode)
            return -EXDEV;
    }
    return 0;
}

static int authorize(struct dppd_monitor_server *server, const struct dppd_monitor_record *record,
    enum request_type type, int fd, int guard_fd, uint64_t revision)
{
    struct ucred peer;
    socklen_t size = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0)
        return -errno;
    if (size != sizeof(peer) || peer.uid != geteuid())
        return -EACCES;
    int exited = process_exited(server->business_pidfd);
    if (type == PROBE)
        return guard_fd < 0 && revision == 0 ? 0 : -EINVAL;
    if (type == BUSINESS_CHECK)
        return guard_fd < 0 && revision == 0 && exited == 0 && server->business_pid == (uint32_t)peer.pid ? 0 : -EACCES;
    if (type == RECOVERY_CHECK) {
        if (guard_fd >= 0 || exited != 1 || server->recovery_fd < 0 || server->recovery_pid != (uint32_t)peer.pid ||
            process_exited(server->recovery_pidfd) != 0 || revision != server->recovery_revision)
            return -EACCES;
        struct dppd_recovery_record current;
        int rc = dppd_recovery_guard_read_locked(server->recovery_guard, record->recovery_path, &current);
        if (rc != 0)
            return rc;
        if (current.revision != server->recovery_revision || memcmp(current.monitor_session, record->session, 16) != 0)
            return -ESTALE;
        return 0;
    }
    if (type == BUSINESS_REGISTER && server->business_pidfd >= 0)
        return -EALREADY;
    if (type == RECOVERY_ACQUIRE && (exited != 1 || server->recovery_fd >= 0))
        return -EBUSY;
    if (guard_fd < 0)
        return -EBADF;
    struct stat info;
    if (fstat(guard_fd, &info) != 0)
        return -errno;
    if ((uint64_t)info.st_dev != record->recovery_device || (uint64_t)info.st_ino != record->recovery_inode ||
        info.st_uid != geteuid())
        return -EXDEV;
    struct dppd_recovery_record recovery;
    int rc = dppd_recovery_guard_read_locked(guard_fd, record->recovery_path, &recovery);
    if (rc != 0)
        return rc;
    if (recovery.revision != revision)
        return -ESTALE;
    if (type == BUSINESS_REGISTER) {
        if (recovery.count != 0 || recovery.attempt_count != 0 || recovery.revision != record->baseline_revision ||
            recovery.last_attempt != record->baseline_attempt)
            return -ESTALE;
        int process = open_process(peer.pid);
        if (process < 0)
            return process;
        server->business_pidfd = process;
        server->business_pid = (uint32_t)peer.pid;
        return process_exited(server->business_pidfd) == 0 ? 0 : -EOWNERDEAD;
    }
    if (recovery.format != 6 || recovery.revision <= record->baseline_revision ||
        recovery.last_attempt < record->baseline_attempt || memcmp(recovery.monitor_session, record->session, 16) != 0)
        return -ESTALE;
    for (uint32_t index = 0; index < recovery.count; ++index) {
        bool found = false;
        for (uint32_t port = 0; port < record->count; ++port)
            found |= recovery.ports[index].port_id == record->ports[port].port_id &&
                same_identity(&recovery.ports[index].identity, &record->ports[port].identity);
        if (!found)
            return -EXDEV;
    }
    server->recovery_revision = recovery.revision;
    return 0;
}

static void release_recovery(struct dppd_monitor_server *server)
{
    if (server->recovery_fd >= 0)
        close(server->recovery_fd);
    if (server->recovery_guard >= 0)
        close(server->recovery_guard);
    if (server->recovery_pidfd >= 0)
        close(server->recovery_pidfd);
    server->recovery_fd = server->recovery_guard = server->recovery_pidfd = -1;
    server->recovery_pid = 0;
    server->recovery_revision = 0;
}

int dppd_monitor_server_poll(struct dppd_monitor_server *server, const struct dppd_monitor_record *record,
    int (*check)(void *context), void *context)
{
    if (server == NULL || server->fd < 0 || record == NULL || check == NULL)
        return -EINVAL;
    uint64_t now;
    int rc = milliseconds(&now);
    if (rc != 0)
        return rc;
    if (server->recovery_fd >= 0) {
        unsigned char byte;
        ssize_t size = recv(server->recovery_fd, &byte, 1, MSG_DONTWAIT);
        if (size >= 0 || (errno != EAGAIN && errno != EINTR) || process_exited(server->recovery_pidfd) != 0)
            release_recovery(server);
    }
    for (unsigned int index = 0; index < DPPD_MONITOR_CLIENT_LIMIT; ++index) {
        if (server->clients[index] >= 0)
            continue;
        int fd = accept4(server->fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EINTR)
                break;
            return -errno;
        }
        if (peer_matches(fd, 0) != 0) {
            close(fd);
            continue;
        }
        server->clients[index] = fd;
        server->expires[index] = now + DPPD_MONITOR_REPLY_MS;
    }
    for (unsigned int index = 0; index < DPPD_MONITOR_CLIENT_LIMIT; ++index) {
        int fd = server->clients[index];
        if (fd < 0)
            continue;
        unsigned char request[MESSAGE_SIZE];
        int guard_fd;
        ssize_t size = receive_request(fd, request, &guard_fd);
        if (size < 0 && (errno == EAGAIN || errno == EINTR) && now < server->expires[index])
            continue;
        enum request_type type = size == sizeof(request) ? (enum request_type)read_number(request + 8, 4) : 0;
        bool valid = size == sizeof(request) && memcmp(request, channel_magic, 8) == 0 && type >= PROBE &&
            type <= RECOVERY_CHECK && read_number(request + 12, 4) == 0 &&
            memcmp(request + 16, record->session, 16) == 0 && dppd_tap_cookie_present(request + 32) &&
            read_number(request + 48, 8) == record->recovery_device &&
            read_number(request + 56, 8) == record->recovery_inode;
        /* 错误或过期客户端只关闭自己的连接，不能拖住事件循环或使其他会话失效 */
        if (valid && now < server->expires[index]) {
            rc = check(context);
            int allowed = rc == 0 ? authorize(server, record, type, fd, guard_fd, read_number(request + 64, 8)) : rc;
            if (allowed == 0 && type == RECOVERY_ACQUIRE) {
                struct ucred peer;
                socklen_t length = sizeof(peer);
                if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) == 0) {
                    int process = open_process(peer.pid);
                    if (process >= 0 && process_exited(process) == 0 &&
                        send(fd, request, sizeof(request), MSG_NOSIGNAL | MSG_DONTWAIT) == sizeof(request)) {
                        server->recovery_fd = fd;
                        server->recovery_guard = guard_fd;
                        server->recovery_pidfd = process;
                        server->recovery_pid = (uint32_t)peer.pid;
                        fd = guard_fd = -1;
                    } else if (process >= 0)
                        close(process);
                }
            } else if (allowed == 0)
                (void)send(fd, request, sizeof(request), MSG_NOSIGNAL | MSG_DONTWAIT);
        }
        if (guard_fd >= 0)
            close(guard_fd);
        if (fd >= 0)
            close(fd);
        server->clients[index] = -1;
        if (rc != 0)
            return rc;
    }
    return 0;
}

void dppd_monitor_server_close(struct dppd_monitor_server *server)
{
    if (server == NULL)
        return;
    /* 未打开的结构体允许只初始化 fd，其余描述符不得被误关 */
    if (server->fd < 0)
        return;
    release_recovery(server);
    if (server->business_pidfd >= 0)
        close(server->business_pidfd);
    server->business_pidfd = -1;
    close(server->fd);
    server->fd = -1;
    for (unsigned int index = 0; index < DPPD_MONITOR_CLIENT_LIMIT; ++index)
        if (server->clients[index] >= 0) {
            close(server->clients[index]);
            server->clients[index] = -1;
        }
}

int dppd_monitor_client_check(struct dppd_monitor_client *client)
{
    if (client == NULL)
        return -EINVAL;
    if (client->error != 0)
        return client->error;
    /* 默认未启用可以跳过，已经绑定后关闭则必须拒绝，不能变回无检查状态 */
    if (client->fd < 0)
        return client->registered || client->recovering ? (client->error = -ENOTCONN) : 0;
    struct stat held, named;
    int rc;
    if (fstat(client->fd, &held) != 0 || lstat(client->path, &named) != 0)
        return client->error = -errno;
    if (!S_ISREG(named.st_mode) || held.st_dev != named.st_dev || held.st_ino != named.st_ino || held.st_nlink != 1 ||
        (held.st_mode & 077) != 0 || held.st_uid != geteuid())
        return client->error = -ESTALE;
    struct dppd_monitor_record record;
    bool locked;
    rc = dppd_monitor_read(client->path, &record, &locked);
    if (rc == 0 && (!locked || record.state != DPPD_MONITOR_ACTIVE))
        rc = -EOWNERDEAD;
    if (rc == 0 && memcmp(&record, &client->record, sizeof(record)) != 0)
        rc = -ESTALE;
    if (rc == 0 && client->recovering) {
        struct pollfd lease = {.fd = client->lease_fd, .events = POLLIN};
        int ready = poll(&lease, 1, 0);
        if (ready < 0)
            rc = -errno;
        else if (ready != 0)
            rc = -ECONNRESET;
    }
    if (rc == 0)
        rc = client->recovering || client->registered
            ? exchange_request(&record, client->recovering ? RECOVERY_CHECK : BUSINESS_CHECK, -1,
                  client->recovering ? client->recovery_revision : 0, NULL)
            : exchange(&record);
    return client->error = rc;
}

static bool same_identity(const struct dppd_recovery_identity *left, const struct dppd_recovery_identity *right)
{
    if (strnlen(right->ifname, sizeof(right->ifname)) == sizeof(right->ifname) ||
        strnlen(right->boot_id, sizeof(right->boot_id)) == sizeof(right->boot_id))
        return false;
    return left->ifindex == right->ifindex && strcmp(left->ifname, right->ifname) == 0 &&
        strcmp(left->boot_id, right->boot_id) == 0 && left->netns_device == right->netns_device &&
        left->netns_inode == right->netns_inode;
}

int dppd_monitor_client_identity(struct dppd_monitor_client *client, uint16_t port,
    const struct dppd_recovery_identity *identity)
{
    if (client == NULL || identity == NULL)
        return -EINVAL;
    if (client->error != 0)
        return client->error;
    if (client->fd < 0)
        return client->registered || client->recovering ? (client->error = -ENOTCONN) : 0;
    for (uint32_t index = 0; index < client->record.count; ++index)
        if (client->record.ports[index].port_id == port)
            return same_identity(&client->record.ports[index].identity, identity) ? 0 : (client->error = -EXDEV);
    return client->error = -ENOENT;
}

int dppd_monitor_client_open(struct dppd_monitor_client *client, const char *path, struct dppd_recovery_guard *guard,
    const struct dppd_recovery_record *ports)
{
    if (client == NULL || path == NULL || guard == NULL || ports == NULL || guard->fd < 0 || ports->count == 0 ||
        ports->count > DPPD_MAX_PORTS)
        return -EINVAL;
    if (client->fd >= 0 || client->error != 0 || client->registered || client->recovering)
        return -EALREADY;
    if (!guard->writable || guard->faulted || guard->record.count != 0 || guard->record.attempt_count != 0)
        return -EUCLEAN;
    if (realpath(path, client->path) == NULL)
        return -errno;
    /* 最后一段符号链接也拒绝，避免把操作者指定的会话悄悄解释成另一个文件 */
    struct stat original;
    if (lstat(path, &original) != 0)
        return -errno;
    if (!S_ISREG(original.st_mode))
        return -EINVAL;
    client->fd = open(client->path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (client->fd < 0)
        return -errno;
    bool locked;
    int rc = dppd_monitor_read(client->path, &client->record, &locked);
    struct stat recovery;
    if (rc == 0 && fstat(guard->fd, &recovery) != 0)
        rc = -errno;
    const struct dppd_monitor_record *record = &client->record;
    if (rc == 0 && (!locked || record->state != DPPD_MONITOR_ACTIVE))
        rc = -EOWNERDEAD;
    if (rc == 0 &&
        ((uint64_t)recovery.st_dev != record->recovery_device || (uint64_t)recovery.st_ino != record->recovery_inode ||
            strcmp(guard->path, record->recovery_path) != 0 || guard->record.revision != record->baseline_revision ||
            guard->record.last_attempt != record->baseline_attempt || ports->count != record->count))
        rc = -EXDEV;
    for (uint32_t index = 0; rc == 0 && index < ports->count; ++index) {
        for (uint32_t prior = 0; prior < index; ++prior)
            if (ports->ports[prior].port_id == ports->ports[index].port_id)
                rc = -EINVAL;
        if (rc == 0)
            rc = dppd_monitor_client_identity(client, ports->ports[index].port_id, &ports->ports[index].identity);
    }
    if (rc == 0)
        rc = dppd_monitor_client_check(client);
    if (rc == 0)
        rc = exchange_request(record, BUSINESS_REGISTER, guard->fd, guard->record.revision, NULL);
    if (rc == 0) {
        client->registered = true;
        rc = dppd_recovery_guard_bind_monitor(guard, record->session);
    }
    if (rc != 0) {
        client->error = rc;
        dppd_monitor_client_close(client);
    }
    return rc;
}

void dppd_monitor_client_close(struct dppd_monitor_client *client)
{
    if (client != NULL && client->recovering && client->lease_fd >= 0) {
        close(client->lease_fd);
        client->lease_fd = -1;
    }
    if (client != NULL && client->fd >= 0) {
        close(client->fd);
        client->fd = -1;
    }
}

int dppd_monitor_recovery_open(struct dppd_monitor_client *client, const char *path, struct dppd_recovery_guard *guard,
    uint64_t revision)
{
    if (client == NULL || path == NULL || guard == NULL || guard->fd < 0)
        return -EINVAL;
    if (client->fd >= 0 || client->error != 0 || client->registered || client->recovering)
        return -EALREADY;
    if (guard->record.revision != revision)
        return -ESTALE;
    if (guard->record.format != 6 || !dppd_tap_cookie_present(guard->record.monitor_session))
        return -ENODATA;
    if (realpath(path, client->path) == NULL)
        return -errno;
    struct stat named, recovery;
    if (lstat(path, &named) != 0 || fstat(guard->fd, &recovery) != 0)
        return -errno;
    if (!S_ISREG(named.st_mode))
        return -EINVAL;
    client->fd = open(client->path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (client->fd < 0)
        return -errno;
    bool locked;
    int rc = dppd_monitor_read(client->path, &client->record, &locked);
    const struct dppd_monitor_record *record = &client->record;
    if (rc == 0)
        rc = same_network_namespace(record);
    if (rc == 0 && (!locked || record->state != DPPD_MONITOR_ACTIVE))
        rc = -EOWNERDEAD;
    if (rc == 0 &&
        (memcmp(guard->record.monitor_session, record->session, 16) != 0 ||
            strcmp(guard->path, record->recovery_path) != 0 || (uint64_t)recovery.st_dev != record->recovery_device ||
            (uint64_t)recovery.st_ino != record->recovery_inode))
        rc = -EXDEV;
    client->lease_fd = -1;
    if (rc == 0)
        rc = dppd_monitor_client_check(client);
    if (rc == 0)
        rc = exchange_request(record, RECOVERY_ACQUIRE, guard->fd, revision, &client->lease_fd);
    if (rc == 0) {
        client->recovering = true;
        client->recovery_revision = revision;
        rc = dppd_monitor_client_check(client);
    }
    if (rc != 0) {
        client->error = rc;
        dppd_monitor_client_close(client);
    }
    return rc;
}
