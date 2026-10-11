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
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MESSAGE_SIZE 64U
static const unsigned char channel_magic[8] = {'D', 'P', 'P', 'M', 'I', 'P', 'C', '1'};

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
    uint64_t now;
    int rc = milliseconds(&now);
    if (rc != 0)
        return rc;
    if (now >= deadline)
        return -ETIMEDOUT;
    struct pollfd item = {.fd = fd, .events = events};
    rc = poll(&item, 1, (int)(deadline - now));
    if (rc < 0)
        return -errno;
    if (rc == 0)
        return -ETIMEDOUT;
    return (item.revents & events) != 0 ? 0 : -ECONNRESET;
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
static int exchange(const struct dppd_monitor_record *record)
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
    ssize_t sent = send(fd, request, sizeof(request), MSG_NOSIGNAL | MSG_DONTWAIT);
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
    close(fd);
    return rc;
}

int dppd_monitor_server_open(struct dppd_monitor_server *server, const struct dppd_monitor_record *record)
{
    if (server == NULL || record == NULL || !dppd_tap_cookie_present(record->session))
        return -EINVAL;
    if (server->fd >= 0)
        return -EALREADY;
    *server = (struct dppd_monitor_server){.fd = -1};
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

int dppd_monitor_server_poll(struct dppd_monitor_server *server, const struct dppd_monitor_record *record,
    int (*check)(void *context), void *context)
{
    if (server == NULL || server->fd < 0 || record == NULL || check == NULL)
        return -EINVAL;
    uint64_t now;
    int rc = milliseconds(&now);
    if (rc != 0)
        return rc;
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
        ssize_t size = recv(fd, request, sizeof(request), MSG_TRUNC | MSG_DONTWAIT);
        if (size < 0 && (errno == EAGAIN || errno == EINTR) && now < server->expires[index])
            continue;
        bool valid = size == sizeof(request) && memcmp(request, channel_magic, 8) == 0 &&
            read_number(request + 8, 4) == 1 && read_number(request + 12, 4) == 0 &&
            memcmp(request + 16, record->session, 16) == 0 && dppd_tap_cookie_present(request + 32) &&
            read_number(request + 48, 8) == record->recovery_device &&
            read_number(request + 56, 8) == record->recovery_inode;
        /* 错误或过期客户端只关闭自己的连接，不能拖住事件循环或使其他会话失效 */
        if (valid && now < server->expires[index]) {
            rc = check(context);
            if (rc == 0)
                (void)send(fd, request, sizeof(request), MSG_NOSIGNAL | MSG_DONTWAIT);
        }
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
    if (client->fd < 0)
        return 0;
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
    if (rc == 0)
        rc = exchange(&record);
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
        return 0;
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
    if (client->fd >= 0 || client->error != 0)
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
        rc = dppd_recovery_guard_bind_monitor(guard, record->session);
    if (rc != 0) {
        client->error = rc;
        dppd_monitor_client_close(client);
    }
    return rc;
}

void dppd_monitor_client_close(struct dppd_monitor_client *client)
{
    if (client != NULL && client->fd >= 0) {
        close(client->fd);
        client->fd = -1;
    }
}
