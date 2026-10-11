#include <errno.h>
#include <linux/rtnetlink.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "dppd/recovery_watch.h"

/** 名称和索引任一命中都算相关，删除后同名重建或改名不能绕过监视 */
static int link_event(struct dppd_recovery_watch *watch, const struct nlmsghdr *header)
{
    if (NLMSG_PAYLOAD(header, 0) < sizeof(struct ifinfomsg))
        return -EBADMSG;
    const struct ifinfomsg *link = NLMSG_DATA(header);
    if (link->ifi_index <= 0)
        return -EBADMSG;
    const struct rtattr *attribute = IFLA_RTA(link);
    size_t remaining = IFLA_PAYLOAD(header);
    const char *name = NULL;
    while (remaining != 0) {
        if (remaining < sizeof(*attribute) || attribute->rta_len < sizeof(*attribute) ||
            RTA_ALIGN(attribute->rta_len) > remaining)
            return -EBADMSG;
        if ((attribute->rta_type & NLA_TYPE_MASK) == IFLA_IFNAME) {
            size_t size = RTA_PAYLOAD(attribute);
            const char *value = RTA_DATA(attribute);
            if (name != NULL || size < 2 || size > DPPD_RECOVERY_IFNAME_SIZE || value[size - 1] != '\0' ||
                strlen(value) != size - 1)
                return -EBADMSG;
            name = value;
        }
        remaining -= RTA_ALIGN(attribute->rta_len);
        attribute = (const struct rtattr *)((const char *)attribute + RTA_ALIGN(attribute->rta_len));
    }
    if (name == NULL)
        return -EBADMSG;
    for (uint32_t index = 0; index < watch->count; ++index)
        if (watch->links[index].ifindex == (uint32_t)link->ifi_index || strcmp(watch->links[index].ifname, name) == 0)
            watch->link_changed = true;
    return 0;
}

/** TC 动作可能跨接口共享，因此同一命名空间的 TC 通知一律使本次观察失效 */
static int watch_message(struct dppd_recovery_watch *watch, const struct nlmsghdr *header)
{
    if ((header->nlmsg_flags & NLM_F_DUMP_INTR) != 0)
        return -EAGAIN;
    switch (header->nlmsg_type) {
    case RTM_NEWLINK:
    case RTM_DELLINK:
        return link_event(watch, header);
    case RTM_NEWQDISC:
    case RTM_DELQDISC:
    case RTM_NEWTCLASS:
    case RTM_DELTCLASS:
    case RTM_NEWTFILTER:
    case RTM_DELTFILTER:
    case RTM_NEWCHAIN:
    case RTM_DELCHAIN:
        if (NLMSG_PAYLOAD(header, 0) < sizeof(struct tcmsg))
            return -EBADMSG;
        watch->tc_changed = true;
        return 0;
    case RTM_NEWACTION:
    case RTM_DELACTION:
        if (NLMSG_PAYLOAD(header, 0) < sizeof(struct tcamsg))
            return -EBADMSG;
        watch->tc_changed = true;
        return 0;
    case NLMSG_OVERRUN:
        return -ENOBUFS;
    default:
        /* 未知通知不猜测为无关事件，也不把控制消息当作观察已经完整 */
        return -ENOTSUP;
    }
}

int dppd_recovery_watch_open(struct dppd_recovery_watch *watch, const struct dppd_recovery_record *record)
{
    if (watch == NULL || record == NULL || record->count > DPPD_MAX_PORTS)
        return -EINVAL;
    if (watch->fd >= 0)
        return -EALREADY;
    *watch = (struct dppd_recovery_watch){.fd = -1, .count = record->count};
    for (uint32_t index = 0; index < record->count; ++index) {
        watch->links[index].ifindex = record->ports[index].identity.ifindex;
        memcpy(watch->links[index].ifname, record->ports[index].identity.ifname, DPPD_RECOVERY_IFNAME_SIZE);
        if (memchr(watch->links[index].ifname, '\0', DPPD_RECOVERY_IFNAME_SIZE) == NULL)
            return watch->error = -EINVAL;
    }
    struct sockaddr_nl address = {.nl_family = AF_NETLINK, .nl_groups = RTMGRP_LINK | RTMGRP_TC};
    int capacity = 1024 * 1024;
    watch->fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
    if (watch->fd < 0)
        return watch->error = -errno;
    /* 增大接收空间只减少丢包，绝不启用会隐藏 ENOBUFS 的 NETLINK_NO_ENOBUFS */
    if (setsockopt(watch->fd, SOL_SOCKET, SO_RCVBUF, &capacity, sizeof(capacity)) != 0 ||
        bind(watch->fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        watch->error = -errno;
        dppd_recovery_watch_close(watch);
        return watch->error;
    }
    return 0;
}

int dppd_recovery_watch_check(struct dppd_recovery_watch *watch)
{
    if (watch == NULL)
        return -EINVAL;
    if (watch->error != 0)
        return watch->error;
    if (watch->fd < 0)
        return -EBADF;
    union {
        struct nlmsghdr aligned;
        unsigned char bytes[32768];
    } buffer;
    /* 持续事件流也必须有界返回，达到上限时拒绝本次观察，不能无限等待空闲 */
    for (unsigned int batch = 0; batch < 128; ++batch) {
        struct sockaddr_nl sender = {0};
        struct iovec vector = {.iov_base = buffer.bytes, .iov_len = sizeof(buffer.bytes)};
        struct msghdr message = {.msg_name = &sender,
            .msg_namelen = sizeof(sender),
            .msg_iov = &vector,
            .msg_iovlen = 1};
        ssize_t received = recvmsg(watch->fd, &message, MSG_DONTWAIT);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return watch->link_changed || watch->tc_changed ? -ESTALE : 0;
            return watch->error = -errno;
        }
        if (received == 0 || (size_t)received > sizeof(buffer.bytes) ||
            (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 || message.msg_namelen != sizeof(sender) ||
            sender.nl_family != AF_NETLINK || sender.nl_pid != 0)
            return watch->error = -EBADMSG;
        size_t remaining = (size_t)received;
        const struct nlmsghdr *header = (const struct nlmsghdr *)buffer.bytes;
        while (remaining != 0) {
            if (remaining < sizeof(*header) || header->nlmsg_len < sizeof(*header) || header->nlmsg_len > remaining ||
                NLMSG_ALIGN(header->nlmsg_len) > remaining)
                return watch->error = -EBADMSG;
            int rc = watch_message(watch, header);
            if (rc != 0)
                return watch->error = rc;
            /* 通知序号来自其他写入者，不是连续事件编号，不能据此宣称没有丢失 */
            remaining -= NLMSG_ALIGN(header->nlmsg_len);
            header = (const struct nlmsghdr *)((const char *)header + NLMSG_ALIGN(header->nlmsg_len));
        }
    }
    return watch->error = -E2BIG;
}

void dppd_recovery_watch_close(struct dppd_recovery_watch *watch)
{
    if (watch != NULL && watch->fd >= 0) {
        close(watch->fd);
        watch->fd = -1;
    }
}
