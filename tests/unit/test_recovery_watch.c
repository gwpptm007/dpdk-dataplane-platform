#include <assert.h>
#include <fcntl.h>
#include <sys/socket.h>

static ssize_t test_receive(int fd, struct msghdr *message, int flags);
static int test_bind(int fd, const struct sockaddr *address, socklen_t length);
#define recvmsg test_receive
#define bind test_bind
#include "../../lib/control/recovery_watch.c"
#undef recvmsg
#undef bind

static unsigned int receive_fault, receives;
static bool fail_bind;
static int opened_fd;
static struct {
    struct nlmsghdr header;
    struct ifinfomsg link;
    unsigned char attributes[32];
} notification;

/** 人工通知只替换接收结果，真实订阅用于检查初始化、非阻塞标志和失败释放 */
static int test_bind(int fd, const struct sockaddr *address, socklen_t length)
{
    opened_fd = fd;
    if (fail_bind) {
        errno = EPERM;
        return -1;
    }
    return bind(fd, address, length);
}

static ssize_t test_receive(int fd, struct msghdr *message, int flags)
{
    (void)fd;
    assert(flags == MSG_DONTWAIT);
    ++receives;
    if (receive_fault == 0 || (receives > 1 && receive_fault != 12)) {
        errno = EAGAIN;
        return -1;
    }
    if (receive_fault == 1 || receive_fault == 2) {
        errno = receive_fault == 1 ? ENOBUFS : EINTR;
        return -1;
    }
    struct sockaddr_nl *sender = message->msg_name;
    *sender = (struct sockaddr_nl){.nl_family = AF_NETLINK};
    memcpy(message->msg_iov[0].iov_base, &notification, sizeof(notification));
    struct nlmsghdr *header = message->msg_iov[0].iov_base;
    switch (receive_fault) {
    case 3:
        message->msg_flags = MSG_TRUNC;
        break;
    case 4:
        sender->nl_pid = 17;
        break;
    case 5:
        header->nlmsg_len = 1;
        break;
    case 6:
        header->nlmsg_type = NLMSG_OVERRUN;
        break;
    case 7:
        header->nlmsg_flags = NLM_F_DUMP_INTR;
        break;
    case 8:
        header->nlmsg_type = RTM_NEWADDR;
        break;
    case 9:
        message->msg_namelen = 0;
        break;
    case 10:
        message->msg_flags = MSG_CTRUNC;
        break;
    case 11:
        return 0;
    case 14:
        header->nlmsg_len = UINT32_MAX;
        break;
    case 15:
        return notification.header.nlmsg_len + 1;
    case 16: {
        /* 同一批里先收到无关通知，再收到相关通知，不能只检查第一个消息 */
        memcpy((char *)header + header->nlmsg_len, &notification, notification.header.nlmsg_len);
        struct ifinfomsg *link = NLMSG_DATA(header);
        link->ifi_index = 99;
        strcpy(RTA_DATA(IFLA_RTA(link)), "other");
        return notification.header.nlmsg_len * 2;
    }
    default:
        break;
    }
    return notification.header.nlmsg_len;
}

static void make_link(uint32_t index, const char *name)
{
    memset(&notification, 0, sizeof(notification));
    notification.header.nlmsg_type = RTM_NEWLINK;
    notification.header.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    /* 通知可能沿用其他写入者的序号，非零序号不能直接当作损坏 */
    notification.header.nlmsg_seq = 991;
    notification.link.ifi_index = index;
    struct rtattr *attribute = (struct rtattr *)notification.attributes;
    attribute->rta_type = IFLA_IFNAME;
    attribute->rta_len = RTA_LENGTH(strlen(name) + 1);
    strcpy(RTA_DATA(attribute), name);
    notification.header.nlmsg_len += RTA_ALIGN(attribute->rta_len);
}

static void test_events(void)
{
    struct dppd_recovery_watch watch = {.fd = -1, .count = 1, .links = {{.ifindex = 17, .ifname = "owned"}}};
    make_link(99, "other");
    assert(watch_message(&watch, &notification.header) == 0 && !watch.link_changed);
    make_link(99, "owned");
    assert(watch_message(&watch, &notification.header) == 0 && watch.link_changed);
    watch.link_changed = false;
    make_link(17, "renamed");
    notification.header.nlmsg_type = RTM_DELLINK;
    assert(watch_message(&watch, &notification.header) == 0 && watch.link_changed);
    make_link(0, "owned");
    assert(watch_message(&watch, &notification.header) == -EBADMSG);
    make_link(17, "owned");
    notification.header.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    assert(watch_message(&watch, &notification.header) == -EBADMSG);
    make_link(17, "owned");
    notification.attributes[0] = 1;
    assert(watch_message(&watch, &notification.header) == -EBADMSG);
    make_link(17, "owned");
    notification.attributes[RTA_LENGTH(5)] = 'x';
    assert(watch_message(&watch, &notification.header) == -EBADMSG);

    unsigned int types[] = {RTM_NEWQDISC, RTM_DELQDISC, RTM_NEWTCLASS, RTM_DELTCLASS, RTM_NEWTFILTER, RTM_DELTFILTER,
        RTM_NEWCHAIN, RTM_DELCHAIN, RTM_NEWACTION, RTM_DELACTION};
    for (unsigned int index = 0; index < sizeof(types) / sizeof(types[0]); ++index) {
        notification.header.nlmsg_type = types[index];
        notification.header.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg));
        watch.tc_changed = false;
        assert(watch_message(&watch, &notification.header) == 0 && watch.tc_changed);
        notification.header.nlmsg_len = NLMSG_LENGTH(1);
        assert(watch_message(&watch, &notification.header) == -EBADMSG);
    }
}

static void test_receive_errors(void)
{
    struct dppd_recovery_record record = {.count = 1};
    record.ports[0].identity.ifindex = 17;
    strcpy(record.ports[0].identity.ifname, "owned");
    const int expected[] = {0, -ENOBUFS, -EINTR, -EBADMSG, -EBADMSG, -EBADMSG, -ENOBUFS, -EAGAIN, -ENOTSUP, -EBADMSG,
        -EBADMSG, -EBADMSG, -E2BIG, -ESTALE, -EBADMSG, -EBADMSG, -ESTALE};
    for (unsigned int fault = 0; fault < sizeof(expected) / sizeof(expected[0]); ++fault) {
        struct dppd_recovery_watch watch = {.fd = -1};
        assert(dppd_recovery_watch_open(&watch, &record) == 0);
        assert((fcntl(watch.fd, F_GETFL) & O_NONBLOCK) != 0);
        assert((fcntl(watch.fd, F_GETFD) & FD_CLOEXEC) != 0);
        assert(dppd_recovery_watch_open(&watch, &record) == -EALREADY);
        make_link(17, "owned");
        receive_fault = fault;
        receives = 0;
        assert(dppd_recovery_watch_check(&watch) == expected[fault]);
        /* 后续接收即使恢复正常，丢包、损坏或中途变化也不能被抹掉 */
        receive_fault = 0;
        assert(dppd_recovery_watch_check(&watch) == expected[fault]);
        dppd_recovery_watch_close(&watch);
        assert(watch.fd == -1);
        dppd_recovery_watch_close(&watch);
    }
    struct dppd_recovery_watch watch = {.fd = -1};
    fail_bind = true;
    assert(dppd_recovery_watch_open(&watch, &record) == -EPERM && watch.fd == -1);
    assert(fcntl(opened_fd, F_GETFD) == -1 && errno == EBADF);
    assert(dppd_recovery_watch_check(&watch) == -EPERM);
    assert(dppd_recovery_watch_open(NULL, &record) == -EINVAL);
    assert(dppd_recovery_watch_check(NULL) == -EINVAL);
    dppd_recovery_watch_close(NULL);
}

/** 生命周期监护允许正常业务更新，但删除后恢复原坐标也不能抹掉变化 */
static void test_lifetime(void)
{
    struct dppd_recovery_record record = {.count = 1};
    record.ports[0].identity.ifindex = 17;
    strcpy(record.ports[0].identity.ifname, "owned");
    fail_bind = false;
    struct dppd_recovery_watch watch = {.fd = -1};
    assert(dppd_recovery_watch_open_lifetime(&watch, &record) == 0 && watch.lifetime_only);
    make_link(17, "owned");
    receive_fault = 13;
    receives = 0;
    assert(dppd_recovery_watch_check(&watch) == 0 && !watch.link_changed);
    notification.header.nlmsg_type = RTM_NEWTFILTER;
    notification.header.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg));
    receives = 0;
    assert(dppd_recovery_watch_check(&watch) == 0 && watch.tc_changed);
    make_link(17, "owned");
    notification.header.nlmsg_type = RTM_DELLINK;
    receives = 0;
    assert(dppd_recovery_watch_check(&watch) == -ESTALE && watch.error == 0);
    make_link(17, "owned");
    receives = 0;
    assert(dppd_recovery_watch_check(&watch) == -ESTALE);
    dppd_recovery_watch_close(&watch);
    const char *names[] = {"renamed", "owned"};
    const uint32_t indexes[] = {17, 99};
    for (unsigned int index = 0; index < 2; ++index) {
        assert(dppd_recovery_watch_open_lifetime(&watch, &record) == 0);
        make_link(indexes[index], names[index]);
        receives = 0;
        assert(dppd_recovery_watch_check(&watch) == -ESTALE);
        dppd_recovery_watch_close(&watch);
    }
    assert(dppd_recovery_watch_open_lifetime(&watch, &record) == 0);
    receive_fault = 1;
    receives = 0;
    assert(dppd_recovery_watch_check(&watch) == -ENOBUFS);
    receive_fault = 0;
    assert(dppd_recovery_watch_check(&watch) == -ENOBUFS);
    dppd_recovery_watch_close(&watch);
}

int main(void)
{
    test_events();
    test_receive_errors();
    test_lifetime();
    return 0;
}
