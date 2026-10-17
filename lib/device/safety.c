#define _GNU_SOURCE
#include "dppd/device_safety.h"
#include "dppd/device.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SAFETY_MAX_MESSAGES 16384U
#define SAFETY_MAX_DATAGRAMS 1024U
#define SAFETY_ROUTE_BUFFER_SIZE 65536U
#define SAFETY_ROUTE_TIMEOUT_MS 2000

static int add_interface(struct dppd_device_safety_interfaces *interfaces, uint32_t index)
{
    if (interfaces->count > DPPD_DEVICE_SAFETY_MAX_INTERFACES || index == 0 || index > INT_MAX)
        return -ENODATA;
    for (uint32_t i = 0; i < interfaces->count; ++i)
        if (interfaces->indices[i] == index)
            return 0;
    if (interfaces->count == DPPD_DEVICE_SAFETY_MAX_INTERFACES)
        return -E2BIG;
    interfaces->indices[interfaces->count++] = index;
    return 0;
}

static int hex_digit(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

/** 只接受完整 BDF 和明确的 representor 后缀，不能把相似前缀认成同一个 PCI 设备 */
int dppd_device_safety_extract_bdf(const char *name, char output[DPPD_DEVICE_SAFETY_BDF_SIZE])
{
    if (name == NULL || output == NULL || strnlen(name, DPPD_CAPABILITY_NAME_SIZE) >= DPPD_CAPABILITY_NAME_SIZE)
        return -ENODATA;
    size_t length = strlen(name);
    if (length < 12 || name[4] != ':' || name[7] != ':' || name[10] != '.')
        return -ENODATA;
    for (size_t i = 0; i < 12; ++i)
        if (i != 4 && i != 7 && i != 10 && hex_digit(name[i]) < 0)
            return -ENODATA;
    if (hex_digit(name[8]) > 1 || name[11] < '0' || name[11] > '7')
        return -ENODATA;
    if (length != 12) {
        static const char suffix[] = "_representor_";
        if (strncmp(name + 12, suffix, sizeof(suffix) - 1U) != 0 || name[12 + sizeof(suffix) - 1U] == '\0')
            return -ENODATA;
        for (const char *cursor = name + 12 + sizeof(suffix) - 1U; *cursor; ++cursor)
            if (!isalnum((unsigned char)*cursor) && *cursor != '_')
                return -ENODATA;
    }
    for (size_t i = 0; i < 12; ++i)
        output[i] = (char)tolower((unsigned char)name[i]);
    output[12] = '\0';
    return 0;
}

static int check_attributes(const uint8_t *data, size_t length)
{
    while (length != 0) {
        struct rtattr attribute;
        if (length < sizeof(attribute))
            return -EBADMSG;
        memcpy(&attribute, data, sizeof(attribute));
        if (attribute.rta_len < sizeof(attribute) || attribute.rta_len > length ||
            RTA_ALIGN(attribute.rta_len) > length)
            return -EBADMSG;
        size_t consumed = RTA_ALIGN(attribute.rta_len);
        data += consumed;
        length -= consumed;
    }
    return 0;
}

static int multipath_interfaces(const uint8_t *data, size_t length, struct dppd_device_safety_interfaces *interfaces)
{
    if (length == 0)
        return -ENODATA;
    while (length != 0) {
        struct rtnexthop hop;
        if (length < sizeof(hop))
            return -EBADMSG;
        memcpy(&hop, data, sizeof(hop));
        if (hop.rtnh_len < sizeof(hop) || hop.rtnh_len > length || (size_t)RTNH_ALIGN(hop.rtnh_len) > length)
            return -EBADMSG;
        int rc = check_attributes(data + sizeof(hop), hop.rtnh_len - sizeof(hop));
        if (rc == 0)
            rc = add_interface(interfaces, (uint32_t)hop.rtnh_ifindex);
        if (rc != 0)
            return rc;
        size_t consumed = RTNH_ALIGN(hop.rtnh_len);
        data += consumed;
        length -= consumed;
    }
    return 0;
}

/** 不按主路由表筛选，所有表与所有下一跳都可能承载当前或后续管理连接 */
static int parse_route(const uint8_t *data, size_t length, struct dppd_device_safety_interfaces *interfaces)
{
    struct rtmsg route;
    if (length < sizeof(route))
        return -EBADMSG;
    memcpy(&route, data, sizeof(route));
    if (route.rtm_family != AF_INET && route.rtm_family != AF_INET6)
        return 0;
    unsigned int maximum = route.rtm_family == AF_INET ? 32U : 128U;
    if (route.rtm_dst_len > maximum || route.rtm_src_len > maximum)
        return -EBADMSG;
    data += sizeof(route);
    length -= sizeof(route);
    int rc = check_attributes(data, length);
    if (rc != 0)
        return rc;
    if (route.rtm_dst_len != 0 || (route.rtm_type != RTN_UNICAST && route.rtm_type != RTN_LOCAL))
        return 0;
    bool found = false, has_output = false, has_multipath = false;
    while (length != 0) {
        struct rtattr attribute;
        memcpy(&attribute, data, sizeof(attribute));
        size_t size = attribute.rta_len - sizeof(attribute);
        const uint8_t *payload = data + sizeof(attribute);
        uint16_t type = attribute.rta_type & NLA_TYPE_MASK;
        if (type == RTA_OIF) {
            uint32_t index;
            if (has_output || size != sizeof(index) || attribute.rta_type != RTA_OIF)
                return -EBADMSG;
            memcpy(&index, payload, sizeof(index));
            rc = add_interface(interfaces, index);
            if (rc != 0)
                return rc;
            has_output = found = true;
        } else if (type == RTA_MULTIPATH) {
            if (has_multipath)
                return -EBADMSG;
            rc = multipath_interfaces(payload, size, interfaces);
            if (rc != 0)
                return rc;
            has_multipath = found = true;
        } else if (type == RTA_DST) {
            if (size != (route.rtm_family == AF_INET ? 4U : 16U))
                return -EBADMSG;
            for (size_t i = 0; i < size; ++i)
                if (payload[i] != 0)
                    return -EBADMSG;
        } else if (type == RTA_TABLE && size != sizeof(uint32_t)) {
            return -EBADMSG;
        }
        size_t consumed = RTA_ALIGN(attribute.rta_len);
        data += consumed;
        length -= consumed;
    }
    return found ? 0 : -ENODATA;
}

/** 完整校验一个数据报后才发布接口集合，截断或错误序号不能留下貌似完整的结果 */
int dppd_device_safety_parse_routes(const void *input, size_t length, uint32_t sequence,
    struct dppd_device_safety_interfaces *interfaces, bool *done)
{
    if (input == NULL || interfaces == NULL || done == NULL || sequence == 0 || length == 0 ||
        interfaces->count > DPPD_DEVICE_SAFETY_MAX_INTERFACES)
        return -EINVAL;
    struct dppd_device_safety_interfaces next = *interfaces;
    bool complete = *done;
    const uint8_t *data = input;
    while (length != 0) {
        struct nlmsghdr header;
        if (complete || length < sizeof(header))
            return -EBADMSG;
        memcpy(&header, data, sizeof(header));
        if (header.nlmsg_len < sizeof(header) || header.nlmsg_len > length || NLMSG_ALIGN(header.nlmsg_len) > length ||
            header.nlmsg_seq != sequence)
            return -EBADMSG;
        if (next.messages >= SAFETY_MAX_MESSAGES)
            return -E2BIG;
        ++next.messages;
        if ((header.nlmsg_flags & NLM_F_DUMP_INTR) != 0)
            return -EINTR;
        const uint8_t *payload = data + sizeof(header);
        size_t size = header.nlmsg_len - sizeof(header);
        int rc = 0;
        if (header.nlmsg_type == RTM_NEWROUTE) {
            if ((header.nlmsg_flags & NLM_F_MULTI) == 0)
                return -EBADMSG;
            rc = parse_route(payload, size, &next);
        } else if (header.nlmsg_type == NLMSG_DONE) {
            if (size != 0 && size != sizeof(int))
                return -EBADMSG;
            int status = 0;
            if (size != 0)
                memcpy(&status, payload, sizeof(status));
            if (status > 0 || status < -4095)
                return -EBADMSG;
            if (status != 0)
                return status;
            complete = true;
        } else if (header.nlmsg_type == NLMSG_ERROR) {
            struct nlmsgerr error;
            if (size < sizeof(error))
                return -EBADMSG;
            memcpy(&error, payload, sizeof(error));
            if (error.error >= 0 || error.error < -4095)
                return -EBADMSG;
            return error.error;
        } else {
            return header.nlmsg_type == NLMSG_OVERRUN ? -EOVERFLOW : -EBADMSG;
        }
        if (rc != 0)
            return rc;
        size_t consumed = NLMSG_ALIGN(header.nlmsg_len);
        data += consumed;
        length -= consumed;
    }
    *interfaces = next;
    *done = complete;
    return 0;
}

static int64_t milliseconds(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return -1;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static int read_routes(void *context, struct dppd_device_safety_interfaces *interfaces)
{
    (void)context;
    int descriptor = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
    if (descriptor < 0)
        return -errno;
    int rc = 0;
    struct sockaddr_nl local = {.nl_family = AF_NETLINK};
    if (bind(descriptor, (struct sockaddr *)&local, sizeof(local)) != 0) {
        rc = -errno;
        goto cleanup;
    }
    int64_t started = milliseconds();
    if (started < 0) {
        rc = -errno;
        goto cleanup;
    }
    struct {
        struct nlmsghdr header;
        struct rtmsg route;
    } request = {.header = {.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg)),
                     .nlmsg_type = RTM_GETROUTE,
                     .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP},
        .route = {.rtm_family = AF_UNSPEC, .rtm_table = RT_TABLE_UNSPEC}};
    uint32_t sequence = (uint32_t)started ^ (uint32_t)getpid();
    request.header.nlmsg_seq = sequence == 0 ? 1 : sequence;
    struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
    ssize_t sent = sendto(descriptor, &request, sizeof(request), 0, (struct sockaddr *)&kernel, sizeof(kernel));
    if (sent != (ssize_t)sizeof(request)) {
        rc = sent < 0 ? -errno : -EIO;
        goto cleanup;
    }
    bool done = false;
    for (uint32_t packet = 0; packet < SAFETY_MAX_DATAGRAMS && !done; ++packet) {
        int64_t now = milliseconds();
        if (now < 0) {
            rc = -errno;
            goto cleanup;
        }
        int64_t remaining = SAFETY_ROUTE_TIMEOUT_MS - (now - started);
        if (remaining <= 0) {
            rc = -ETIMEDOUT;
            goto cleanup;
        }
        struct pollfd event = {.fd = descriptor, .events = POLLIN};
        int ready = poll(&event, 1, (int)remaining);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0 || (event.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            rc = ready == 0 ? -ETIMEDOUT : ready < 0 ? -errno : -EIO;
            goto cleanup;
        }
        union {
            struct nlmsghdr aligned;
            uint8_t data[SAFETY_ROUTE_BUFFER_SIZE];
        } buffer;
        struct sockaddr_nl source = {0};
        struct iovec vector = {.iov_base = buffer.data, .iov_len = sizeof(buffer.data)};
        struct msghdr message = {.msg_name = &source,
            .msg_namelen = sizeof(source),
            .msg_iov = &vector,
            .msg_iovlen = 1};
        ssize_t received = recvmsg(descriptor, &message, MSG_DONTWAIT);
        if (received < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (received <= 0) {
            rc = received == 0 ? -ENODATA : -errno;
            goto cleanup;
        }
        if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
            rc = -EMSGSIZE;
            goto cleanup;
        }
        if (message.msg_namelen != sizeof(source) || source.nl_family != AF_NETLINK || source.nl_pid != 0 ||
            source.nl_groups != 0) {
            rc = -EBADMSG;
            goto cleanup;
        }
        rc =
            dppd_device_safety_parse_routes(buffer.data, (size_t)received, request.header.nlmsg_seq, interfaces, &done);
        if (rc != 0)
            goto cleanup;
    }
    if (!done)
        rc = -E2BIG;
cleanup:
    if (close(descriptor) != 0 && rc == 0)
        rc = -errno;
    return rc;
}

static bool valid_port(const char *text)
{
    if (*text == '\0')
        return false;
    unsigned int result = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9' || result > 6553)
            return false;
        result = result * 10U + (unsigned int)(*text - '0');
    }
    return result >= 1 && result <= UINT16_MAX;
}

/** SSH 的第三项才是本地地址，不能把远端客户端地址误认为本机的管理接口 */
int dppd_device_safety_ssh_interfaces(const char *connection, const struct dppd_device_safety_address *addresses,
    uint32_t count, struct dppd_device_safety_interfaces *interfaces)
{
    if (interfaces == NULL || count > DPPD_DEVICE_SAFETY_MAX_ADDRESSES || (count != 0 && addresses == NULL))
        return -EINVAL;
    if (connection == NULL || *connection == '\0')
        return 0;
    if (strnlen(connection, 512) >= 512)
        return -ENODATA;
    char remote[128], local[128], source_port[16], destination_port[16], extra[2];
    if (sscanf(connection, "%127s %15s %127s %15s %1s", remote, source_port, local, destination_port, extra) != 4 ||
        !valid_port(source_port) || !valid_port(destination_port))
        return -ENODATA;
    uint8_t wanted[16] = {0}, peer[16];
    char *remote_zone = strchr(remote, '%');
    if (remote_zone != NULL)
        *remote_zone = '\0';
    if (inet_pton(AF_INET, remote, peer) != 1 && inet_pton(AF_INET6, remote, peer) != 1)
        return -ENODATA;
    char *zone = strchr(local, '%');
    if (zone != NULL)
        *zone++ = '\0';
    int family = inet_pton(AF_INET, local, wanted) == 1 ? AF_INET : AF_INET6;
    if (family == AF_INET6 && inet_pton(AF_INET6, local, wanted) != 1)
        return -ENODATA;
    if (family == AF_INET6) {
        struct in6_addr address;
        memcpy(&address, wanted, sizeof(address));
        if (IN6_IS_ADDR_V4MAPPED(&address)) {
            memmove(wanted, wanted + 12, 4);
            family = AF_INET;
        }
    }
    if (zone != NULL && (family != AF_INET6 || *zone == '\0' || strlen(zone) >= DPPD_DEVICE_SAFETY_IFNAME_SIZE))
        return -ENODATA;
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (addresses[i].family != family || memcmp(addresses[i].address, wanted, family == AF_INET ? 4U : 16U) != 0)
            continue;
        if (zone != NULL) {
            char numeric[16];
            snprintf(numeric, sizeof(numeric), "%u", addresses[i].ifindex);
            if (strnlen(addresses[i].interface_name, DPPD_DEVICE_SAFETY_IFNAME_SIZE) >=
                    DPPD_DEVICE_SAFETY_IFNAME_SIZE ||
                (strcmp(zone, numeric) != 0 && strcmp(zone, addresses[i].interface_name) != 0))
                continue;
        }
        int rc = add_interface(interfaces, addresses[i].ifindex);
        if (rc != 0)
            return rc;
        found = true;
    }
    return found ? 0 : -ENODATA;
}

static int read_ssh(void *context, struct dppd_device_safety_interfaces *interfaces)
{
    (void)context;
    const char *connection = getenv("SSH_CONNECTION");
    if (connection == NULL || *connection == '\0')
        return 0;
    struct ifaddrs *first = NULL;
    if (getifaddrs(&first) != 0)
        return -errno;
    struct dppd_device_safety_address addresses[DPPD_DEVICE_SAFETY_MAX_ADDRESSES];
    uint32_t count = 0;
    int rc = 0;
    for (const struct ifaddrs *current = first; current != NULL; current = current->ifa_next) {
        if (current->ifa_addr == NULL ||
            (current->ifa_addr->sa_family != AF_INET && current->ifa_addr->sa_family != AF_INET6))
            continue;
        if (count == DPPD_DEVICE_SAFETY_MAX_ADDRESSES || current->ifa_name == NULL ||
            strnlen(current->ifa_name, DPPD_DEVICE_SAFETY_IFNAME_SIZE) >= DPPD_DEVICE_SAFETY_IFNAME_SIZE) {
            rc = -ENODATA;
            goto cleanup;
        }
        struct dppd_device_safety_address *address = &addresses[count++];
        memset(address, 0, sizeof(*address));
        address->ifindex = if_nametoindex(current->ifa_name);
        if (address->ifindex == 0) {
            rc = -ENODATA;
            goto cleanup;
        }
        address->family = current->ifa_addr->sa_family;
        strcpy(address->interface_name, current->ifa_name);
        if (address->family == AF_INET)
            memcpy(address->address, &((const struct sockaddr_in *)current->ifa_addr)->sin_addr, 4);
        else
            memcpy(address->address, &((const struct sockaddr_in6 *)current->ifa_addr)->sin6_addr, 16);
    }
    rc = dppd_device_safety_ssh_interfaces(connection, addresses, count, interfaces);
cleanup:
    freeifaddrs(first);
    return rc;
}

static int read_small_file(const char *path, char *buffer, size_t capacity)
{
    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return -errno;
    ssize_t length;
    do {
        length = read(descriptor, buffer, capacity - 1U);
    } while (length < 0 && errno == EINTR);
    int rc = length < 0 ? -errno : length == 0 || (size_t)length == capacity - 1U ? -ENODATA : 0;
    if (length >= 0)
        buffer[length] = '\0';
    if (close(descriptor) != 0 && rc == 0)
        rc = -errno;
    return rc;
}

int dppd_device_safety_pci_network_class(const char *text)
{
    if (text == NULL || strnlen(text, 64) >= 64)
        return -ENODATA;
    char *end;
    errno = 0;
    unsigned long code = strtoul(text, &end, 0);
    const char *digits_end = end;
    while (isspace((unsigned char)*end))
        ++end;
    return errno != 0 || digits_end == text || *end != '\0' || code > 0xffffffUL || (code >> 16) != 2 ? -ENODATA : 0;
}

/** 每一层都核对网络 class，VF 沿 physfn 到 PF，避免管理 VF 与待复位 PF 被误认为独立 */
static int pci_parent(const char *name, char output[DPPD_DEVICE_SAFETY_BDF_SIZE])
{
    char bdf[DPPD_DEVICE_SAFETY_BDF_SIZE];
    int rc = dppd_device_safety_extract_bdf(name, bdf);
    if (rc != 0)
        return rc;
    for (uint32_t depth = 0; depth < 8; ++depth) {
        char path[PATH_MAX], resolved[PATH_MAX], field[64];
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s", bdf);
        if (realpath(path, resolved) == NULL)
            return -errno;
        const char *leaf = strrchr(resolved, '/');
        if (leaf == NULL || strlen(leaf + 1) != 12 || strcasecmp(leaf + 1, bdf) != 0)
            return -ENODATA;
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/class", bdf);
        rc = read_small_file(path, field, sizeof(field));
        if (rc != 0)
            return rc;
        rc = dppd_device_safety_pci_network_class(field);
        if (rc != 0)
            return rc;
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/physfn", bdf);
        if (realpath(path, resolved) == NULL) {
            if (errno != ENOENT)
                return -errno;
            struct stat metadata;
            if (lstat(path, &metadata) == 0 || errno != ENOENT)
                return -ENODATA;
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s", bdf);
            if (realpath(path, resolved) == NULL)
                return -ENODATA;
            memcpy(output, bdf, sizeof(bdf));
            return 0;
        }
        leaf = strrchr(resolved, '/');
        char parent[DPPD_DEVICE_SAFETY_BDF_SIZE];
        if (leaf == NULL || strlen(leaf + 1) != 12 || dppd_device_safety_extract_bdf(leaf + 1, parent) != 0 ||
            strcmp(parent, bdf) == 0)
            return -ENODATA;
        memcpy(bdf, parent, sizeof(bdf));
    }
    return -ELOOP;
}

static int interface_parent(void *context, uint32_t index, char output[DPPD_DEVICE_SAFETY_BDF_SIZE])
{
    (void)context;
    char name[IF_NAMESIZE], path[PATH_MAX], resolved[PATH_MAX];
    if (if_indextoname(index, name) == NULL)
        return -ENODATA;
    snprintf(path, sizeof(path), "/sys/class/net/%s/device", name);
    if (realpath(path, resolved) == NULL)
        return errno == ENOENT ? -ENODATA : -errno;
    const char *leaf = strrchr(resolved, '/');
    if (leaf == NULL || strlen(leaf + 1) != 12)
        return -ENODATA;
    return pci_parent(leaf + 1, output);
}

static int device_parent(void *context, const char *name, char output[DPPD_DEVICE_SAFETY_BDF_SIZE])
{
    (void)context;
    return pci_parent(name, output);
}

int dppd_devices_check_reset_safety_with_reader(const struct dppd_device_set *devices,
    const struct dppd_device_safety_reader *reader)
{
    if (devices == NULL || reader == NULL || devices->nb_ports == 0 || devices->nb_ports > DPPD_MAX_PORTS ||
        reader->routes == NULL || reader->ssh == NULL || reader->interface_parent == NULL ||
        reader->device_parent == NULL)
        return -EINVAL;
    struct dppd_device_safety_interfaces interfaces = {0};
    int rc = reader->routes(reader->context, &interfaces);
    if (rc == 0 && interfaces.count > DPPD_DEVICE_SAFETY_MAX_INTERFACES)
        return -ENODATA;
    if (rc == 0)
        rc = reader->ssh(reader->context, &interfaces);
    if (rc != 0)
        return rc;
    if (interfaces.count == 0 || interfaces.count > DPPD_DEVICE_SAFETY_MAX_INTERFACES)
        return -ENODATA;
    char managed[DPPD_DEVICE_SAFETY_MAX_INTERFACES][DPPD_DEVICE_SAFETY_BDF_SIZE];
    for (uint32_t i = 0; i < interfaces.count; ++i) {
        rc = reader->interface_parent(reader->context, interfaces.indices[i], managed[i]);
        char normalized[DPPD_DEVICE_SAFETY_BDF_SIZE];
        if (rc != 0)
            return rc;
        if (strnlen(managed[i], sizeof(managed[i])) != 12 ||
            dppd_device_safety_extract_bdf(managed[i], normalized) != 0)
            return -ENODATA;
        memcpy(managed[i], normalized, sizeof(normalized));
    }
    for (uint16_t i = 0; i < devices->nb_ports; ++i) {
        const struct dppd_port *port = &devices->ports[i];
        if (!port->configured || !port->identity.device_name_known ||
            strnlen(port->identity.device_name, sizeof(port->identity.device_name)) ==
                sizeof(port->identity.device_name))
            return -ENODATA;
        char bdf[DPPD_DEVICE_SAFETY_BDF_SIZE], normalized[DPPD_DEVICE_SAFETY_BDF_SIZE];
        if (dppd_device_safety_extract_bdf(port->identity.device_name, normalized) != 0)
            return -ENODATA;
        rc = reader->device_parent(reader->context, port->identity.device_name, bdf);
        if (rc != 0)
            return rc;
        if (strnlen(bdf, sizeof(bdf)) != 12 || dppd_device_safety_extract_bdf(bdf, normalized) != 0)
            return -ENODATA;
        for (uint32_t j = 0; j < interfaces.count; ++j)
            if (strcmp(normalized, managed[j]) == 0)
                return -EACCES;
    }
    return 0;
}

int dppd_devices_check_reset_safety(const struct dppd_device_set *devices)
{
    static const struct dppd_device_safety_reader reader = {.routes = read_routes,
        .ssh = read_ssh,
        .interface_parent = interface_parent,
        .device_parent = device_parent};
    return dppd_devices_check_reset_safety_with_reader(devices, &reader);
}
