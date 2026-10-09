/** 直接编译解析器以构造损坏的内核消息，测试不创建、删除或修改任何网络对象 */
#define _GNU_SOURCE
#include <sys/socket.h>
static ssize_t test_receive(int fd, struct msghdr *message, int flags);
#define recvmsg test_receive
#include "../../lib/control/recovery_inspect.c"
#undef recvmsg
#include <assert.h>

static unsigned int receive_fault;

/** 只替换接收结果以模拟内核消息损坏，其余场景仍使用真实 socket 和只读 GET 请求 */
static ssize_t test_receive(int fd, struct msghdr *message, int flags)
{
    if (receive_fault == 0)
        return recvmsg(fd, message, flags);
    struct sockaddr_nl *sender = message->msg_name;
    *sender = (struct sockaddr_nl){.nl_family = AF_NETLINK};
    struct nlmsghdr *header = message->msg_iov[0].iov_base;
    *header = (struct nlmsghdr){.nlmsg_len = NLMSG_LENGTH(sizeof(int)), .nlmsg_seq = 1, .nlmsg_type = NLMSG_DONE};
    *(int *)NLMSG_DATA(header) = 0;
    switch (receive_fault) {
    case 1: message->msg_flags = MSG_TRUNC; break;
    case 2: sender->nl_pid = 123; break;
    case 3: header->nlmsg_seq = 2; break;
    case 4: header->nlmsg_flags = NLM_F_DUMP_INTR; break;
    case 5: *(int *)NLMSG_DATA(header) = -ENOBUFS; break;
    case 6: header->nlmsg_type = NLMSG_ERROR; break;
    case 7: header->nlmsg_len = 1; return sizeof(*header);
    case 8: errno = EAGAIN; return -1;
    case 9: header->nlmsg_type = NLMSG_OVERRUN; break;
    default: break;
    }
    return header->nlmsg_len;
}

struct test_message {
    struct nlmsghdr header;
    struct tcmsg tc;
    unsigned char data[256];
};

/** 测试消息按真实 Netlink 对齐规则拼装，允许精确破坏某个属性边界 */
static void append(struct test_message *message, unsigned short type, const void *data, size_t size)
{
    struct rtattr *attribute = (struct rtattr *)((char *)message + message->header.nlmsg_len);
    assert(message->header.nlmsg_len + RTA_SPACE(size) <= sizeof(*message));
    attribute->rta_type = type;
    attribute->rta_len = RTA_LENGTH(size);
    memcpy(RTA_DATA(attribute), data, size);
    message->header.nlmsg_len += RTA_ALIGN(attribute->rta_len);
}

/** 核对真实对象与分类器标题的区别，以及链号、字节序、容量和损坏字段的拒绝 */
static void filter_messages(void)
{
    struct dppd_recovery_inspection inspection = {0};
    struct tc_query query = {.index = 17, .parent = TAP_MULTIQ, .inspection = &inspection};
    struct test_message message = {.header = {.nlmsg_type = RTM_NEWTFILTER,
        .nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg))},
        .tc = {.tcm_ifindex = 17, .tcm_parent = TAP_MULTIQ, .tcm_handle = 0x1234,
            .tcm_info = (55U << 16) | htons(0x800)}};
    uint32_t chain = 9;
    append(&message, TCA_KIND, "flower", 7);
    append(&message, TCA_CHAIN, &chain, sizeof(chain));
    assert(read_tc(&message.header, &query) == 0 && inspection.count == 1);
    const struct dppd_recovery_filter *filter = &inspection.filters[0];
    assert(filter->handle == 0x1234 && filter->parent == TAP_MULTIQ && filter->chain == 9);
    assert(filter->priority == 55 && filter->protocol == 0x800 && strcmp(filter->kind, "flower") == 0);
    message.tc.tcm_handle = 0;
    assert(read_tc(&message.header, &query) == 0 && inspection.count == 1);
    message.tc.tcm_handle = 1;
    inspection.count = DPPD_RECOVERY_FILTER_LIMIT;
    assert(read_tc(&message.header, &query) == -E2BIG);
    inspection.count = 0;
    message.tc.tcm_parent = TAP_INGRESS;
    assert(read_tc(&message.header, &query) == -EBADMSG);
    message.tc.tcm_parent = TAP_MULTIQ;
    struct rtattr *kind = (struct rtattr *)message.data;
    ((char *)RTA_DATA(kind))[6] = 'x';
    assert(read_tc(&message.header, &query) == -EBADMSG);
    ((char *)RTA_DATA(kind))[6] = '\0';
    append(&message, TCA_CHAIN, &chain, sizeof(chain));
    assert(read_tc(&message.header, &query) == -EBADMSG);
    message.header.nlmsg_len--;
    assert(read_tc(&message.header, &query) == -EBADMSG);
    kind->rta_len = 1;
    assert(read_tc(&message.header, &query) == -EBADMSG);
}

/** 预期挂载点如果改成另一类 qdisc，不能继续声称该范围已经完整检查 */
static void qdisc_messages(void)
{
    struct tc_query query = {.index = 17};
    struct test_message message = {.header = {.nlmsg_type = RTM_NEWQDISC,
        .nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg))},
        .tc = {.tcm_ifindex = 17, .tcm_handle = TAP_MULTIQ}};
    append(&message, TCA_KIND, "multiq", 7);
    assert(read_tc(&message.header, &query) == 0 && query.multiq && !query.ingress);
    message.tc.tcm_handle = TAP_INGRESS;
    assert(read_tc(&message.header, &query) == -ENOTSUP);
    message.header.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg));
    append(&message, TCA_KIND, "ingress", 8);
    assert(read_tc(&message.header, &query) == 0 && query.ingress);
    message.tc.tcm_ifindex = 99;
    query.multiq = query.ingress = false;
    assert(read_tc(&message.header, &query) == 0 && !query.multiq && !query.ingress);
}

/** 截断、错误发送者、错序号、dump 中断、丢包和超时均必须阻止成功返回 */
static void transport_failures(void)
{
    static const int errors[] = {0, -EBADMSG, -EBADMSG, -EBADMSG, -EAGAIN,
        -ENOBUFS, -EBADMSG, -EBADMSG, -EAGAIN, -EBADMSG};
    struct tc_query query = {.index = 1};
    for (receive_fault = 1; receive_fault < sizeof(errors) / sizeof(errors[0]); ++receive_fault)
        assert(query_tc(&query) == errors[receive_fault]);
    receive_fault = 0;
}

/** 不支持的 PMD、旧记录和其他启动环境都明确返回未知，绝不能返回成功的空规则集 */
static void unknown_identity(void)
{
    struct dppd_recovery_port port = {0};
    struct dppd_recovery_inspection inspection;
    strcpy(port.driver, "net_ring");
    assert(dppd_recovery_inspect(&port, &inspection) == -ENOTSUP);
    assert(inspection.state == DPPD_INSPECT_UNSUPPORTED && inspection.count == 0);
    strcpy(port.driver, "net_tap");
    assert(dppd_recovery_inspect(&port, &inspection) == -ENODATA);
    assert(inspection.state == DPPD_INSPECT_NO_IDENTITY);
    port.identity.ifindex = 1;
    strcpy(port.identity.boot_id, "00000000-0000-0000-0000-000000000000");
    assert(dppd_recovery_inspect(&port, &inspection) == -EXDEV);
    assert(inspection.state == DPPD_INSPECT_CONTEXT_MISMATCH);
    assert(dppd_recovery_tap_identity(0, &port.identity) == -EINVAL);
    /** loopback 存在但不是 TAP，验证真实只读 Netlink 路径不会凭接口存在就接受 */
    assert(dppd_recovery_tap_identity(1, &port.identity) == -EXDEV);
}

int main(void)
{
    filter_messages();
    qdisc_messages();
    transport_failures();
    unknown_identity();
    return 0;
}
