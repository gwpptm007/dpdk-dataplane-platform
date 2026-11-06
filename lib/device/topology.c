#include "dppd/topology.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <linux/if_link.h>
#include <linux/rtnetlink.h>
#include <rte_dev.h>
#include <rte_ethdev.h>
#include <rte_version.h>

/** 每次失败都清掉旧角色，防止复用结构时保留上一块设备的身份 */
static void clear_role(struct dppd_endpoint *endpoint, int error)
{
    endpoint->role = DPPD_ENDPOINT_ROLE_UNKNOWN;
    endpoint->role_source = DPPD_ENDPOINT_ROLE_SOURCE_UNKNOWN;
    endpoint->role_error = error;
    endpoint->has_controller = false;
    endpoint->has_pf = false;
    endpoint->has_function = false;
    endpoint->has_physical_port = false;
    endpoint->controller_id = 0;
    endpoint->pf_id = 0;
    endpoint->function_id = 0;
    endpoint->physical_port_id = 0;
}

/** 十进制编号必须非负且完整，逐位检查避免 sscanf 接受负数、空白或整数溢出 */
static int parse_number(const char **cursor, uint32_t *value)
{
    const char *text = *cursor;
    uint32_t result = 0;

    if (*text < '0' || *text > '9')
        return -EINVAL;
    do {
        uint32_t digit = (uint32_t)(*text - '0');
        if (result > (UINT32_MAX - digit) / 10U)
            return -ERANGE;
        result = result * 10U + digit;
        ++text;
    } while (*text >= '0' && *text <= '9');
    *cursor = text;
    *value = result;
    return 0;
}

/**
 * 按 mlx5 的物理端口名解析端口角色和编号，结果写入 endpoint
 * 保存名称 → 解析可选 controller 和端口编号 → 核对 representor 标志 → 保存结果
 * 例如 c0pf1vf2 → controller 0、PF 1、VF 2；p0 → 物理上联端口 0
 * driver_name 指定驱动，is_representor 来自 PMD 标志；PF/VF/SF 表示代理口所代表的角色
 * 成功返回 0，失败返回负错误码，角色保持 UNKNOWN 并记录 role_error
 */
int dppd_topology_resolve_role(struct dppd_endpoint *endpoint, const char *driver_name, bool is_representor,
    const char *phys_port_name)
{
    struct dppd_endpoint parsed = {0};
    char name[sizeof(endpoint->phys_port_name)];
    const char *cursor = name;
    size_t length = phys_port_name == NULL ? 0 : strnlen(phys_port_name, sizeof(name));
    int rc = -EINVAL;

    if (endpoint == NULL)
        return -EINVAL;
    /** 先保存输入，允许调用方直接传入 endpoint 自己保存的原始名称 */
    if (phys_port_name != NULL && length < sizeof(name))
        memcpy(name, phys_port_name, length + 1U);
    clear_role(endpoint, -EINVAL);
    endpoint->phys_port_name[0] = '\0';
    if (driver_name == NULL || phys_port_name == NULL)
        return -EINVAL;
    if (length >= sizeof(name)) {
        endpoint->role_error = -ENAMETOOLONG;
        return -ENAMETOOLONG;
    }
    memcpy(endpoint->phys_port_name, name, length + 1U);
    if (strcmp(driver_name, "mlx5_pci") != 0 && strcmp(driver_name, "net_mlx5") != 0) {
        endpoint->role_error = -ENOTSUP;
        return -ENOTSUP;
    }
    /** 可选前缀 c0 表示 controller 0，parse_number 同时把 cursor 移到后面的 pf 或 p */
    if (*cursor == 'c') {
        ++cursor;
        rc = parse_number(&cursor, &parsed.controller_id);
        if (rc != 0)
            goto failed;
        parsed.has_controller = true;
    }
    /** pf1 先确定 PF 编号，后接 vf2 或 sf2 时再确定其所代表的 VF/SF 及编号 */
    if (strncmp(cursor, "pf", 2) == 0) {
        cursor += 2;
        rc = parse_number(&cursor, &parsed.pf_id);
        if (rc != 0)
            goto failed;
        parsed.has_pf = true;
        parsed.role = DPPD_ENDPOINT_ROLE_PF;
        if (strncmp(cursor, "vf", 2) == 0 || strncmp(cursor, "sf", 2) == 0) {
            parsed.role = *cursor == 'v' ? DPPD_ENDPOINT_ROLE_VF : DPPD_ENDPOINT_ROLE_SF;
            cursor += 2;
            rc = parse_number(&cursor, &parsed.function_id);
            if (rc != 0)
                goto failed;
            parsed.has_function = true;
        }
    } else if (*cursor == 'p') {
        /** p0 是物理上联端口 0，和 pf0 所代表的 PF 0 分别记录 */
        ++cursor;
        rc = parse_number(&cursor, &parsed.physical_port_id);
        if (rc != 0)
            goto failed;
        parsed.has_physical_port = true;
        parsed.role = DPPD_ENDPOINT_ROLE_UPLINK;
    } else {
        rc = -EINVAL;
        goto failed;
    }
    /** 必须消耗整个名称，不能把 pf1vf2tail 仅按合法前缀 pf1vf2 接受 */
    if (*cursor != '\0') {
        rc = -EINVAL;
        goto failed;
    }
    /** 本项目要求：UPLINK 对应非代理口，PF/VF/SF 对应代理口；名称与 PMD 标志冲突则拒绝 */
    if ((parsed.role != DPPD_ENDPOINT_ROLE_UPLINK) != is_representor) {
        rc = -EPROTO;
        goto failed;
    }
    /** parsed 只保存本次解析结果，通过所有检查后才写入角色、有效标志和编号 */
    endpoint->role = parsed.role;
    endpoint->role_source = DPPD_ENDPOINT_ROLE_SOURCE_MLX5_PHYS_PORT_NAME;
    endpoint->role_error = 0;
    endpoint->has_controller = parsed.has_controller;
    endpoint->has_pf = parsed.has_pf;
    endpoint->has_function = parsed.has_function;
    endpoint->has_physical_port = parsed.has_physical_port;
    endpoint->controller_id = parsed.controller_id;
    endpoint->pf_id = parsed.pf_id;
    endpoint->function_id = parsed.function_id;
    endpoint->physical_port_id = parsed.physical_port_id;
    return 0;

failed:
    clear_role(endpoint, rc);
    return rc;
}

/** 内核返回的字符串必须带完整结尾，不能截断后继续认领身份 */
static int copy_attribute(char *output, size_t capacity, const struct rtattr *attribute)
{
    size_t length = RTA_PAYLOAD(attribute);
    const char *value = RTA_DATA(attribute);

    if (length == 0 || length > capacity || value[length - 1U] != '\0')
        return -EOVERFLOW;
    if (memchr(value, '\0', length - 1U) != NULL)
        return -EPROTO;
    memcpy(output, value, length);
    return 0;
}

/**
 * 按 if_index 查询当前网络命名空间中的接口，填写接口编号、接口名和可选物理端口名
 * 组装 RTM_GETLINK 请求 → 发送给内核 → 校验回复并提取属性 → 关闭 socket
 * 成功返回 0，失败返回负错误码，失败时输出可能已部分填写，由调用方丢弃
 * sysfs 挂载可能仍指向宿主，先查询当前网络命名空间，再由 read_identity 交叉核对
 */
static int read_netlink_identity(uint32_t if_index, struct dppd_topology_identity *identity)
{
    /**
     * 请求布局：[Netlink 消息头 header][接口查询条件 link]
     * RTM_GETLINK 表示查询，ifi_index 指定接口；seq 用于对应请求和回复
     */
    struct {
        struct nlmsghdr header;
        struct ifinfomsg link;
    } request = {.header = {.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)),
                     .nlmsg_type = RTM_GETLINK,
                     .nlmsg_flags = NLM_F_REQUEST,
                     .nlmsg_seq = 1},
        .link = {.ifi_family = AF_UNSPEC, .ifi_index = (int)if_index}};
    struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 200000};
    /** 按消息头要求对齐接收缓冲；vector 指向缓冲，message 同时接收发送方地址 */
    _Alignas(struct nlmsghdr) char buffer[16384];
    struct iovec vector = {.iov_base = buffer, .iov_len = sizeof(buffer)};
    struct msghdr message = {.msg_name = &kernel, .msg_namelen = sizeof(kernel), .msg_iov = &vector, .msg_iovlen = 1};
    struct nlmsghdr *header;
    ssize_t received;
    int remaining, fd, rc = -ENODATA;
    bool has_interface = false, has_phys_port = false;

    if (if_index == 0 || if_index > INT_MAX)
        return -ENODEV;
    /** NETLINK_ROUTE 查询内核网络信息，消息不经业务网卡发出；接收最多等待 200 ms */
    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return -errno;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        rc = -errno;
        goto done;
    }
    if (sendto(fd, &request, request.header.nlmsg_len, 0, (struct sockaddr *)&kernel, sizeof(kernel)) !=
        (ssize_t)request.header.nlmsg_len) {
        rc = errno == 0 ? -EIO : -errno;
        goto done;
    }
    received = recvmsg(fd, &message, 0);
    if (received < 0) {
        rc = -errno;
        goto done;
    }
    /** 拒绝被截断的回复，并检查发送方为内核单播地址 */
    if ((message.msg_flags & MSG_TRUNC) != 0 || kernel.nl_pid != 0 || kernel.nl_groups != 0) {
        rc = -EOVERFLOW;
        goto done;
    }
    if (message.msg_namelen < sizeof(kernel) || kernel.nl_family != AF_NETLINK) {
        rc = -EPROTO;
        goto done;
    }
    remaining = (int)received;
    /** 一次接收可包含多条消息，按各自长度和对齐位置遍历，只处理本次请求的回复 */
    for (header = (struct nlmsghdr *)buffer; NLMSG_OK(header, remaining); header = NLMSG_NEXT(header, remaining)) {
        struct ifinfomsg *link;
        struct rtattr *attribute;
        int attributes;

        if (header->nlmsg_seq != request.header.nlmsg_seq)
            continue;
        if (header->nlmsg_type == NLMSG_ERROR) {
            if (header->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr)))
                rc = -EPROTO;
            else
                rc = ((struct nlmsgerr *)NLMSG_DATA(header))->error;
            if (rc >= 0)
                rc = -ENODATA;
            goto done;
        }
        /** RTM_NEWLINK 在这里承载接口信息，收到它并不表示本函数创建了接口 */
        if (header->nlmsg_type != RTM_NEWLINK || header->nlmsg_len < NLMSG_LENGTH(sizeof(*link)))
            continue;
        link = NLMSG_DATA(header);
        if (link->ifi_index != (int)if_index)
            continue;
        identity->if_index = if_index;
        attributes = IFLA_PAYLOAD(header);
        /** 属性按类型、长度和值组织，只提取接口名和物理端口名，其他属性跳过 */
        for (attribute = IFLA_RTA(link); RTA_OK(attribute, attributes); attribute = RTA_NEXT(attribute, attributes)) {
            if (attribute->rta_type == IFLA_IFNAME) {
                if (has_interface) {
                    rc = -EPROTO;
                    goto done;
                }
                has_interface = true;
                rc = copy_attribute(identity->interface_name, sizeof(identity->interface_name), attribute);
            } else if (attribute->rta_type == IFLA_PHYS_PORT_NAME) {
                if (has_phys_port) {
                    rc = -EPROTO;
                    goto done;
                }
                has_phys_port = true;
                rc = copy_attribute(identity->phys_port_name, sizeof(identity->phys_port_name), attribute);
            } else
                continue;
            if (rc != 0)
                goto done;
        }
        /** 物理端口名是可选证据，通用 PCI 接口缺少它时仍须继续核对接口和设备 */
        rc = attributes != 0 ? -EPROTO : identity->interface_name[0] == '\0' ? -ENODATA : 0;
        goto done;
    }

    /** socket 创建成功后统一从这里关闭，查询失败也不会遗漏释放 */
done:
    close(fd);
    return rc;
}

static bool valid_interface_name(const char *name)
{
    if (*name == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    for (; *name != '\0'; ++name)
        if (!((*name >= 'a' && *name <= 'z') || (*name >= 'A' && *name <= 'Z') || (*name >= '0' && *name <= '9') ||
                *name == '_' || *name == '-' || *name == '.'))
            return false;
    return true;
}

static bool valid_pci_name(const char *name)
{
    unsigned int domain, bus, slot, function;
    char canonical[16];
    int end = 0;

    if (name == NULL || strlen(name) != 12 ||
        sscanf(name, "%4x:%2x:%2x.%1x%n", &domain, &bus, &slot, &function, &end) != 4 || end != 12 || slot > 31U ||
        function > 7U)
        return false;
    snprintf(canonical, sizeof(canonical), "%04x:%02x:%02x.%x", domain, bus, slot, function);
    return strcmp(name, canonical) == 0;
}

/** 读取上限包含结尾检查，多读到一个字节就报错，不把长字符串截成合法前缀 */
static int read_sysfs_text(const char *path, char *output, size_t capacity)
{
    size_t used = 0;
    int rc = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

    if (fd < 0)
        return -errno;
    while (used < capacity) {
        ssize_t length = read(fd, output + used, capacity - used);
        if (length < 0 && errno == EINTR)
            continue;
        if (length < 0) {
            rc = -errno;
            break;
        }
        if (length == 0)
            break;
        used += (size_t)length;
    }
    close(fd);
    if (rc != 0) {
        /** 已读到半份内容后才报缺失或不支持，不能把它当成可选字段完全不存在 */
        if (used != 0 && (rc == -ENOENT || rc == -EOPNOTSUPP || rc == -ENODATA))
            return -EIO;
        return rc;
    }
    if (used >= capacity)
        return -EOVERFLOW;
    if (used > 0 && output[used - 1U] == '\n')
        --used;
    output[used] = '\0';
    if (memchr(output, '\0', used) != NULL)
        return -EPROTO;
    if (used == 0)
        return -ENODATA;
    return 0;
}

/** 双方没有物理端口名时允许通用身份成立，权限或格式错误不能被吞成“没有名称” */
static int read_optional_phys_port(const char *path, char *output, size_t capacity)
{
    int rc = read_sysfs_text(path, output, capacity);

    if (rc == -ENOENT || rc == -EOPNOTSUPP || rc == -ENODATA) {
        output[0] = '\0';
        return 0;
    }
    return rc;
}

/**
 * 核对 if_index 对应的 Linux 接口是否属于 device_name 指定的 PCI 设备
 * Netlink 查询接口 → sysfs 核对编号、设备和物理端口名 → 再查接口是否变化
 * 全部通过后写入 identity 并返回 0，失败返回负错误码，不写入输出
 */
static int read_identity(uint32_t if_index, const char *device_name, struct dppd_topology_identity *identity,
    void *opaque)
{
    struct dppd_topology_identity observed = {0}, current = {0};
    char path[PATH_MAX], target[PATH_MAX], text[64];
    const char *basename;
    const char *cursor;
    uint32_t observed_index;
    int rc;

    (void)opaque;
    if (!valid_pci_name(device_name))
        return -ENOTSUP;
    /** Netlink 向当前网络命名空间的内核查询接口，observed 保存首次结果 */
    rc = read_netlink_identity(if_index, &observed);
    if (rc != 0)
        return rc;
    if (!valid_interface_name(observed.interface_name))
        return -EINVAL;
    /** /sys/class/net/enp3s0/ifindex 例如编号 5 查到 enp3s0，还需确认 sysfs 中 enp3s0 的 ifindex 也是 5 */
    snprintf(path, sizeof(path), "/sys/class/net/%s/ifindex", observed.interface_name);
    rc = read_sysfs_text(path, text, sizeof(text));
    if (rc != 0)
        return rc;
    cursor = text;
    if (parse_number(&cursor, &observed_index) != 0 || *cursor != '\0' || observed_index != if_index)
        return -EXDEV;
    /** device 是设备链接，解析实际路径后取末段 PCI 名称，与 PMD 提供的设备名比较 */
    snprintf(path, sizeof(path), "/sys/class/net/%s/device", observed.interface_name);
    if (realpath(path, target) == NULL)
        return -errno;
    basename = strrchr(target, '/');
    if (basename == NULL || strcmp(basename + 1, device_name) != 0)
        return -EXDEV;
    /** 物理端口名也要与 Netlink 一致；两边都没有名称时允许继续，具体角色由上层判断 */
    snprintf(path, sizeof(path), "/sys/class/net/%s/phys_port_name", observed.interface_name);
    rc = read_optional_phys_port(path, text, sizeof(text));
    if (rc != 0)
        return rc;
    if (strcmp(text, observed.phys_port_name) != 0)
        return -EXDEV;
    /** 读取过程中接口可能改名或编号被复用，再查询并比较；发现变化返回 -EAGAIN */
    rc = read_netlink_identity(if_index, &current);
    if (rc != 0)
        return rc;
    if (strcmp(current.interface_name, observed.interface_name) != 0 ||
        strcmp(current.phys_port_name, observed.phys_port_name) != 0 ||
        if_nametoindex(observed.interface_name) != if_index)
        return -EAGAIN;
    /** 前面的步骤都只修改局部变量，成功后才一次性复制给调用方 */
    memcpy(observed.device_name, device_name, strlen(device_name) + 1U);
    *identity = observed;
    return 0;
}

static const char *get_device_name(const struct rte_eth_dev_info *info)
{
    if (info->device == NULL)
        return NULL;
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
    return rte_dev_name(info->device);
#else
    return info->device->name;
#endif
}

/**
 * 为当前网口补充设备名称、已核对的 Linux 接口名和可确认的端口角色，结果写入 endpoint
 * 保存 PMD 观察 → 检查 PCI 名称和接口编号 → reader 核对接口身份
 *   → 校验读取器结果并保存名称 → 对支持的 mlx5 网口继续解析角色
 * 正式入口传入的 reader 是 read_identity，opaque 原样传给读取器
 * 无法确认时保留 UNKNOWN 并填写 role_error，本函数返回不会中止外层的基础网口收集
 */
static void discover_identity(struct dppd_endpoint *endpoint, const struct rte_eth_dev_info *info,
    dppd_topology_identity_reader reader, void *opaque)
{
    struct dppd_topology_identity identity = {0};
    const char *device_name = get_device_name(info);
    int rc;

    endpoint->if_index = info->if_index;
    /** 先清掉旧角色及编号，后面只用本次核对通过的信息确认角色 */
    clear_role(endpoint, -ENOTSUP);
    /** 可信接口字段只从完整成功的读取器发布，错误时不保留容易被误认领的原始名称 */
    endpoint->interface_name[0] = endpoint->phys_port_name[0] = '\0';
    if (device_name != NULL && strnlen(device_name, sizeof(endpoint->device_name)) < sizeof(endpoint->device_name))
        memcpy(endpoint->device_name, device_name, strlen(device_name) + 1U);
    bool mlx5 = strcmp(endpoint->driver_name, "mlx5_pci") == 0 || strcmp(endpoint->driver_name, "net_mlx5") == 0;
    if (device_name == NULL || device_name[0] == '\0') {
        endpoint->role_error = mlx5 || info->if_index != 0 ? -ENODATA : -ENOTSUP;
        return;
    }
    /** 当前正式读取器按 PCI 设备与内核接口核对身份，非 PCI 名称保持未识别状态 */
    if (!valid_pci_name(device_name))
        return;
    if (info->if_index == 0 || info->if_index > INT_MAX) {
        endpoint->role_error = -ENODEV;
        return;
    }
    if (reader == NULL) {
        endpoint->role_error = -ENODATA;
        return;
    }
    /** 正式入口中实际调用 read_identity，identity 接收核对后的接口信息 */
    rc = reader(info->if_index, device_name, &identity, opaque);
    if (rc != 0) {
        endpoint->role_error = rc < 0 ? rc : -EIO;
        return;
    }
    if (identity.if_index != info->if_index || identity.if_index == 0 ||
        memchr(identity.device_name, '\0', sizeof(identity.device_name)) == NULL ||
        memchr(identity.interface_name, '\0', sizeof(identity.interface_name)) == NULL ||
        memchr(identity.phys_port_name, '\0', sizeof(identity.phys_port_name)) == NULL ||
        !valid_interface_name(identity.interface_name) || strcmp(identity.device_name, device_name) != 0) {
        endpoint->role_error = -EXDEV;
        return;
    }
    memcpy(endpoint->interface_name, identity.interface_name, sizeof(endpoint->interface_name));
    memcpy(endpoint->phys_port_name, identity.phys_port_name, sizeof(endpoint->phys_port_name));
    /** 接口核对成功后保存名称；角色解析目前仅支持 mlx5，其他驱动仍保留 UNKNOWN */
    if (!mlx5)
        return;
    if (identity.phys_port_name[0] == '\0') {
        endpoint->role_error = -ENODATA;
        return;
    }
    dppd_topology_resolve_role(endpoint, endpoint->driver_name, endpoint->kind == DPPD_ENDPOINT_REPRESENTOR,
        identity.phys_port_name);
}

/** PMD 名称也要完整复制，截断后碰巧相同的前缀不能作为重新发现时的身份证据 */
static int copy_pmd_name(char *destination, size_t capacity, const char *value, const char *unknown)
{
    if (value == NULL)
        value = unknown;
    size_t length = strnlen(value, capacity);
    if (length == capacity)
        return -ENAMETOOLONG;
    memcpy(destination, value, length + 1U);
    return 0;
}

/**
 * 按 cfg 的网口列表读取 PMD 信息，生成网口身份与关联记录，reader 用于进一步核对接口身份
 * 清空输出 → 逐口填入临时 discovered → 基础信息全部读取成功后整体写入 topology
 * 身份或角色无法确认时保留 UNKNOWN 和 role_error；参数、PMD 查询或名称复制错误返回负错误码
 * 成功返回 0；topology 非空时，失败不会输出只发现一半的网口列表
 */
int dppd_topology_discover_with_reader(const struct dppd_config *cfg, struct dppd_topology *topology,
    dppd_topology_identity_reader reader, void *opaque)
{
    /** 临时结果与调用方的输出分开，发现中途失败时不发布部分结果 */
    struct dppd_topology discovered = {0};

    if (topology == NULL)
        return -EINVAL;
    memset(topology, 0, sizeof(*topology));
    if (cfg == NULL || cfg->nb_ports > DPPD_MAX_PORTS)
        return -EINVAL;
    for (uint16_t i = 0; i < cfg->nb_ports; ++i)
        for (uint16_t j = 0; j < i; ++j)
            if (cfg->ports[i] == cfg->ports[j])
                return -EEXIST;

    for (uint16_t i = 0; i < cfg->nb_ports; ++i) {
        struct rte_eth_dev_info info = {0};
        /** 本轮填写下一个空条目，完成当前网口后才增加 nb_endpoints */
        struct dppd_endpoint *endpoint = &discovered.endpoints[discovered.nb_endpoints];
        int rc = rte_eth_dev_info_get(cfg->ports[i], &info);

        if (rc != 0)
            return rc;
        endpoint->ethdev_port_id = cfg->ports[i];
        endpoint->kind = DPPD_ENDPOINT_ETHDEV;
        if (info.dev_flags != NULL && ((*info.dev_flags & RTE_ETH_DEV_REPRESENTOR) != 0))
            endpoint->kind = DPPD_ENDPOINT_REPRESENTOR;
        endpoint->has_switch_domain = info.switch_info.domain_id != RTE_ETH_DEV_SWITCH_DOMAIN_ID_INVALID;
        endpoint->switch_domain_id = info.switch_info.domain_id;
        endpoint->switch_port_id = info.switch_info.port_id;
        rc = copy_pmd_name(endpoint->switch_name, sizeof(endpoint->switch_name), info.switch_info.name, "-");
        if (rc == 0)
            rc = copy_pmd_name(endpoint->driver_name, sizeof(endpoint->driver_name), info.driver_name, "unknown");
        if (rc != 0)
            return rc;
        const char *device_name = get_device_name(&info);
        if (device_name != NULL && strnlen(device_name, sizeof(endpoint->device_name)) == sizeof(endpoint->device_name))
            return -ENAMETOOLONG;
        /** reader 核对接口身份，无法识别的角色及原因保存在当前条目中，不凭端口编号猜测 */
        discover_identity(endpoint, &info, reader, opaque);
        discovered.nb_endpoints++;
    }
    /** 所有 PMD 信息读取成功后才发布，调用方不会拿到只发现一半的拓扑 */
    *topology = discovered;
    return 0;
}

/** 使用正式身份读取函数 read_identity 发现 cfg 选定的网口，结果写入 topology，返回发现结果 */
int dppd_topology_discover(const struct dppd_config *cfg, struct dppd_topology *topology)
{
    // read_identity 函数指针
    return dppd_topology_discover_with_reader(cfg, topology, read_identity, NULL);
}

static bool role_is_valid(enum dppd_endpoint_role role)
{
    switch (role) {
    case DPPD_ENDPOINT_ROLE_UNKNOWN:
    case DPPD_ENDPOINT_ROLE_UPLINK:
    case DPPD_ENDPOINT_ROLE_PF:
    case DPPD_ENDPOINT_ROLE_VF:
    case DPPD_ENDPOINT_ROLE_SF:
        return true;
    }
    return false;
}

/** 不读取未终止字符串，也不让两份同样缺失的设备身份互相作证 */
static int identity_shape(const struct dppd_endpoint *endpoint)
{
    if ((endpoint->kind != DPPD_ENDPOINT_ETHDEV && endpoint->kind != DPPD_ENDPOINT_REPRESENTOR) ||
        !role_is_valid(endpoint->role) ||
        (endpoint->role_source != DPPD_ENDPOINT_ROLE_SOURCE_UNKNOWN &&
            endpoint->role_source != DPPD_ENDPOINT_ROLE_SOURCE_MLX5_PHYS_PORT_NAME) ||
        (endpoint->has_switch_domain && endpoint->switch_domain_id == UINT16_MAX))
        return -EINVAL;
    if (memchr(endpoint->switch_name, '\0', sizeof(endpoint->switch_name)) == NULL ||
        memchr(endpoint->driver_name, '\0', sizeof(endpoint->driver_name)) == NULL ||
        memchr(endpoint->interface_name, '\0', sizeof(endpoint->interface_name)) == NULL ||
        memchr(endpoint->device_name, '\0', sizeof(endpoint->device_name)) == NULL ||
        memchr(endpoint->phys_port_name, '\0', sizeof(endpoint->phys_port_name)) == NULL)
        return -EINVAL;
    if (endpoint->driver_name[0] == '\0' || strcmp(endpoint->driver_name, "unknown") == 0 ||
        endpoint->device_name[0] == '\0')
        return -ENODATA;
    return 0;
}

static int identity_port_set(const struct dppd_topology *topology, const uint16_t *ports, uint16_t count)
{
    if (topology == NULL || topology->nb_endpoints > DPPD_MAX_PORTS)
        return -EINVAL;
    for (uint16_t i = 0; i < topology->nb_endpoints; ++i) {
        for (uint16_t j = 0; j < i; ++j)
            if (topology->endpoints[i].ethdev_port_id == topology->endpoints[j].ethdev_port_id)
                return -EUCLEAN;
        int rc = identity_shape(&topology->endpoints[i]);
        if (rc != 0)
            return rc;
    }
    for (uint16_t i = 0; i < count; ++i)
        if (dppd_topology_find(topology, ports[i]) == NULL)
            return -ENOENT;
    return topology->nb_endpoints == count ? 0 : -EXDEV;
}

/** 有效标志本身也属于身份，标志为未知时不使用残留编号猜测同一端点 */
static bool same_identity(const struct dppd_endpoint *left, const struct dppd_endpoint *right)
{
    return left->ethdev_port_id == right->ethdev_port_id && left->kind == right->kind &&
        left->has_switch_domain == right->has_switch_domain &&
        (!left->has_switch_domain ||
            (left->switch_domain_id == right->switch_domain_id && left->switch_port_id == right->switch_port_id)) &&
        left->role == right->role && left->role_source == right->role_source && left->role_error == right->role_error &&
        left->if_index == right->if_index && left->has_controller == right->has_controller &&
        (!left->has_controller || left->controller_id == right->controller_id) && left->has_pf == right->has_pf &&
        (!left->has_pf || left->pf_id == right->pf_id) && left->has_function == right->has_function &&
        (!left->has_function || left->function_id == right->function_id) &&
        left->has_physical_port == right->has_physical_port &&
        (!left->has_physical_port || left->physical_port_id == right->physical_port_id) &&
        strcmp(left->switch_name, right->switch_name) == 0 && strcmp(left->driver_name, right->driver_name) == 0 &&
        strcmp(left->interface_name, right->interface_name) == 0 &&
        strcmp(left->device_name, right->device_name) == 0 && strcmp(left->phys_port_name, right->phys_port_name) == 0;
}

int dppd_topology_validate_identity(const struct dppd_topology *expected, const struct dppd_topology *candidate,
    const uint16_t *ports, uint16_t count)
{
    if (ports == NULL || count == 0 || count > DPPD_MAX_PORTS)
        return -EINVAL;
    for (uint16_t i = 0; i < count; ++i) {
        if (ports[i] == UINT16_MAX)
            return -EINVAL;
        for (uint16_t j = 0; j < i; ++j)
            if (ports[i] == ports[j])
                return -EEXIST;
    }
    int rc = identity_port_set(expected, ports, count);
    if (rc == 0)
        rc = identity_port_set(candidate, ports, count);
    if (rc != 0)
        return rc;
    for (uint16_t i = 0; i < count; ++i)
        if (!same_identity(dppd_topology_find(expected, ports[i]), dppd_topology_find(candidate, ports[i])))
            return -EXDEV;
    return 0;
}

int dppd_topology_rediscover_verified_with_reader(const struct dppd_config *cfg, const struct dppd_topology *expected,
    struct dppd_topology *output, dppd_topology_identity_reader reader, void *opaque)
{
    if (cfg == NULL || expected == NULL || output == NULL)
        return -EINVAL;
    int rc = dppd_topology_validate_identity(expected, expected, cfg->ports, cfg->nb_ports);
    if (rc != 0)
        return rc;
    struct dppd_topology candidate;
    rc = dppd_topology_discover_with_reader(cfg, &candidate, reader, opaque);
    if (rc == 0)
        rc = dppd_topology_validate_identity(expected, &candidate, cfg->ports, cfg->nb_ports);
    if (rc == 0)
        *output = candidate;
    return rc;
}

int dppd_topology_rediscover_verified(const struct dppd_config *cfg, const struct dppd_topology *expected,
    struct dppd_topology *output)
{
    return dppd_topology_rediscover_verified_with_reader(cfg, expected, output, read_identity, NULL);
}

const struct dppd_endpoint *dppd_topology_find(const struct dppd_topology *topology, uint16_t ethdev_port_id)
{
    if (topology == NULL || topology->nb_endpoints > DPPD_MAX_PORTS)
        return NULL;
    for (uint16_t i = 0; i < topology->nb_endpoints; ++i) {
        if (topology->endpoints[i].ethdev_port_id == ethdev_port_id)
            return &topology->endpoints[i];
    }
    return NULL;
}

void dppd_topology_dump(const struct dppd_topology *topology)
{
    if (topology == NULL || topology->nb_endpoints > DPPD_MAX_PORTS)
        return;
    for (uint16_t i = 0; i < topology->nb_endpoints; ++i) {
        const struct dppd_endpoint *endpoint = &topology->endpoints[i];
        printf("[dppd] endpoint port=%u kind=%s role=%s role-source=%s role-error=%d driver=%s switch-domain=%s",
            endpoint->ethdev_port_id, endpoint->kind == DPPD_ENDPOINT_REPRESENTOR ? "representor" : "ethdev",
            dppd_endpoint_role_name(endpoint->role), dppd_endpoint_role_source_name(endpoint->role_source),
            endpoint->role_error, endpoint->driver_name, endpoint->has_switch_domain ? "yes" : "no");
        if (endpoint->has_switch_domain)
            printf(" domain=%u switch-port=%u switch=%s", endpoint->switch_domain_id, endpoint->switch_port_id,
                endpoint->switch_name);
        if (endpoint->if_index != 0)
            printf(" ifindex=%u interface=%s phys-port-name=%s device=%s", endpoint->if_index, endpoint->interface_name,
                endpoint->phys_port_name, endpoint->device_name);
        printf("\n");
    }
}
