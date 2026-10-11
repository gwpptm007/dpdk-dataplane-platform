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
    unsigned char data[1024];
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

/** 真正嵌套编码覆盖短标识、重复属性、多动作和复制标识，不能只测试手填结构 */
static void cookie_messages(void)
{
    const uint8_t cookie[DPPD_TAP_COOKIE_SIZE] = {0x12, 0x34};
    for (unsigned int mode = 0; mode < 6; ++mode) {
        struct test_message action = {.header.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg))};
        struct test_message actions = action, options = action;
        struct dppd_recovery_filter filter = {0};
        append(&action, TCA_ACT_KIND, "gact", 5);
        append(&action, TCA_ACT_COOKIE, cookie, mode == 1 ? 15 : sizeof(cookie));
        if (mode == 2)
            append(&action, TCA_ACT_COOKIE, cookie, sizeof(cookie));
        append(&actions, 1, action.data, action.header.nlmsg_len - NLMSG_LENGTH(sizeof(struct tcmsg)));
        if (mode == 3)
            append(&actions, 2, action.data, action.header.nlmsg_len - NLMSG_LENGTH(sizeof(struct tcmsg)));
        append(&options, TCA_FLOWER_ACT, actions.data, actions.header.nlmsg_len - NLMSG_LENGTH(sizeof(struct tcmsg)));
        struct test_message outer = {.header.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg))};
        append(&outer, TCA_OPTIONS, options.data, options.header.nlmsg_len - NLMSG_LENGTH(sizeof(struct tcmsg)));
        struct rtattr *attr = (struct rtattr *)outer.data;
        if (mode == 4)
            attr->rta_len--;
        if (mode == 5)
            ((struct rtattr *)RTA_DATA(attr))->rta_len = 1;
        int rc = read_cookie(attr, &filter);
        assert(rc == (mode == 0 ? 0 : mode == 1 || mode == 3 ? -ENOTSUP : -EBADMSG));
        if (mode == 0)
            assert(filter.cookie_valid && memcmp(filter.cookie, cookie, sizeof(cookie)) == 0);
    }
    struct dppd_recovery_inspection inspection = {.state = DPPD_INSPECT_PRESENT, .count = 2};
    inspection.filters[0].cookie_valid = true;
    memcpy(inspection.filters[0].cookie, cookie, sizeof(cookie));
    assert(dppd_recovery_cookie_matches(&inspection, cookie) == 1);
    inspection.filters[1] = inspection.filters[0];
    assert(dppd_recovery_cookie_matches(&inspection, cookie) == 2);
    inspection.filters[0].cookie_valid = inspection.filters[1].cookie_valid = false;
    assert(dppd_recovery_cookie_matches(&inspection, cookie) == 0);
    inspection.state = DPPD_INSPECT_UNAVAILABLE;
    assert(dppd_recovery_cookie_matches(&inspection, cookie) == -ENODATA);
}

/** 构造合法动作后逐项加入未知字段、共享引用和行为变化，检查标识相同也不能误报匹配 */
static void content_messages(void)
{
    const struct dppd_tap_content expected = {.version = 1, .action = 1,
        .parent = TAP_MULTIQ, .priority = 26, .protocol = 3};
    for (unsigned int mode = 0; mode < 12; ++mode) {
        struct test_message options = {.header.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg))};
        struct test_message action = options, actions = options, parameters = options, outer = options;
        struct tc_gact gact = {.index = 123, .action = TC_ACT_SHOT, .refcnt = 1, .bindcnt = 1};
        struct dppd_recovery_filter filter = {.parent = TAP_MULTIQ, .priority = 26, .protocol = 3};
        if (mode == 1) gact.bindcnt = gact.refcnt = 2;
        if (mode == 2) gact.refcnt = 2;
        if (mode == 3) gact.action = TC_ACT_OK;
        if (mode == 4) filter.priority++;
        if (mode == 5) filter.chain = 1;
        uint32_t value = 0;
        append(&parameters, TCA_GACT_PARMS, &gact, sizeof(gact));
        if (mode == 6) append(&parameters, TCA_GACT_PROB, &value, sizeof(value));
        append(&action, TCA_ACT_KIND, "gact", 5);
        append(&action, TCA_ACT_OPTIONS, parameters.data, parameters.header.nlmsg_len - NLMSG_LENGTH(sizeof(struct tcmsg)));
        if (mode == 7) append(&action, TCA_ACT_MAX + 99, &value, sizeof(value));
        append(&actions, 1, action.data, action.header.nlmsg_len - NLMSG_LENGTH(sizeof(struct tcmsg)));
        append(&options, TCA_FLOWER_ACT, actions.data, actions.header.nlmsg_len - NLMSG_LENGTH(sizeof(struct tcmsg)));
        if (mode == 8) append(&options, TCA_FLOWER_MAX + 99, &value, sizeof(value));
        if (mode == 9) append(&options, TCA_FLOWER_CLASSID, &value, sizeof(value));
        if (mode == 10) {
            uint16_t ether = htons(0x0800);
            append(&options, TCA_FLOWER_KEY_ETH_TYPE, &ether, sizeof(ether));
        }
        if (mode == 11) {
            value = TCA_CLS_FLAGS_SKIP_HW;
            append(&options, TCA_FLOWER_FLAGS, &value, sizeof(value));
        }
        append(&outer, TCA_OPTIONS, options.data, options.header.nlmsg_len - NLMSG_LENGTH(sizeof(struct tcmsg)));
        read_content((const struct rtattr *)outer.data, &filter);
        int content = dppd_recovery_verify(&expected, &filter, false);
        int scope = dppd_recovery_verify(&expected, &filter, true);
        if (mode == 0) assert(content == 0 && scope == 0);
        else if (mode == 1) assert(content == 0 && scope == -EMLINK);
        else if (mode == 2) assert(content == 0 && scope == -ENOTSUP);
        else if (mode == 4 || mode == 5) assert(content == 0 && scope == -ESTALE);
        else if (mode == 10) assert(content == -ESTALE && scope == 0);
        else assert(content == -ENOTSUP && scope == -ENOTSUP);
        const struct dppd_tap_content legacy = {0};
        assert(dppd_recovery_verify(&legacy, &filter, false) == -ENODATA);
    }
}

int main(void)
{
    /** 正常单新增、同时多新增、旧对象变化和重复坐标必须分开判断 */
    struct dppd_recovery_inspection before = {.state = DPPD_INSPECT_PRESENT, .count = 1};
    struct dppd_recovery_inspection after = {.state = DPPD_INSPECT_PRESENT, .count = 2};
    struct dppd_recovery_filter candidate;
    before.filters[0] = (struct dppd_recovery_filter){.handle = 1, .kind = "flower"};
    after.filters[0] = before.filters[0];
    after.filters[1] = (struct dppd_recovery_filter){.handle = 2, .kind = "flower"};
    assert(dppd_recovery_compare(&before, &after, &candidate) == DPPD_EVIDENCE_SINGLE_ADDITION);
    assert(candidate.handle == 2);
    after.count = 3;
    assert(dppd_recovery_compare(&before, &after, &candidate) == DPPD_EVIDENCE_AMBIGUOUS && candidate.handle == 0);
    after.count = 2;
    after.filters[0].handle = 3;
    assert(dppd_recovery_compare(&before, &after, &candidate) == DPPD_EVIDENCE_AMBIGUOUS);
    after.filters[0] = after.filters[1];
    assert(dppd_recovery_compare(&before, &after, &candidate) == DPPD_EVIDENCE_AMBIGUOUS);
    filter_messages();
    qdisc_messages();
    transport_failures();
    unknown_identity();
    cookie_messages();
    content_messages();
    return 0;
}
