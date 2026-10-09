#define _GNU_SOURCE
#include "dppd/recovery_inspect.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_link.h>
#include <linux/if_tun.h>
#include <linux/pkt_sched.h>
#include <linux/rtnetlink.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

/** 固定检查 TAP PMD 使用的两个挂载点，不把其他接口或 remote= 的规则算入本地范围 */
#define TAP_MULTIQ 0x10000U
#define TAP_INGRESS 0xffff0000U

struct link_info {
    uint32_t index;
    char name[DPPD_RECOVERY_IFNAME_SIZE];
    bool tap;
};

struct tc_query {
    uint32_t index, parent;
    bool multiq, ingress;
    struct dppd_recovery_inspection *inspection;
};

typedef int (*message_reader)(const struct nlmsghdr *, void *);

/** 属性必须完整结束，重复的已知字段也拒绝，防止歧义数据被解释成有效身份 */
static int attributes(const void *data, size_t length, const struct rtattr **table, size_t count)
{
    const struct rtattr *attribute = data;
    memset(table, 0, sizeof(*table) * count);
    while (length != 0) {
        if (length < sizeof(*attribute) || attribute->rta_len < sizeof(*attribute) ||
            RTA_ALIGN(attribute->rta_len) > length)
            return -EBADMSG;
        unsigned int type = attribute->rta_type & NLA_TYPE_MASK;
        if (type < count) {
            if (table[type] != NULL)
                return -EBADMSG;
            table[type] = attribute;
        }
        length -= RTA_ALIGN(attribute->rta_len);
        attribute = (const struct rtattr *)((const char *)attribute + RTA_ALIGN(attribute->rta_len));
    }
    return 0;
}

/** 内核字符串必须在属性范围内结束，输出空间不足时不截断成另一个看似有效的名称 */
static int attribute_string(const struct rtattr *attribute, char *output, size_t capacity)
{
    if (attribute == NULL || RTA_PAYLOAD(attribute) <= 1 || RTA_PAYLOAD(attribute) > capacity)
        return -EBADMSG;
    size_t size = RTA_PAYLOAD(attribute);
    const char *value = RTA_DATA(attribute);
    if (value[size - 1] != '\0' || strlen(value) != size - 1)
        return -EBADMSG;
    memcpy(output, value, size);
    return 0;
}

/** 每次查询单独开 socket，验证内核来源和序号，截断、丢包或中断的 dump 都不能当作完整结果 */
static int exchange(struct nlmsghdr *request, bool dump, message_reader reader, void *context)
{
    struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
    struct timeval timeout = {.tv_sec = 2};
    union { struct nlmsghdr aligned; unsigned char bytes[32768]; } buffer;
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    int rc = 0;
    if (fd < 0)
        return -errno;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        bind(fd, (struct sockaddr *)&kernel, sizeof(kernel)) != 0) {
        rc = -errno;
        goto done;
    }
    request->nlmsg_flags = NLM_F_REQUEST | (dump ? NLM_F_DUMP : 0);
    request->nlmsg_seq = 1;
    if (sendto(fd, request, request->nlmsg_len, 0, (struct sockaddr *)&kernel, sizeof(kernel)) !=
        (ssize_t)request->nlmsg_len) {
        rc = -errno;
        if (rc == 0)
            rc = -EIO;
        goto done;
    }
    /** 除接收超时外再限制消息批数，异常的持续数据不能让离线工具无限等待 */
    for (unsigned int batch = 0; batch < 128; ++batch) {
        struct sockaddr_nl sender = {0};
        struct iovec vector = {.iov_base = buffer.bytes, .iov_len = sizeof(buffer.bytes)};
        struct msghdr message = {.msg_name = &sender, .msg_namelen = sizeof(sender),
            .msg_iov = &vector, .msg_iovlen = 1};
        ssize_t received = recvmsg(fd, &message, 0);
        if (received < 0) {
            rc = -errno;
            goto done;
        }
        if (received == 0 || (message.msg_flags & MSG_TRUNC) != 0 ||
            message.msg_namelen != sizeof(sender) || sender.nl_family != AF_NETLINK || sender.nl_pid != 0) {
            rc = -EBADMSG;
            goto done;
        }
        size_t remaining = (size_t)received;
        const struct nlmsghdr *header = (const struct nlmsghdr *)buffer.bytes;
        while (remaining != 0) {
            if (remaining < sizeof(*header) || header->nlmsg_len < sizeof(*header) ||
                NLMSG_ALIGN(header->nlmsg_len) > remaining || header->nlmsg_seq != 1) {
                rc = -EBADMSG;
                goto done;
            }
            if ((header->nlmsg_flags & NLM_F_DUMP_INTR) != 0) {
                rc = -EAGAIN;
                goto done;
            }
            if (header->nlmsg_type == NLMSG_ERROR) {
                if (NLMSG_PAYLOAD(header, 0) < sizeof(struct nlmsgerr))
                    rc = -EBADMSG;
                else {
                    const struct nlmsgerr *error = NLMSG_DATA(header);
                    rc = error->error < 0 && error->error >= -4095 ? error->error : -EBADMSG;
                }
                goto done;
            }
            if (header->nlmsg_type == NLMSG_DONE) {
                rc = dump ? 0 : -EBADMSG;
                if (NLMSG_PAYLOAD(header, 0) != 0) {
                    int error;
                    if (NLMSG_PAYLOAD(header, 0) < sizeof(error))
                        rc = -EBADMSG;
                    else {
                        memcpy(&error, NLMSG_DATA(header), sizeof(error));
                        rc = error <= 0 && error >= -4095 ? error : -EBADMSG;
                    }
                }
                goto done;
            }
            rc = reader(header, context);
            if (rc != 0 || !dump)
                goto done;
            remaining -= NLMSG_ALIGN(header->nlmsg_len);
            header = (const struct nlmsghdr *)((const char *)header + NLMSG_ALIGN(header->nlmsg_len));
        }
    }
    rc = -E2BIG;
done:
    close(fd);
    return rc;
}

/** 只有 tun 类型且明确声明 IFF_TAP 的接口才作为 TAP，名称相似并不是类型证据 */
static int read_link(const struct nlmsghdr *header, void *context)
{
    struct link_info *link = context;
    const struct rtattr *attrs[IFLA_MAX + 1], *info[IFLA_INFO_MAX + 1], *tun[IFLA_TUN_MAX + 1];
    char kind[DPPD_RECOVERY_KIND_SIZE];
    if (header->nlmsg_type != RTM_NEWLINK || NLMSG_PAYLOAD(header, 0) < sizeof(struct ifinfomsg))
        return -EBADMSG;
    const struct ifinfomsg *message = NLMSG_DATA(header);
    if (message->ifi_index <= 0 || attributes(IFLA_RTA(message), IFLA_PAYLOAD(header), attrs,
        IFLA_MAX + 1) != 0 || attribute_string(attrs[IFLA_IFNAME], link->name, sizeof(link->name)) != 0)
        return -EBADMSG;
    link->index = (uint32_t)message->ifi_index;
    if (attrs[IFLA_LINKINFO] == NULL)
        return 0;
    if (attributes(RTA_DATA(attrs[IFLA_LINKINFO]), RTA_PAYLOAD(attrs[IFLA_LINKINFO]),
        info, IFLA_INFO_MAX + 1) != 0 || attribute_string(info[IFLA_INFO_KIND], kind, sizeof(kind)) != 0)
        return -EBADMSG;
    if (strcmp(kind, "tun") != 0 || info[IFLA_INFO_DATA] == NULL)
        return 0;
    if (attributes(RTA_DATA(info[IFLA_INFO_DATA]), RTA_PAYLOAD(info[IFLA_INFO_DATA]),
        tun, IFLA_TUN_MAX + 1) != 0 || tun[IFLA_TUN_TYPE] == NULL || RTA_PAYLOAD(tun[IFLA_TUN_TYPE]) != 1)
        return -EBADMSG;
    link->tap = *(const unsigned char *)RTA_DATA(tun[IFLA_TUN_TYPE]) == IFF_TAP;
    return 0;
}

/** 按索引查询后仍核对实际名称，缺失时另外按名称查找，发现同名替换接口 */
static int query_link(uint32_t index, const char *name, struct link_info *link)
{
    struct {
        struct nlmsghdr header;
        struct ifinfomsg message;
        unsigned char attributes[RTA_SPACE(DPPD_RECOVERY_IFNAME_SIZE)];
    } request = {.header = {.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)), .nlmsg_type = RTM_GETLINK},
        .message = {.ifi_family = AF_UNSPEC, .ifi_index = (int)index}};
    memset(link, 0, sizeof(*link));
    if (name != NULL) {
        struct rtattr *attribute = (struct rtattr *)request.attributes;
        attribute->rta_type = IFLA_IFNAME;
        attribute->rta_len = RTA_LENGTH(strlen(name) + 1);
        memcpy(RTA_DATA(attribute), name, strlen(name) + 1);
        request.header.nlmsg_len += RTA_ALIGN(attribute->rta_len);
    }
    return exchange(&request.header, false, read_link, link);
}

/** 启动 UUID 和当前线程网络命名空间一起定位环境，换了系统或命名空间不能报告规则已消失 */
static int current_context(struct dppd_recovery_identity *identity)
{
    struct stat info;
    char line[64];
    FILE *file = fopen("/proc/sys/kernel/random/boot_id", "re");
    if (file == NULL)
        return -errno;
    bool valid = fgets(line, sizeof(line), file) != NULL;
    fclose(file);
    if (!valid || strlen(line) != 37 || line[36] != '\n')
        return -EBADMSG;
    for (size_t index = 0; index < 36; ++index) {
        bool hyphen = index == 8 || index == 13 || index == 18 || index == 23;
        if (hyphen ? line[index] != '-' : !((line[index] >= '0' && line[index] <= '9') ||
            (line[index] >= 'a' && line[index] <= 'f')))
            return -EBADMSG;
    }
    line[36] = '\0';
    strcpy(identity->boot_id, line);
    if (stat("/proc/thread-self/ns/net", &info) != 0)
        return -errno;
    identity->netns_device = info.st_dev;
    identity->netns_inode = info.st_ino;
    return 0;
}

/** 保存前查询真实内核接口，发生错误时不降级成没有身份的 TAP 标记 */
int dppd_recovery_tap_identity(uint32_t ifindex, struct dppd_recovery_identity *identity)
{
    struct link_info link;
    int rc;
    if (identity == NULL || ifindex == 0 || ifindex > INT32_MAX)
        return -EINVAL;
    memset(identity, 0, sizeof(*identity));
    rc = current_context(identity);
    if (rc == 0)
        rc = query_link(ifindex, NULL, &link);
    if (rc == 0 && (!link.tap || link.index != ifindex))
        rc = -EXDEV;
    if (rc == 0) {
        identity->ifindex = ifindex;
        strcpy(identity->ifname, link.name);
    }
    return rc;
}

/** 先枚举挂载点再读取过滤器，挂载点类型被替换时返回不支持，避免误报完整的空列表 */
static int read_tc(const struct nlmsghdr *header, void *context)
{
    struct tc_query *query = context;
    const struct rtattr *attrs[TCA_MAX + 1];
    char kind[DPPD_RECOVERY_KIND_SIZE];
    if (header->nlmsg_type != (query->inspection == NULL ? RTM_NEWQDISC : RTM_NEWTFILTER) ||
        NLMSG_PAYLOAD(header, 0) < sizeof(struct tcmsg))
        return -EBADMSG;
    const struct tcmsg *message = NLMSG_DATA(header);
    /** qdisc dump 在部分内核返回所有接口，按明确索引筛选，不把其他接口混入结果 */
    if (query->inspection == NULL && (uint32_t)message->tcm_ifindex != query->index)
        return 0;
    if ((uint32_t)message->tcm_ifindex != query->index || (query->inspection != NULL &&
        message->tcm_parent != query->parent))
        return -EBADMSG;
    if (attributes(TCA_RTA(message), TCA_PAYLOAD(header), attrs, TCA_MAX + 1) != 0 ||
        attribute_string(attrs[TCA_KIND], kind, sizeof(kind)) != 0)
        return -EBADMSG;
    if (query->inspection == NULL) {
        if (message->tcm_handle == TAP_MULTIQ) {
            if (strcmp(kind, "multiq") != 0)
                return -ENOTSUP;
            query->multiq = true;
        } else if (message->tcm_handle == TAP_INGRESS) {
            if (strcmp(kind, "ingress") != 0)
                return -ENOTSUP;
            query->ingress = true;
        }
        return 0;
    }
    /** 内核还会返回没有具体 handle 的分类器标题，它不是一条可定位的实际规则 */
    if (message->tcm_handle == 0)
        return 0;
    if (query->inspection->count == DPPD_RECOVERY_FILTER_LIMIT)
        return -E2BIG;
    struct dppd_recovery_filter *filter = &query->inspection->filters[query->inspection->count];
    if (attrs[TCA_CHAIN] != NULL) {
        if (RTA_PAYLOAD(attrs[TCA_CHAIN]) != sizeof(filter->chain))
            return -EBADMSG;
        memcpy(&filter->chain, RTA_DATA(attrs[TCA_CHAIN]), sizeof(filter->chain));
    }
    filter->parent = message->tcm_parent;
    filter->handle = message->tcm_handle;
    filter->priority = (uint16_t)(message->tcm_info >> 16);
    filter->protocol = ntohs((uint16_t)message->tcm_info);
    strcpy(filter->kind, kind);
    query->inspection->count++;
    return 0;
}

/** dump 仅使用 GET 请求，不发送 NEW、DEL、flush 或设备复位指令 */
static int query_tc(struct tc_query *query)
{
    struct { struct nlmsghdr header; struct tcmsg message; } request = {
        .header = {.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg)),
            .nlmsg_type = query->inspection == NULL ? RTM_GETQDISC : RTM_GETTFILTER},
        .message = {.tcm_family = AF_UNSPEC, .tcm_ifindex = (int)query->index, .tcm_parent = query->parent}};
    return exchange(&request.header, true, read_tc, query);
}

/** 索引和名称同时一致仍不能证明接口从未重建，所以结果只描述当前坐标匹配 */
static int locate(const struct dppd_recovery_identity *identity, struct dppd_recovery_inspection *inspection)
{
    struct link_info link;
    int rc = query_link(identity->ifindex, NULL, &link);
    if (rc == -ENODEV) {
        rc = query_link(0, identity->ifname, &link);
        if (rc == -ENODEV) {
            inspection->state = DPPD_INSPECT_ABSENT;
            return 0;
        }
        if (rc == 0) {
            inspection->state = DPPD_INSPECT_IDENTITY_MISMATCH;
            return -EXDEV;
        }
    }
    if (rc == 0 && (!link.tap || link.index != identity->ifindex || strcmp(link.name, identity->ifname) != 0)) {
        inspection->state = DPPD_INSPECT_IDENTITY_MISMATCH;
        return -EXDEV;
    }
    if (rc == 0)
        inspection->state = DPPD_INSPECT_PRESENT;
    return rc;
}

/** 旧记录和其他 PMD 明确返回未知，局部无规则也不会自动改写恢复保护文件 */
int dppd_recovery_inspect(const struct dppd_recovery_port *port, struct dppd_recovery_inspection *inspection)
{
    struct dppd_recovery_identity current = {0};
    struct tc_query query;
    int rc;
    if (port == NULL || inspection == NULL)
        return -EINVAL;
    memset(inspection, 0, sizeof(*inspection));
    if (strcmp(port->driver, "net_tap") != 0) {
        inspection->state = DPPD_INSPECT_UNSUPPORTED;
        return -ENOTSUP;
    }
    if (port->identity.ifindex == 0) {
        inspection->state = DPPD_INSPECT_NO_IDENTITY;
        return -ENODATA;
    }
    rc = current_context(&current);
    if (rc != 0)
        return rc;
    if (strcmp(current.boot_id, port->identity.boot_id) != 0 ||
        current.netns_device != port->identity.netns_device || current.netns_inode != port->identity.netns_inode) {
        inspection->state = DPPD_INSPECT_CONTEXT_MISMATCH;
        return -EXDEV;
    }
    rc = locate(&port->identity, inspection);
    if (rc != 0 || inspection->state == DPPD_INSPECT_ABSENT)
        return rc;
    query = (struct tc_query){.index = port->identity.ifindex};
    rc = query_tc(&query);
    if (rc == 0 && query.multiq) {
        query.inspection = inspection;
        query.parent = TAP_MULTIQ;
        rc = query_tc(&query);
    }
    if (rc == 0 && query.ingress) {
        query.inspection = inspection;
        query.parent = TAP_INGRESS;
        rc = query_tc(&query);
    }
    /** 查询结束再次核对接口，明显的并发移除必须报告失败，不能保留此前部分结果 */
    if (rc == 0) {
        rc = locate(&port->identity, inspection);
        if (rc == 0 && inspection->state != DPPD_INSPECT_PRESENT)
            rc = -EAGAIN;
    }
    if (rc != 0) {
        memset(inspection, 0, sizeof(*inspection));
        inspection->state = DPPD_INSPECT_UNAVAILABLE;
    }
    return rc;
}

/** 名称描述观察结论而非清理结论，coordinates-match 不等于对象归属已证明 */
const char *dppd_recovery_inspection_name(enum dppd_recovery_inspection_state state)
{
    switch (state) {
    case DPPD_INSPECT_UNSUPPORTED: return "unsupported-driver";
    case DPPD_INSPECT_NO_IDENTITY: return "identity-unavailable";
    case DPPD_INSPECT_CONTEXT_MISMATCH: return "context-mismatch";
    case DPPD_INSPECT_IDENTITY_MISMATCH: return "identity-mismatch";
    case DPPD_INSPECT_ABSENT: return "interface-absent";
    case DPPD_INSPECT_PRESENT: return "coordinates-match";
    default: return "unavailable";
    }
}
