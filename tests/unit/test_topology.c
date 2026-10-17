#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
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
static int kernel_mode, open_kind, kernel_fds;
static unsigned int kernel_receives;
static size_t read_offset;
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
static struct rte_device fake_device = {.name = "0000:03:00.0"};
#else
static char fake_device;
#endif

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
    return "0000:03:00.0";
}
#endif

static int fake_reader(uint32_t if_index, const char *device_name, struct dppd_topology_identity *identity,
    void *opaque)
{
    assert(opaque == &reader_mode);
    assert(if_index == 13);
    assert(strcmp(device_name, "0000:03:00.0") == 0);
    ++reader_calls;
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
    default:
        break;
    }
    return reader_error;
}

static void reset(void)
{
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
}

static void check_unknown(const struct dppd_endpoint *endpoint, int error)
{
    assert(endpoint->role == DPPD_ENDPOINT_ROLE_UNKNOWN && endpoint->role_error == error);
    assert(endpoint->role_source == DPPD_ENDPOINT_ROLE_SOURCE_UNKNOWN);
    assert(!endpoint->has_controller && !endpoint->has_pf && !endpoint->has_function && !endpoint->has_physical_port);
    assert(endpoint->controller_id == 0 && endpoint->pf_id == 0 && endpoint->function_id == 0);
    assert(endpoint->physical_port_id == 0);
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
        check_unknown(&topology.endpoints[0], -EXDEV);
    }
    reader_mode = 8;
    assert(dppd_topology_discover_with_reader(&config, &topology, fake_reader, &reader_mode) == 0);
    check_unknown(&topology.endpoints[0], -EPROTO);
    reader_mode = 0;
    const int errors[] = {-EIO, -ENOTSUP, -EOVERFLOW, -ENAMETOOLONG, -ENODEV, -EAGAIN, -EPERM, 1};
    for (unsigned int i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        reader_error = errors[i];
        assert(dppd_topology_discover_with_reader(&config, &topology, fake_reader, &reader_mode) == 0);
        check_unknown(&topology.endpoints[0], reader_error < 0 ? reader_error : -EIO);
        assert(strcmp(topology.endpoints[0].phys_port_name, "c0pf0vf2") == 0);
    }
    reader_error = 0;
    information[3].dev_flags = NULL;
    assert(dppd_topology_discover_with_reader(&config, &topology, fake_reader, &reader_mode) == 0);
    check_unknown(&topology.endpoints[0], -EPROTO);
    information[3].driver_name = "net_tap";
    unsigned int previous = reader_calls;
    assert(dppd_topology_discover_with_reader(&config, &topology, fake_reader, &reader_mode) == 0);
    check_unknown(&topology.endpoints[0], -ENOTSUP);
    assert(reader_calls == previous);
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
    if (kernel_mode != 8)
        append_attribute(header, IFLA_IFNAME, "pf0vf2", sizeof("pf0vf2"));
    if (kernel_mode == 4 || kernel_mode == 5) {
        memset(oversized, 'x', sizeof(oversized));
        if (kernel_mode == 4)
            oversized[64] = '\0';
        append_attribute(header, IFLA_PHYS_PORT_NAME, oversized, kernel_mode == 4 ? 65 : 8);
    } else if (kernel_mode == 10 && kernel_receives >= 2)
        append_attribute(header, IFLA_PHYS_PORT_NAME, "pf0vf3", sizeof("pf0vf3"));
    else
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
    char oversized[65];
    size_t available;

    assert(fd == 30002);
    if (kernel_mode == 16) {
        errno = EIO;
        return -1;
    }
    if (open_kind == 1 && kernel_mode == 11)
        value = "14\n";
    if (open_kind == 1 && kernel_mode == 23)
        value = "13junk\n";
    if (open_kind == 2 && kernel_mode == 12)
        value = "pf0vf3\n";
    if (open_kind == 2 && kernel_mode == 14) {
        memset(oversized, 'x', sizeof(oversized));
        oversized[64] = '\0';
        value = oversized;
    }
    available = strlen(value);
    if (read_offset >= available)
        return 0;
    available -= read_offset;
    if (available > count)
        available = count;
    /** 故意让每次读取只返回少量字节，验证短读不会被误当成完整名称 */
    if (kernel_mode == 24 && available > 2)
        available = 2;
    memcpy(buffer, value + read_offset, available);
    read_offset += available;
    return (ssize_t)available;
}

char *__wrap_realpath(const char *path, char *resolved)
{
    const char *target =
        kernel_mode == 13 ? "/sys/devices/pci0000:00/0000:03:00.1" : "/sys/devices/pci0000:00/0000:03:00.0";

    assert(strcmp(path, "/sys/class/net/pf0vf2/device") == 0);
    if (kernel_mode == 17) {
        errno = ENOENT;
        return NULL;
    }
    memcpy(resolved, target, strlen(target) + 1U);
    return resolved;
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
            check_unknown(&topology.endpoints[0], expected[kernel_mode]);
        else {
            assert(topology.endpoints[0].role == DPPD_ENDPOINT_ROLE_VF && topology.endpoints[0].function_id == 2);
            assert(kernel_receives == 2 && topology.endpoints[0].role_error == 0);
        }
    }
}

int main(void)
{
    test_role_parser();
    test_identity_evidence();
    test_discovery_atomicity();
    test_default_reader();
    return 0;
}
