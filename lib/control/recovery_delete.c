#define _GNU_SOURCE
#include "dppd/recovery_cleanup.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <linux/rtnetlink.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int milliseconds(uint64_t *value)
{
    struct timespec now;
    /** 时钟失败必须返回非零错误，不能让未写入的时间被当成成功结果 */
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return errno != 0 ? -errno : -EIO;
    *value = (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
    return 0;
}

/** 删除 ACK 共用一个绝对截止时间，持续信号不能延长等待或诱使调用方把超时当作没删除 */
static int wait_ready(int fd, short events, uint64_t deadline)
{
    for (;;) {
        uint64_t now;
        int rc = milliseconds(&now);
        if (rc != 0 || now >= deadline)
            return rc != 0 ? rc : -ETIMEDOUT;
        struct pollfd item = {.fd = fd, .events = events};
        rc = poll(&item, 1, (int)(deadline - now));
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc <= 0)
            return rc < 0 ? -errno : -ETIMEDOUT;
        return (item.revents & events) != 0 ? 0 : -EIO;
    }
}

int dppd_recovery_delete_filter(struct dppd_recovery_scope *scope, const struct dppd_recovery_port *port,
    const struct dppd_recovery_filter *filter)
{
    if (scope == NULL || !scope->enabled || scope->role != DPPD_SCOPE_RECOVERY || port == NULL || filter == NULL)
        return -EACCES;
    if (strcmp(port->driver, "net_tap") != 0 || port->identity.ifindex == 0 || port->identity.ifindex > INT32_MAX ||
        port->identity.netns_device != scope->netns_device || port->identity.netns_inode != scope->netns_inode ||
        !filter->cookie_valid || !dppd_tap_cookie_present(filter->cookie) || strcmp(filter->kind, "flower") != 0 ||
        filter->handle == 0 || filter->priority == 0 || (filter->parent != 0x10000U && filter->parent != 0xffff0000U))
        return -ENOTSUP;
    int rc = dppd_scope_check(scope);
    if (rc != 0)
        return rc;
    struct {
        struct nlmsghdr header;
        struct tcmsg message;
        unsigned char attributes[32];
    } request = {.header = {.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg)),
                     .nlmsg_type = RTM_DELTFILTER,
                     .nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK,
                     .nlmsg_seq = 1},
        .message = {.tcm_family = AF_UNSPEC,
            .tcm_ifindex = (int)port->identity.ifindex,
            .tcm_handle = filter->handle,
            .tcm_parent = filter->parent,
            .tcm_info = (uint32_t)filter->priority << 16 | htons(filter->protocol)}};
    struct rtattr *attribute = (struct rtattr *)request.attributes;
    attribute->rta_type = TCA_KIND;
    attribute->rta_len = RTA_LENGTH(sizeof("flower"));
    memcpy(RTA_DATA(attribute), "flower", sizeof("flower"));
    request.header.nlmsg_len += RTA_ALIGN(attribute->rta_len);
    attribute = (struct rtattr *)((unsigned char *)&request + request.header.nlmsg_len);
    attribute->rta_type = TCA_CHAIN;
    attribute->rta_len = RTA_LENGTH(sizeof(filter->chain));
    memcpy(RTA_DATA(attribute), &filter->chain, sizeof(filter->chain));
    request.header.nlmsg_len += RTA_ALIGN(attribute->rta_len);
    struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return -errno;
    uint64_t started;
    rc = milliseconds(&started);
    if (rc == 0 && bind(fd, (struct sockaddr *)&kernel, sizeof(kernel)) != 0)
        rc = -errno;
    if (rc == 0)
        rc = wait_ready(fd, POLLOUT, started + 2000);
    if (rc == 0) {
        ssize_t sent = sendto(fd, &request, request.header.nlmsg_len, MSG_DONTWAIT | MSG_NOSIGNAL,
            (struct sockaddr *)&kernel, sizeof(kernel));
        if (sent != request.header.nlmsg_len)
            rc = sent < 0 ? -errno : -EIO;
    }
    if (rc == 0)
        rc = wait_ready(fd, POLLIN, started + 2000);
    if (rc == 0) {
        union {
            struct nlmsghdr aligned;
            unsigned char bytes[4096];
        } reply;
        struct sockaddr_nl sender = {0};
        struct iovec vector = {.iov_base = reply.bytes, .iov_len = sizeof(reply.bytes)};
        struct msghdr message = {.msg_name = &sender,
            .msg_namelen = sizeof(sender),
            .msg_iov = &vector,
            .msg_iovlen = 1};
        ssize_t size = recvmsg(fd, &message, MSG_DONTWAIT);
        const struct nlmsghdr *header = &reply.aligned;
        if (size < 0)
            rc = -errno;
        else if (size < (ssize_t)NLMSG_LENGTH(sizeof(struct nlmsgerr)) ||
            (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 || message.msg_namelen != sizeof(sender) ||
            sender.nl_family != AF_NETLINK || sender.nl_pid != 0 || header->nlmsg_len > (size_t)size ||
            header->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr)) ||
            NLMSG_ALIGN(header->nlmsg_len) != (size_t)size || header->nlmsg_type != NLMSG_ERROR ||
            header->nlmsg_seq != 1)
            rc = -EBADMSG;
        else {
            const struct nlmsgerr *error = NLMSG_DATA(header);
            rc = error->msg.nlmsg_type != RTM_DELTFILTER || error->msg.nlmsg_seq != 1 || error->error > 0 ||
                    error->error < -4095
                ? -EBADMSG
                : error->error;
        }
    }
    close(fd);
    return rc;
}
