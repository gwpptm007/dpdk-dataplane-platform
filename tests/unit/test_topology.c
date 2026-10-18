#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/if_link.h>
#include <linux/rtnetlink.h>
#include <rte_dev.h>
#include <rte_ethdev.h>
#include <rte_version.h>
#include "dppd/topology.h"

/** 这些设备和内核观察值全部是模拟数据，不能作为真实硬件角色已经验证的证据 */
static struct rte_eth_dev_info information[DPPD_MAX_PORTS];
static uint32_t flags[DPPD_MAX_PORTS];
static int failed_port = -1, reader_error, reader_mode;
static unsigned int info_calls, reader_calls;
static unsigned int reader_fail_at;
static int kernel_mode, open_kind, kernel_fds;
static unsigned int kernel_receives;
static unsigned int kernel_realpaths;
static size_t read_offset;
static const char *current_device_name = "0000:03:00.0";
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
static struct rte_device fake_device = {.name = "0000:03:00.0"};
#else
static char fake_device;
#endif

static void set_device_name(const char *name)
{
    current_device_name = name;
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    fake_device.name = name;
#endif
}

int __wrap_rte_eth_dev_info_get(uint16_t port_id, struct rte_eth_dev_info *info);
int __wrap_rte_eth_dev_info_get(uint16_t port_id, struct rte_eth_dev_info *info)
{
    assert(port_id < DPPD_MAX_PORTS);
    ++info_calls;
    if (port_id == failed_port)
        return -EIO;
    *info = information[port_id];
    return 0;
}

#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
const char *__wrap_rte_dev_name(const struct rte_device *device);
const char *__wrap_rte_dev_name(const struct rte_device *device)
{
    assert(device == (const struct rte_device *)&fake_device);
    return current_device_name;
}
#endif

static int fake_reader(uint32_t if_index, const char *device_name, struct dppd_topology_identity *identity,
    void *opaque)
{
    assert(opaque == &reader_mode);
    assert(if_index == 13);
    assert(strcmp(device_name, current_device_name) == 0);
    ++reader_calls;
    if (reader_fail_at != 0 && reader_calls == reader_fail_at)
        return -EIO;
    identity->if_index = if_index;
    memcpy(identity->device_name, device_name, strlen(device_name) + 1U);
    memcpy(identity->interface_name, "pf0vf2", sizeof("pf0vf2"));
    memcpy(identity->phys_port_name, "c0pf0vf2", sizeof("c0pf0vf2"));
    switch (reader_mode) {
    case 1:
        identity->if_index = 14;
        break;
    case 2:
        memcpy(identity->device_name, "0000:03:00.1", sizeof("0000:03:00.1"));
        break;
    case 3:
        memcpy(identity->interface_name, "../pf0", sizeof("../pf0"));
        break;
    case 4:
        memset(identity->interface_name, 'x', sizeof(identity->interface_name));
        break;
    case 5:
        memset(identity->phys_port_name, 'x', sizeof(identity->phys_port_name));
        break;
    case 6:
        memset(identity->device_name, 'x', sizeof(identity->device_name));
        break;
    case 7:
        identity->interface_name[0] = '\0';
        break;
    case 8:
        memcpy(identity->phys_port_name, "p0", sizeof("p0"));
        break;
    case 9:
        identity->phys_port_name[0] = '\0';
        break;
    default:
        break;
    }
    return reader_error;
}

static void reset(void)
{
    set_device_name("0000:03:00.0");
    memset(information, 0, sizeof(information));
    memset(flags, 0, sizeof(flags));
    for (unsigned int i = 0; i < DPPD_MAX_PORTS; ++i) {
        information[i].dev_flags = &flags[i];
        information[i].switch_info.domain_id = RTE_ETH_DEV_SWITCH_DOMAIN_ID_INVALID;
        information[i].driver_name = "net_tap";
    }
    failed_port = -1;
    reader_error = reader_mode = 0;
    info_calls = reader_calls = 0;
    reader_fail_at = 0;
    kernel_mode = kernel_fds = 0;
    kernel_receives = kernel_realpaths = 0;
}

static void check_unknown(const struct dppd_endpoint *endpoint, int error)
{
    assert(endpoint->role == DPPD_ENDPOINT_ROLE_UNKNOWN && endpoint->role_error == error);
    assert(endpoint->role_source == DPPD_ENDPOINT_ROLE_SOURCE_UNKNOWN);
    assert(!endpoint->has_controller && !endpoint->has_pf && !endpoint->has_function && !endpoint->has_physical_port);
    assert(endpoint->controller_id == 0 && endpoint->pf_id == 0 && endpoint->function_id == 0);
    assert(endpoint->physical_port_id == 0);
}

/** 接口和物理名称只表示已核对身份，失败的原始字符串不能进入这些可信字段 */
static void check_unverified(const struct dppd_endpoint *endpoint, int error)
{
    check_unknown(endpoint, error);
    assert(endpoint->interface_name[0] == '\0' && endpoint->phys_port_name[0] == '\0');
}

/** 同一结构反复解析，既检查合法编号，也确保失败不会残留上一次的身份 */
static void test_role_parser(void)
{
    struct dppd_endpoint endpoint = {0};
    const char *invalid[] = {"", "0", "-1", "pf", "pf-1vf0", "pf+1vf0", "pf 1vf0", "pf0vf", "pf0sf", "pf0vf-1",
        "pf0sf+1", "pf0vf1tail", "pf0vf1\n", "p", "p-1", "p0tail", "c0", "c-1pf0vf1", " pf0vf1", "pf0vf4294967296",
        "pf4294967296", "c4294967296pf0vf1"};
    char oversized[65];

    assert(dppd_topology_resolve_role(NULL, "net_mlx5", true, "pf0vf0") == -EINVAL);
    assert(dppd_topology_resolve_role(&endpoint, "net_mlx5", true, "c2pf1vf4095") == 0);
    assert(endpoint.role == DPPD_ENDPOINT_ROLE_VF && endpoint.has_controller && endpoint.controller_id == 2);
    assert(endpoint.has_pf && endpoint.pf_id == 1 && endpoint.has_function && endpoint.function_id == 4095);
    assert(!endpoint.has_physical_port && endpoint.role_error == 0);
    assert(endpoint.role_source == DPPD_ENDPOINT_ROLE_SOURCE_MLX5_PHYS_PORT_NAME);
    assert(strcmp(endpoint.phys_port_name, "c2pf1vf4095") == 0);
    assert(dppd_topology_resolve_role(&endpoint, "net_mlx5", true, endpoint.phys_port_name) == 0);
    assert(endpoint.role == DPPD_ENDPOINT_ROLE_VF && endpoint.function_id == 4095);

    assert(dppd_topology_resolve_role(&endpoint, "mlx5_pci", true, "pf0sf0") == 0);
    assert(endpoint.role == DPPD_ENDPOINT_ROLE_SF && !endpoint.has_controller);
    assert(endpoint.has_pf && endpoint.pf_id == 0 && endpoint.has_function && endpoint.function_id == 0);
    assert(dppd_topology_resolve_role(&endpoint, "net_mlx5", true, "c0pf1") == 0);
    assert(endpoint.role == DPPD_ENDPOINT_ROLE_PF && endpoint.has_controller && endpoint.controller_id == 0);
    assert(endpoint.has_pf && endpoint.pf_id == 1 && !endpoint.has_function);

    assert(dppd_topology_resolve_role(&endpoint, "net_mlx5", false, "p1") == 0);
    assert(endpoint.role == DPPD_ENDPOINT_ROLE_UPLINK && endpoint.has_physical_port && endpoint.physical_port_id == 1);
    assert(!endpoint.has_controller && !endpoint.has_pf && !endpoint.has_function);
    assert(dppd_topology_resolve_role(&endpoint, "net_mlx5", false, "c1p0") == 0);
    assert(endpoint.role == DPPD_ENDPOINT_ROLE_UPLINK && endpoint.has_controller && endpoint.controller_id == 1);
    assert(endpoint.has_physical_port && endpoint.physical_port_id == 0);
    assert(dppd_topology_resolve_role(&endpoint, "net_mlx5", true, "pf4294967295vf4294967295") == 0);
    assert(endpoint.pf_id == UINT32_MAX && endpoint.function_id == UINT32_MAX);

    for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        int rc = dppd_topology_resolve_role(&endpoint, "net_mlx5", true, invalid[i]);
        assert(rc == -EINVAL || rc == -ERANGE);
        check_unknown(&endpoint, rc);
    }
    assert(dppd_topology_resolve_role(&endpoint, "net_mlx5", true, "p0") == -EPROTO);
    check_unknown(&endpoint, -EPROTO);
    assert(dppd_topology_resolve_role(&endpoint, "net_mlx5", false, "pf0vf1") == -EPROTO);
    check_unknown(&endpoint, -EPROTO);
    assert(dppd_topology_resolve_role(&endpoint, "net_mlx5", false, "pf0") == -EPROTO);
    check_unknown(&endpoint, -EPROTO);
    assert(dppd_topology_resolve_role(&endpoint, "net_i40e", true, "pf0vf1") == -ENOTSUP);
    check_unknown(&endpoint, -ENOTSUP);
    assert(dppd_topology_resolve_role(&endpoint, "net_mlx5_future", true, "pf0vf1") == -ENOTSUP);
    check_unknown(&endpoint, -ENOTSUP);
    memset(oversized, '1', sizeof(oversized));
    oversized[64] = '\0';
    assert(dppd_topology_resolve_role(&endpoint, "net_mlx5", true, oversized) == -ENAMETOOLONG);
    check_unknown(&endpoint, -ENAMETOOLONG);
    assert(endpoint.phys_port_name[0] == '\0');
    assert(dppd_topology_resolve_role(&endpoint, NULL, true, "pf0vf0") == -EINVAL);
    check_unknown(&endpoint, -EINVAL);
    assert(dppd_topology_resolve_role(&endpoint, "net_mlx5", true, NULL) == -EINVAL);
    check_unknown(&endpoint, -EINVAL);
    assert(strcmp(dppd_endpoint_role_name(DPPD_ENDPOINT_ROLE_PF), "pf") == 0);
    assert(strcmp(dppd_endpoint_role_name(DPPD_ENDPOINT_ROLE_VF), "vf") == 0);
    assert(strcmp(dppd_endpoint_role_name(DPPD_ENDPOINT_ROLE_SF), "sf") == 0);
    assert(strcmp(dppd_endpoint_role_name(DPPD_ENDPOINT_ROLE_UPLINK), "uplink") == 0);
    assert(strcmp(dppd_endpoint_role_name((enum dppd_endpoint_role)99), "unknown") == 0);
    assert(strcmp(dppd_endpoint_role_source_name(DPPD_ENDPOINT_ROLE_SOURCE_MLX5_PHYS_PORT_NAME),
               "mlx5-phys-port-name") == 0);
    assert(strcmp(dppd_endpoint_role_source_name((enum dppd_endpoint_role_source)99), "unknown") == 0);
}

/** 编号碰巧相同、名称被截断或读取失败都不能证明它是该 DPDK 端口的 representor */
static void test_identity_evidence(void)
{
    struct dppd_config config = {.nb_ports = 1, .ports = {3}};
    struct dppd_topology topology;

    reset();
    information[3].device = (struct rte_device *)&fake_device;
    information[3].driver_name = "mlx5_pci";
    information[3].if_index = 13;
    information[3].switch_info.domain_id = 4;
    information[3].switch_info.port_id = 16386;
    information[3].switch_info.name = "test-switch";
    flags[3] = RTE_ETH_DEV_REPRESENTOR;
    assert(dppd_topology_discover_with_reader(&config, &topology, fake_reader, &reader_mode) == 0);
    assert(info_calls == 1 && reader_calls == 1 && topology.nb_endpoints == 1);
    assert(topology.endpoints[0].role == DPPD_ENDPOINT_ROLE_VF && topology.endpoints[0].function_id == 2);
    assert(topology.endpoints[0].has_switch_domain && topology.endpoints[0].switch_domain_id == 4);
    assert(topology.endpoints[0].switch_port_id == 16386 && topology.endpoints[0].if_index == 13);
    assert(strcmp(topology.endpoints[0].interface_name, "pf0vf2") == 0);
    assert(strcmp(topology.endpoints[0].device_name, "0000:03:00.0") == 0);
    assert(strcmp(topology.endpoints[0].phys_port_name, "c0pf0vf2") == 0);

    for (reader_mode = 1; reader_mode <= 7; ++reader_mode) {
        assert(dppd_topology_discover_with_reader(&config, &topology, fake_reader, &reader_mode) == 0);
        check_unverified(&topology.endpoints[0], -EXDEV);
    }
    reader_mode = 8;
    assert(dppd_topology_discover_with_reader(&config, &topology, fake_reader, &reader_mode) == 0);
    check_unknown(&topology.endpoints[0], -EPROTO);
    assert(strcmp(topology.endpoints[0].interface_name, "pf0vf2") == 0);
    reader_mode = 0;
    const int errors[] = {-EIO, -ENOTSUP, -EOVERFLOW, -ENAMETOOLONG, -ENODEV, -EAGAIN, -EPERM, 1};
    for (unsigned int i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        reader_error = errors[i];
        assert(dppd_topology_discover_with_reader(&config, &topology, fake_reader, &reader_mode) == 0);
        check_unverified(&topology.endpoints[0], reader_error < 0 ? reader_error : -EIO);
        assert(topology.endpoints[0].if_index == 13 && strcmp(topology.endpoints[0].device_name, "0000:03:00.0") == 0);
    }
    reader_error = 0;
    information[3].dev_flags = NULL;
    assert(dppd_topology_discover_with_reader(&config, &topology, fake_reader, &reader_mode) == 0);
    check_unknown(&topology.endpoints[0], -EPROTO);
    information[3].driver_name = "net_tap";
    unsigned int previous = reader_calls;
    assert(dppd_topology_discover_with_reader(&config, &topology, fake_reader, &reader_mode) == 0);
    check_unknown(&topology.endpoints[0], -ENOTSUP);
    assert(reader_calls == previous + 1U);
    assert(strcmp(topology.endpoints[0].interface_name, "pf0vf2") == 0);
    previous = reader_calls;
    information[3].driver_name = "mlx5_pci";
    information[3].device = NULL;
    assert(dppd_topology_discover_with_reader(&config, &topology, fake_reader, &reader_mode) == 0);
    check_unknown(&topology.endpoints[0], -ENODATA);
    assert(reader_calls == previous);
    information[3].device = (struct rte_device *)&fake_device;
    assert(dppd_topology_discover_with_reader(&config, &topology, NULL, NULL) == 0);
    check_unknown(&topology.endpoints[0], -ENODATA);
}

/** 配置错误和 PMD 失败必须返回空拓扑，不能让后续逻辑使用半份发现结果 */
static void test_discovery_atomicity(void)
{
    struct dppd_config config = {.nb_ports = 2, .ports = {3, 7}};
    struct dppd_topology topology;
    const struct dppd_topology empty = {0};

    reset();
    assert(dppd_topology_discover(NULL, NULL) == -EINVAL);
    memset(&topology, 0x55, sizeof(topology));
    assert(dppd_topology_discover(NULL, &topology) == -EINVAL);
    assert(memcmp(&topology, &empty, sizeof(topology)) == 0);
    config.nb_ports = DPPD_MAX_PORTS + 1U;
    assert(dppd_topology_discover(&config, &topology) == -EINVAL && info_calls == 0);
    assert(memcmp(&topology, &empty, sizeof(topology)) == 0);
    config.nb_ports = 2;
    config.ports[1] = 3;
    assert(dppd_topology_discover(&config, &topology) == -EEXIST && info_calls == 0);
    assert(memcmp(&topology, &empty, sizeof(topology)) == 0);
    config.ports[1] = 7;
    failed_port = 7;
    assert(dppd_topology_discover(&config, &topology) == -EIO && info_calls == 2);
    assert(memcmp(&topology, &empty, sizeof(topology)) == 0);
    failed_port = -1;
    assert(dppd_topology_discover(&config, &topology) == 0);
    assert(topology.nb_endpoints == 2);
    assert(!topology.endpoints[0].has_switch_domain && !topology.endpoints[1].has_switch_domain);
    check_unknown(&topology.endpoints[0], -ENOTSUP);
    assert(dppd_topology_find(&topology, 3) == &topology.endpoints[0]);
    assert(dppd_topology_find(&topology, 7) == &topology.endpoints[1]);
    assert(dppd_topology_find(&topology, 4) == NULL && dppd_topology_find(NULL, 0) == NULL);
    topology.nb_endpoints = DPPD_MAX_PORTS + 1U;
    assert(dppd_topology_find(&topology, 3) == NULL);
    dppd_topology_dump(&topology);
    dppd_topology_dump(NULL);
    config.nb_ports = 0;
    assert(dppd_topology_discover(&config, &topology) == 0 && topology.nb_endpoints == 0);
    config.nb_ports = DPPD_MAX_PORTS;
    for (unsigned int i = 0; i < DPPD_MAX_PORTS; ++i)
        config.ports[i] = (uint16_t)i;
    assert(dppd_topology_discover(&config, &topology) == 0 && topology.nb_endpoints == DPPD_MAX_PORTS);
}

/** 下列包装只在本测试中模拟只读内核接口，不创建接口、切换网络或修改真实 sysfs */
int __wrap_socket(int domain, int type, int protocol);
int __wrap_setsockopt(int fd, int level, int option, const void *value, socklen_t length);
ssize_t __wrap_sendto(int fd, const void *buffer, size_t length, int flags, const struct sockaddr *address,
    socklen_t address_length);
ssize_t __wrap_recvmsg(int fd, struct msghdr *message, int flags);
int __wrap_close(int fd);
int __wrap_open(const char *path, int flags, ...);
int __wrap_open64(const char *path, int flags, ...);
ssize_t __wrap_read(int fd, void *buffer, size_t count);
char *__wrap_realpath(const char *path, char *resolved);
char *__wrap___realpath_chk(const char *path, char *resolved, size_t capacity);
unsigned int __wrap_if_nametoindex(const char *name);

int __wrap_socket(int domain, int type, int protocol)
{
    assert(domain == AF_NETLINK && type == (SOCK_RAW | SOCK_CLOEXEC) && protocol == NETLINK_ROUTE);
    if (kernel_mode == 19) {
        errno = EACCES;
        return -1;
    }
    ++kernel_fds;
    return 30001;
}

int __wrap_setsockopt(int fd, int level, int option, const void *value, socklen_t length)
{
    assert(fd == 30001 && level == SOL_SOCKET && option == SO_RCVTIMEO && value != NULL && length != 0);
    if (kernel_mode == 20) {
        errno = ENOPROTOOPT;
        return -1;
    }
    return 0;
}

ssize_t __wrap_sendto(int fd, const void *buffer, size_t length, int flags, const struct sockaddr *address,
    socklen_t address_length)
{
    const struct nlmsghdr *header = buffer;

    assert(fd == 30001 && flags == 0 && address != NULL && address_length >= sizeof(struct sockaddr_nl));
    assert(header->nlmsg_type == RTM_GETLINK && header->nlmsg_seq == 1);
    if (kernel_mode == 21) {
        errno = ENOBUFS;
        return -1;
    }
    return (ssize_t)length;
}

static void append_attribute(struct nlmsghdr *header, unsigned int type, const char *value, size_t length)
{
    struct rtattr *attribute = (struct rtattr *)((char *)header + NLMSG_ALIGN(header->nlmsg_len));

    attribute->rta_type = (uint16_t)type;
    attribute->rta_len = (uint16_t)RTA_LENGTH(length);
    memcpy(RTA_DATA(attribute), value, length);
    header->nlmsg_len = NLMSG_ALIGN(header->nlmsg_len) + RTA_ALIGN(attribute->rta_len);
}

ssize_t __wrap_recvmsg(int fd, struct msghdr *message, int flags)
{
    struct nlmsghdr *header = message->msg_iov[0].iov_base;
    struct sockaddr_nl *sender = message->msg_name;
    struct ifinfomsg *link;
    char oversized[65];

    assert(fd == 30001 && flags == 0 && message->msg_iov[0].iov_len >= 1024);
    ++kernel_receives;
    if (kernel_mode == 22) {
        errno = EINTR;
        return -1;
    }
    memset(header, 0, 1024);
    sender->nl_pid = kernel_mode == 2 ? 9 : 0;
    sender->nl_groups = 0;
    sender->nl_family = kernel_mode == 38 ? AF_UNIX : AF_NETLINK;
    if (kernel_mode == 39)
        message->msg_namelen = 1;
    message->msg_flags = kernel_mode == 1 ? MSG_TRUNC : 0;
    header->nlmsg_seq = kernel_mode == 7 ? 2 : 1;
    header->nlmsg_type = RTM_NEWLINK;
    header->nlmsg_len = NLMSG_LENGTH(sizeof(*link));
    link = NLMSG_DATA(header);
    link->ifi_index = kernel_mode == 6 ? 14 : 13;
    if (kernel_mode == 3) {
        header->nlmsg_type = NLMSG_ERROR;
        header->nlmsg_len = NLMSG_LENGTH(sizeof(struct nlmsgerr));
        ((struct nlmsgerr *)NLMSG_DATA(header))->error = -EIO;
        return header->nlmsg_len;
    }
    if (kernel_mode != 8) {
        const char *name = kernel_mode == 35 && kernel_receives >= 2 ? "other0" : "pf0vf2";
        append_attribute(header, IFLA_IFNAME, name, strlen(name) + 1U);
        if (kernel_mode == 36)
            append_attribute(header, IFLA_IFNAME, "pf0vf2", sizeof("pf0vf2"));
    }
    bool missing_phys = (kernel_mode >= 25 && kernel_mode <= 28) || (kernel_mode >= 30 && kernel_mode <= 33) ||
        kernel_mode == 42 || kernel_mode == 43;
    if (kernel_mode == 33 && kernel_receives >= 2)
        missing_phys = false;
    if (kernel_mode == 34 && kernel_receives >= 2)
        missing_phys = true;
    if (kernel_mode == 4 || kernel_mode == 5) {
        memset(oversized, 'x', sizeof(oversized));
        if (kernel_mode == 4)
            oversized[64] = '\0';
        append_attribute(header, IFLA_PHYS_PORT_NAME, oversized, kernel_mode == 4 ? 65 : 8);
    } else if (kernel_mode == 10 && kernel_receives >= 2)
        append_attribute(header, IFLA_PHYS_PORT_NAME, "pf0vf3", sizeof("pf0vf3"));
    else if (kernel_mode == 41)
        append_attribute(header, IFLA_PHYS_PORT_NAME, "", 1);
    else if (kernel_mode == 45) {
        const char malformed[] = {'c', '0', '\0', 'v', 'f', '2', '\0'};
        append_attribute(header, IFLA_PHYS_PORT_NAME, malformed, sizeof(malformed));
    } else if (!missing_phys)
        append_attribute(header, IFLA_PHYS_PORT_NAME, "c0pf0vf2", sizeof("c0pf0vf2"));
    if (kernel_mode == 37)
        append_attribute(header, IFLA_PHYS_PORT_NAME, "c0pf0vf2", sizeof("c0pf0vf2"));
    if (kernel_mode == 9)
        ++header->nlmsg_len;
    return header->nlmsg_len;
}

int __wrap_close(int fd)
{
    assert(fd == 30001 || fd == 30002);
    assert(kernel_fds > 0);
    --kernel_fds;
    return 0;
}

static int fake_open(const char *path, int flags)
{
    assert((flags & (O_RDONLY | O_CLOEXEC | O_NOFOLLOW)) == (O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (kernel_mode == 15) {
        errno = EPERM;
        return -1;
    }
    if (strcmp(path, "/sys/class/net/pf0vf2/ifindex") == 0)
        open_kind = 1;
    else {
        assert(strcmp(path, "/sys/class/net/pf0vf2/phys_port_name") == 0);
        if (kernel_mode == 25 || kernel_mode == 29 || kernel_mode == 33 || kernel_mode == 42) {
            errno = ENOENT;
            return -1;
        }
        if (kernel_mode == 30) {
            errno = EACCES;
            return -1;
        }
        open_kind = 2;
    }
    read_offset = 0;
    ++kernel_fds;
    return 30002;
}

int __wrap_open(const char *path, int flags, ...)
{
    return fake_open(path, flags);
}

int __wrap_open64(const char *path, int flags, ...)
{
    return fake_open(path, flags);
}

ssize_t __wrap_read(int fd, void *buffer, size_t count)
{
    const char *value = open_kind == 1 ? "13\n" : "c0pf0vf2\n";
    const char malformed[] = {'a', '\0', 'b', '\n'};
    char oversized[65];
    size_t available;

    assert(fd == 30002);
    if (kernel_mode == 16) {
        errno = EIO;
        return -1;
    }
    if (open_kind == 2 && (kernel_mode == 26 || (kernel_mode == 43 && read_offset != 0))) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (open_kind == 1 && kernel_mode == 11)
        value = "14\n";
    if (open_kind == 1 && kernel_mode == 23)
        value = "13junk\n";
    if (open_kind == 2 && kernel_mode == 12)
        value = "pf0vf3\n";
    if (open_kind == 2 && (kernel_mode == 27 || kernel_mode == 41))
        value = "\n";
    if (open_kind == 2 && (kernel_mode == 14 || kernel_mode == 31)) {
        memset(oversized, 'x', sizeof(oversized));
        oversized[64] = '\0';
        value = oversized;
    }
    if (open_kind == 2 && kernel_mode == 32)
        value = malformed;
    available = open_kind == 2 && kernel_mode == 32 ? sizeof(malformed) : strlen(value);
    if (read_offset >= available)
        return 0;
    available -= read_offset;
    if (available > count)
        available = count;
    /** 故意让每次读取只返回少量字节，验证短读不会被误当成完整名称 */
    if (kernel_mode == 24 && available > 2)
        available = 2;
    if (open_kind == 2 && kernel_mode == 43 && available > 2)
        available = 2;
    memcpy(buffer, value + read_offset, available);
    read_offset += available;
    return (ssize_t)available;
}

char *__wrap_realpath(const char *path, char *resolved)
{
    const char *target = kernel_mode == 13 || kernel_mode == 42 ? "/sys/devices/pci0000:00/0000:03:00.1"
                                                                : "/sys/devices/pci0000:00/0000:03:00.0";

    assert(strcmp(path, "/sys/class/net/pf0vf2/device") == 0);
    ++kernel_realpaths;
    if (kernel_mode == 17) {
        errno = ENOENT;
        return NULL;
    }
    memcpy(resolved, target, strlen(target) + 1U);
    return resolved;
}

/** 优化构建会调用带长度检查的 libc 入口，必须继续使用同一份模拟内核身份 */
char *__wrap___realpath_chk(const char *path, char *resolved, size_t capacity)
{
    if (capacity < sizeof("/sys/devices/pci0000:00/0000:03:00.0")) {
        errno = ERANGE;
        return NULL;
    }
    return __wrap_realpath(path, resolved);
}

unsigned int __wrap_if_nametoindex(const char *name)
{
    assert(strcmp(name, "pf0vf2") == 0);
    return kernel_mode == 18 ? 14 : 13;
}

/** 错命名空间、接口复用、超长内核属性和半截 sysfs 名称都应保留未知身份 */
static void test_default_reader(void)
{
    struct dppd_config config = {.nb_ports = 1, .ports = {3}};
    struct dppd_topology topology;
    const int expected[] = {0, -EOVERFLOW, -EOVERFLOW, -EIO, -EOVERFLOW, -EOVERFLOW, -ENODATA, -ENODATA, -ENODATA,
        -EPROTO, -EAGAIN, -EXDEV, -EXDEV, -EXDEV, -EOVERFLOW, -EPERM, -EIO, -ENOENT, -EAGAIN, -EACCES, -ENOPROTOOPT,
        -ENOBUFS, -EINTR, -EXDEV, 0};

    reset();
    information[3].device = (struct rte_device *)&fake_device;
    information[3].driver_name = "mlx5_pci";
    information[3].if_index = 13;
    flags[3] = RTE_ETH_DEV_REPRESENTOR;
    for (kernel_mode = 0; kernel_mode < (int)(sizeof(expected) / sizeof(expected[0])); ++kernel_mode) {
        kernel_fds = 0;
        kernel_receives = 0;
        assert(dppd_topology_discover(&config, &topology) == 0);
        assert(topology.nb_endpoints == 1 && kernel_fds == 0);
        if (expected[kernel_mode] != 0)
            check_unverified(&topology.endpoints[0], expected[kernel_mode]);
        else {
            assert(topology.endpoints[0].role == DPPD_ENDPOINT_ROLE_VF && topology.endpoints[0].function_id == 2);
            assert(kernel_receives == 2 && topology.endpoints[0].role_error == 0);
        }
    }
}

/** 通用 PCI PMD 复用相同接口核对，缺少角色名称不等于可以跳过 PCI 或命名空间证据 */
static void test_generic_identity(void)
{
    struct dppd_config config = {.nb_ports = 1, .ports = {3}};
    struct dppd_topology topology;
    static const struct {
        int mode, error;
        bool verified, phys_present;
    } cases[] = {{0, -ENOTSUP, true, true}, {11, -EXDEV, false, false}, {13, -EXDEV, false, false},
        {17, -ENOENT, false, false}, {25, -ENOTSUP, true, false}, {26, -ENOTSUP, true, false},
        {27, -ENOTSUP, true, false}, {28, -EXDEV, false, false}, {29, -EXDEV, false, false},
        {30, -EACCES, false, false}, {31, -EOVERFLOW, false, false}, {32, -EPROTO, false, false},
        {33, -EAGAIN, false, false}, {34, -EAGAIN, false, false}, {35, -EAGAIN, false, false},
        {36, -EPROTO, false, false}, {37, -EPROTO, false, false}, {38, -EPROTO, false, false},
        {39, -EPROTO, false, false}, {41, -ENOTSUP, true, false}, {42, -EXDEV, false, false}, {43, -EIO, false, false},
        {45, -EPROTO, false, false}};

    reset();
    information[3].device = (struct rte_device *)&fake_device;
    information[3].driver_name = "net_i40e";
    information[3].if_index = 13;
    flags[3] = RTE_ETH_DEV_REPRESENTOR;
    for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        kernel_mode = cases[i].mode;
        kernel_fds = 0;
        kernel_receives = kernel_realpaths = 0;
        assert(dppd_topology_discover(&config, &topology) == 0);
        const struct dppd_endpoint *endpoint = &topology.endpoints[0];
        assert(topology.nb_endpoints == 1 && endpoint->if_index == 13 && kernel_fds == 0);
        assert(endpoint->kind == DPPD_ENDPOINT_REPRESENTOR);
        assert(strcmp(endpoint->device_name, "0000:03:00.0") == 0);
        if (cases[i].verified) {
            check_unknown(endpoint, -ENOTSUP);
            assert(strcmp(endpoint->interface_name, "pf0vf2") == 0 && kernel_receives == 2 && kernel_realpaths == 1);
            assert(strcmp(endpoint->phys_port_name, cases[i].phys_present ? "c0pf0vf2" : "") == 0);
        } else
            check_unverified(endpoint, cases[i].error);
        if (cases[i].mode == 42)
            assert(kernel_realpaths == 1 && kernel_receives == 1);
    }

    /** mlx5 也先形成接口身份，双方没有物理端口名时不能凭 representor 标志推测角色 */
    information[3].driver_name = "mlx5_pci";
    const int missing_phys_modes[] = {25, 26, 27, 41};
    for (unsigned int i = 0; i < sizeof(missing_phys_modes) / sizeof(missing_phys_modes[0]); ++i) {
        kernel_mode = missing_phys_modes[i];
        kernel_receives = kernel_realpaths = 0;
        assert(dppd_topology_discover(&config, &topology) == 0);
        check_unknown(&topology.endpoints[0], -ENODATA);
        assert(strcmp(topology.endpoints[0].interface_name, "pf0vf2") == 0 &&
            topology.endpoints[0].phys_port_name[0] == '\0');
        assert(kernel_receives == 2 && kernel_realpaths == 1 && kernel_fds == 0);
    }

    /** 通用角色未知仍可稳定重发现，换 PCI 设备或读取失败都不能覆盖原来的可信拓扑 */
    information[3].driver_name = "net_ice";
    kernel_mode = 25;
    kernel_receives = 0;
    assert(dppd_topology_discover(&config, &topology) == 0);
    struct dppd_topology expected = topology, output, sentinel;
    assert(dppd_topology_rediscover_verified(&config, &expected, &output) == 0);
    memset(&sentinel, 0xa5, sizeof(sentinel));
    memcpy(&output, &sentinel, sizeof(output));
    kernel_mode = 42;
    assert(dppd_topology_rediscover_verified(&config, &expected, &output) == -EXDEV);
    assert(memcmp(&output, &sentinel, sizeof(output)) == 0);
    kernel_mode = 30;
    assert(dppd_topology_rediscover_verified(&config, &expected, &output) == -EXDEV);
    assert(memcmp(&output, &sentinel, sizeof(output)) == 0 && kernel_fds == 0);

    /** 没有宿主接口、超范围编号和 auxiliary/vdev 名称都不能补成可信 PCI 接口 */
    kernel_mode = 0;
    information[3].if_index = 0;
    kernel_receives = 0;
    assert(dppd_topology_discover(&config, &topology) == 0);
    check_unverified(&topology.endpoints[0], -ENODEV);
    assert(kernel_receives == 0 && kernel_fds == 0);
    information[3].if_index = UINT32_MAX;
    assert(dppd_topology_discover(&config, &topology) == 0);
    check_unverified(&topology.endpoints[0], -ENODEV);
    assert(kernel_receives == 0 && topology.endpoints[0].if_index == UINT32_MAX);
    information[3].if_index = 13;
    set_device_name("mlx5_core.sf.1");
    assert(dppd_topology_discover(&config, &topology) == 0);
    check_unverified(&topology.endpoints[0], -ENOTSUP);
    assert(kernel_receives == 0);
    set_device_name("0000:03:00.0");
    assert(dppd_topology_discover_with_reader(&config, &topology, NULL, NULL) == 0);
    check_unverified(&topology.endpoints[0], -ENODATA);
    for (reader_mode = 1; reader_mode <= 7; ++reader_mode) {
        assert(dppd_topology_discover_with_reader(&config, &topology, fake_reader, &reader_mode) == 0);
        check_unverified(&topology.endpoints[0], -EXDEV);
    }
    reader_mode = 0;
    reader_error = -EIO;
    assert(dppd_topology_discover_with_reader(&config, &topology, fake_reader, &reader_mode) == 0);
    check_unverified(&topology.endpoints[0], -EIO);
    reader_error = 0;
    reader_mode = 9;
    assert(dppd_topology_discover_with_reader(&config, &topology, fake_reader, &reader_mode) == 0);
    check_unknown(&topology.endpoints[0], -ENOTSUP);
    assert(
        strcmp(topology.endpoints[0].interface_name, "pf0vf2") == 0 && topology.endpoints[0].phys_port_name[0] == '\0');
}

/** 每次重新建立带完整身份的双端口夹具，不把已知字符串解析当作真实设备证明 */
static void make_verified_fixture(struct dppd_config *config, struct dppd_topology *topology)
{
    reset();
    *config = (struct dppd_config){.nb_ports = 2, .ports = {3, 7}};
    for (unsigned int i = 0; i < config->nb_ports; ++i) {
        uint16_t port = config->ports[i];
        information[port].device = (struct rte_device *)&fake_device;
        information[port].driver_name = "mlx5_pci";
        information[port].if_index = 13;
        information[port].switch_info.domain_id = 4;
        information[port].switch_info.port_id = 100 + port;
        information[port].switch_info.name = "test-switch";
        flags[port] = RTE_ETH_DEV_REPRESENTOR;
    }
    assert(dppd_topology_discover_with_reader(config, topology, fake_reader, &reader_mode) == 0);
}

static void test_identity_comparison(void)
{
    struct dppd_config config;
    struct dppd_topology expected, candidate;
    make_verified_fixture(&config, &expected);
    candidate = expected;
    assert(dppd_topology_validate_identity(&expected, &candidate, config.ports, config.nb_ports) == 0);
    candidate.endpoints[0] = expected.endpoints[1];
    candidate.endpoints[1] = expected.endpoints[0];
    assert(dppd_topology_validate_identity(&expected, &candidate, config.ports, config.nb_ports) == 0);

#define CHECK_CHANGED(member, value)                                                                             \
    do {                                                                                                         \
        candidate = expected;                                                                                    \
        candidate.endpoints[0].member = (value);                                                                 \
        assert(dppd_topology_validate_identity(&expected, &candidate, config.ports, config.nb_ports) == -EXDEV); \
    } while (0)
    CHECK_CHANGED(kind, DPPD_ENDPOINT_ETHDEV);
    CHECK_CHANGED(has_switch_domain, false);
    CHECK_CHANGED(switch_domain_id, 5);
    CHECK_CHANGED(switch_port_id, 104);
    CHECK_CHANGED(if_index, 14);
    CHECK_CHANGED(role, DPPD_ENDPOINT_ROLE_SF);
    CHECK_CHANGED(role_source, DPPD_ENDPOINT_ROLE_SOURCE_UNKNOWN);
    CHECK_CHANGED(role_error, -EIO);
    CHECK_CHANGED(has_controller, false);
    CHECK_CHANGED(controller_id, 1);
    CHECK_CHANGED(has_pf, false);
    CHECK_CHANGED(pf_id, 1);
    CHECK_CHANGED(has_function, false);
    CHECK_CHANGED(function_id, 3);
    CHECK_CHANGED(has_physical_port, true);
    expected.endpoints[0].has_physical_port = true;
    CHECK_CHANGED(physical_port_id, 1);
    expected.endpoints[0].has_physical_port = false;
#undef CHECK_CHANGED

    struct text_member {
        size_t offset, size;
    };
    static const struct text_member texts[] = {
        {offsetof(struct dppd_endpoint, switch_name), sizeof(candidate.endpoints[0].switch_name)},
        {offsetof(struct dppd_endpoint, driver_name), sizeof(candidate.endpoints[0].driver_name)},
        {offsetof(struct dppd_endpoint, interface_name), sizeof(candidate.endpoints[0].interface_name)},
        {offsetof(struct dppd_endpoint, device_name), sizeof(candidate.endpoints[0].device_name)},
        {offsetof(struct dppd_endpoint, phys_port_name), sizeof(candidate.endpoints[0].phys_port_name)}};
    for (unsigned int i = 0; i < sizeof(texts) / sizeof(texts[0]); ++i) {
        candidate = expected;
        char *value = (char *)&candidate.endpoints[0] + texts[i].offset;
        value[0] = 'x';
        assert(dppd_topology_validate_identity(&expected, &candidate, config.ports, config.nb_ports) == -EXDEV);
        memset(value, 'x', texts[i].size);
        assert(dppd_topology_validate_identity(&expected, &candidate, config.ports, config.nb_ports) == -EINVAL);
        assert(dppd_topology_validate_identity(&candidate, &expected, config.ports, config.nb_ports) == -EINVAL);
    }
    candidate = expected;
    strcpy(candidate.endpoints[0].device_name, "0000:03:00.1");
    assert(dppd_topology_validate_identity(&expected, &candidate, config.ports, config.nb_ports) == -EXDEV);
    candidate = expected;
    candidate.endpoints[0].device_name[0] = '\0';
    assert(dppd_topology_validate_identity(&candidate, &candidate, config.ports, config.nb_ports) == -ENODATA);
    candidate = expected;
    candidate.endpoints[0].driver_name[0] = '\0';
    assert(dppd_topology_validate_identity(&candidate, &candidate, config.ports, config.nb_ports) == -ENODATA);
    strcpy(candidate.endpoints[0].driver_name, "unknown");
    assert(dppd_topology_validate_identity(&candidate, &candidate, config.ports, config.nb_ports) == -ENODATA);

    candidate = expected;
    candidate.endpoints[0].kind = (enum dppd_endpoint_kind)99;
    assert(dppd_topology_validate_identity(&expected, &candidate, config.ports, config.nb_ports) == -EINVAL);
    candidate = expected;
    candidate.endpoints[0].role = (enum dppd_endpoint_role)99;
    assert(dppd_topology_validate_identity(&expected, &candidate, config.ports, config.nb_ports) == -EINVAL);
    candidate = expected;
    candidate.endpoints[0].role_source = (enum dppd_endpoint_role_source)99;
    assert(dppd_topology_validate_identity(&expected, &candidate, config.ports, config.nb_ports) == -EINVAL);
    candidate = expected;
    candidate.endpoints[0].switch_domain_id = UINT16_MAX;
    assert(dppd_topology_validate_identity(&expected, &candidate, config.ports, config.nb_ports) == -EINVAL);

    /** 未生效的编号与填充字节不携带身份证据，相同未知状态不能被补成已知角色 */
    expected.endpoints[0].has_controller = expected.endpoints[0].has_pf = expected.endpoints[0].has_function = false;
    expected.endpoints[0].has_switch_domain = false;
    expected.endpoints[0].role = DPPD_ENDPOINT_ROLE_UNKNOWN;
    expected.endpoints[0].role_source = DPPD_ENDPOINT_ROLE_SOURCE_UNKNOWN;
    expected.endpoints[0].role_error = -ENOTSUP;
    candidate = expected;
    candidate.endpoints[0].controller_id = candidate.endpoints[0].pf_id = candidate.endpoints[0].function_id = 42;
    candidate.endpoints[0].physical_port_id = 42;
    candidate.endpoints[0].switch_domain_id = candidate.endpoints[0].switch_port_id = 42;
    candidate.endpoints[0].driver_name[sizeof(candidate.endpoints[0].driver_name) - 1U] = 'y';
    for (size_t i = sizeof(expected.endpoints[0].ethdev_port_id); i < offsetof(struct dppd_endpoint, kind); ++i)
        ((unsigned char *)&candidate.endpoints[0])[i] = 0xa5;
    assert(dppd_topology_validate_identity(&expected, &candidate, config.ports, config.nb_ports) == 0);
    candidate.endpoints[0].has_pf = true;
    assert(dppd_topology_validate_identity(&expected, &candidate, config.ports, config.nb_ports) == -EXDEV);

    /** 普通虚拟设备可以没有 Linux 接口和角色，但仍必须保留相同的设备名与驱动 */
    expected.endpoints[0].if_index = 0;
    expected.endpoints[0].interface_name[0] = '\0';
    expected.endpoints[0].phys_port_name[0] = '\0';
    strcpy(expected.endpoints[0].device_name, "test-vdev0");
    strcpy(expected.endpoints[0].driver_name, "net_null");
    candidate = expected;
    assert(dppd_topology_validate_identity(&expected, &candidate, config.ports, config.nb_ports) == 0);

    make_verified_fixture(&config, &expected);
    candidate = expected;
    candidate.endpoints[1] = candidate.endpoints[0];
    assert(dppd_topology_validate_identity(&expected, &candidate, config.ports, config.nb_ports) == -EUCLEAN);
    assert(dppd_topology_validate_identity(&candidate, &expected, config.ports, config.nb_ports) == -EUCLEAN);
    candidate = expected;
    candidate.nb_endpoints = 1;
    assert(dppd_topology_validate_identity(&expected, &candidate, config.ports, config.nb_ports) == -ENOENT);
    candidate = expected;
    candidate.endpoints[2] = expected.endpoints[0];
    candidate.endpoints[2].ethdev_port_id = 9;
    candidate.nb_endpoints = 3;
    assert(dppd_topology_validate_identity(&expected, &candidate, config.ports, config.nb_ports) == -EXDEV);
    candidate.nb_endpoints = DPPD_MAX_PORTS + 1U;
    assert(dppd_topology_validate_identity(&expected, &candidate, config.ports, config.nb_ports) == -EINVAL);
    uint16_t duplicated[] = {3, 3}, invalid[] = {3, UINT16_MAX};
    assert(dppd_topology_validate_identity(&expected, &expected, duplicated, 2) == -EEXIST);
    assert(dppd_topology_validate_identity(&expected, &expected, invalid, 2) == -EINVAL);
    assert(dppd_topology_validate_identity(NULL, &expected, config.ports, 2) == -EINVAL);
    assert(dppd_topology_validate_identity(&expected, NULL, config.ports, 2) == -EINVAL);
    assert(dppd_topology_validate_identity(&expected, &expected, NULL, 2) == -EINVAL);
    assert(dppd_topology_validate_identity(&expected, &expected, config.ports, 0) == -EINVAL);
    assert(dppd_topology_validate_identity(&expected, &expected, config.ports, DPPD_MAX_PORTS + 1U) == -EINVAL);
}

/** 重发现只在完整成功后写出，第二端口失败也不能覆盖旧拓扑或调用方的哨兵字节 */
static void test_verified_rediscovery(void)
{
    struct dppd_config config;
    struct dppd_topology expected, output, sentinel;
    make_verified_fixture(&config, &expected);
    memset(&sentinel, 0xa5, sizeof(sentinel));
    memcpy(&output, &sentinel, sizeof(output));
    failed_port = 7;
    info_calls = reader_calls = 0;
    assert(
        dppd_topology_rediscover_verified_with_reader(&config, &expected, &output, fake_reader, &reader_mode) == -EIO);
    assert(memcmp(&output, &sentinel, sizeof(output)) == 0 && info_calls == 2 && reader_calls == 1);
    struct dppd_topology original;
    memcpy(&original, &expected, sizeof(original));
    assert(dppd_topology_rediscover_verified_with_reader(&config, &expected, &expected, fake_reader, &reader_mode) ==
        -EIO);
    assert(memcmp(&expected, &original, sizeof(expected)) == 0);
    failed_port = -1;
    reader_calls = 0;
    reader_fail_at = 2;
    assert(dppd_topology_rediscover_verified_with_reader(&config, &expected, &output, fake_reader, &reader_mode) ==
        -EXDEV);
    assert(memcmp(&output, &sentinel, sizeof(output)) == 0 && reader_calls == 2);
    reader_fail_at = 0;
    information[7].switch_info.domain_id = 5;
    assert(dppd_topology_rediscover_verified_with_reader(&config, &expected, &output, fake_reader, &reader_mode) ==
        -EXDEV);
    assert(memcmp(&output, &sentinel, sizeof(output)) == 0);
    information[7].switch_info.domain_id = 4;
    set_device_name("0000:03:00.1");
    assert(dppd_topology_rediscover_verified_with_reader(&config, &expected, &output, fake_reader, &reader_mode) ==
        -EXDEV);
    assert(memcmp(&output, &sentinel, sizeof(output)) == 0);
    set_device_name("0000:03:00.0");
    char oversized[65];
    memset(oversized, 'x', sizeof(oversized));
    oversized[64] = '\0';
    set_device_name(oversized);
    assert(dppd_topology_rediscover_verified_with_reader(&config, &expected, &output, fake_reader, &reader_mode) ==
        -ENAMETOOLONG);
    assert(memcmp(&output, &sentinel, sizeof(output)) == 0);
    set_device_name("0000:03:00.0");
    information[7].driver_name = oversized;
    assert(dppd_topology_rediscover_verified_with_reader(&config, &expected, &output, fake_reader, &reader_mode) ==
        -ENAMETOOLONG);
    assert(memcmp(&output, &sentinel, sizeof(output)) == 0);
    information[7].driver_name = "mlx5_pci";
    information[7].switch_info.name = oversized;
    assert(dppd_topology_rediscover_verified_with_reader(&config, &expected, &output, fake_reader, &reader_mode) ==
        -ENAMETOOLONG);
    assert(memcmp(&output, &sentinel, sizeof(output)) == 0);
    information[7].switch_info.name = "test-switch";
    config.ports[0] = 7;
    config.ports[1] = 3;
    assert(dppd_topology_rediscover_verified_with_reader(&config, &expected, &output, fake_reader, &reader_mode) == 0);
    assert(output.endpoints[0].ethdev_port_id == 7 && output.endpoints[1].ethdev_port_id == 3);
    assert(dppd_topology_validate_identity(&expected, &output, config.ports, config.nb_ports) == 0);
    assert(
        dppd_topology_rediscover_verified_with_reader(&config, &expected, &expected, fake_reader, &reader_mode) == 0);
    assert(expected.endpoints[0].ethdev_port_id == 7);

    /** 基线已经缺少身份或端口集合错误时，连第一次 PMD 读取也不应开始 */
    memcpy(&output, &sentinel, sizeof(output));
    expected.endpoints[0].device_name[0] = '\0';
    info_calls = 0;
    assert(dppd_topology_rediscover_verified_with_reader(&config, &expected, &output, fake_reader, &reader_mode) ==
        -ENODATA);
    assert(info_calls == 0 && memcmp(&output, &sentinel, sizeof(output)) == 0);
    assert(
        dppd_topology_rediscover_verified_with_reader(NULL, &expected, &output, fake_reader, &reader_mode) == -EINVAL);
    assert(dppd_topology_rediscover_verified_with_reader(&config, NULL, &output, fake_reader, &reader_mode) == -EINVAL);
    assert(
        dppd_topology_rediscover_verified_with_reader(&config, &expected, NULL, fake_reader, &reader_mode) == -EINVAL);

    /** 默认内核读取变体同样只读，接口观察失败不会发布未知身份来替换原来的已知身份 */
    make_verified_fixture(&config, &expected);
    config.nb_ports = expected.nb_endpoints = 1;
    kernel_mode = 0;
    kernel_fds = 0;
    kernel_receives = 0;
    assert(dppd_topology_rediscover_verified(&config, &expected, &output) == 0 && kernel_fds == 0);
    kernel_mode = 17;
    memcpy(&output, &sentinel, sizeof(output));
    assert(dppd_topology_rediscover_verified(&config, &expected, &output) == -EXDEV);
    assert(kernel_fds == 0 && memcmp(&output, &sentinel, sizeof(output)) == 0);
}

int main(void)
{
    test_role_parser();
    test_identity_evidence();
    test_discovery_atomicity();
    test_default_reader();
    test_generic_identity();
    test_identity_comparison();
    test_verified_rediscovery();
    return 0;
}
