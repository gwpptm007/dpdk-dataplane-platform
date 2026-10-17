#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <stdio.h>
#include <string.h>
#include "dppd/device.h"
#include "dppd/device_safety.h"

#define SEQUENCE 77U

struct packet {
    uint8_t bytes[4096];
    size_t length;
};

static void append_message(struct packet *packet, uint16_t type, uint16_t flags, const void *payload, size_t length)
{
    struct nlmsghdr header = {.nlmsg_len = NLMSG_LENGTH(length),
        .nlmsg_type = type,
        .nlmsg_flags = flags,
        .nlmsg_seq = SEQUENCE};
    size_t total = NLMSG_ALIGN(header.nlmsg_len);
    assert(packet->length + total <= sizeof(packet->bytes));
    memset(packet->bytes + packet->length, 0, total);
    memcpy(packet->bytes + packet->length, &header, sizeof(header));
    if (length != 0)
        memcpy(packet->bytes + packet->length + sizeof(header), payload, length);
    packet->length += total;
}

static size_t attribute(uint8_t *output, uint16_t type, const void *payload, size_t length)
{
    struct rtattr header = {.rta_type = type, .rta_len = RTA_LENGTH(length)};
    memset(output, 0, RTA_ALIGN(header.rta_len));
    memcpy(output, &header, sizeof(header));
    memcpy(output + sizeof(header), payload, length);
    return RTA_ALIGN(header.rta_len);
}

static void route(struct packet *packet, uint8_t family, uint8_t prefix, uint8_t table, uint32_t interface)
{
    uint8_t payload[128] = {0};
    struct rtmsg header = {.rtm_family = family, .rtm_dst_len = prefix, .rtm_table = table, .rtm_type = RTN_UNICAST};
    memcpy(payload, &header, sizeof(header));
    size_t length = sizeof(header) + attribute(payload + sizeof(header), RTA_OIF, &interface, sizeof(interface));
    append_message(packet, RTM_NEWROUTE, NLM_F_MULTI, payload, length);
}

/** 双栈、多表和全部多路径下一跳都必须留下，不能只保留第一条 IPv4 主表路由 */
static void route_inventory(void)
{
    struct packet packet = {0};
    route(&packet, AF_INET, 0, RT_TABLE_MAIN, 3);
    route(&packet, AF_INET6, 0, 100, 5);
    route(&packet, AF_INET, 24, RT_TABLE_MAIN, 99);
    uint8_t payload[128] = {0}, hops[32] = {0};
    struct rtmsg header = {.rtm_family = AF_INET6, .rtm_type = RTN_UNICAST};
    memcpy(payload, &header, sizeof(header));
    uint32_t table = 70000;
    size_t length = sizeof(header) + attribute(payload + sizeof(header), RTA_TABLE, &table, sizeof(table));
    struct rtnexthop first = {.rtnh_len = sizeof(first), .rtnh_ifindex = 7};
    struct rtnexthop second = {.rtnh_len = sizeof(second), .rtnh_ifindex = 9, .rtnh_flags = RTNH_F_DEAD};
    memcpy(hops, &first, sizeof(first));
    memcpy(hops + sizeof(first), &second, sizeof(second));
    length += attribute(payload + length, RTA_MULTIPATH, hops, sizeof(first) + sizeof(second));
    append_message(&packet, RTM_NEWROUTE, NLM_F_MULTI, payload, length);
    append_message(&packet, NLMSG_DONE, NLM_F_MULTI, NULL, 0);
    struct dppd_device_safety_interfaces interfaces = {0};
    bool done = false;
    assert(dppd_device_safety_parse_routes(packet.bytes, packet.length, SEQUENCE, &interfaces, &done) == 0);
    assert(done && interfaces.count == 4 && interfaces.indices[0] == 3 && interfaces.indices[1] == 5 &&
        interfaces.indices[2] == 7 && interfaces.indices[3] == 9);

    /* 下一数据报没有 DONE 时只是未完成，重复接口不会产生重复保护项 */
    packet.length = 0;
    route(&packet, AF_INET, 0, RT_TABLE_MAIN, 3);
    done = false;
    assert(dppd_device_safety_parse_routes(packet.bytes, packet.length, SEQUENCE, &interfaces, &done) == 0);
    assert(!done && interfaces.count == 4);
}

static void route_corruption(void)
{
    struct packet original = {0};
    route(&original, AF_INET, 0, RT_TABLE_MAIN, 3);
    append_message(&original, NLMSG_DONE, NLM_F_MULTI, NULL, 0);
    for (size_t length = 0; length < original.length; ++length) {
        struct dppd_device_safety_interfaces interfaces = {0};
        bool done = false;
        int rc = dppd_device_safety_parse_routes(original.bytes, length, SEQUENCE, &interfaces, &done);
        assert(rc != 0 || !done);
    }
    for (uint32_t kind = 0; kind < 8; ++kind) {
        struct packet broken = original;
        struct nlmsghdr header;
        memcpy(&header, broken.bytes, sizeof(header));
        if (kind == 0)
            header.nlmsg_seq++;
        if (kind == 1)
            header.nlmsg_len = sizeof(header) - 1U;
        if (kind == 2)
            header.nlmsg_len = sizeof(broken.bytes) + 1U;
        if (kind == 3)
            header.nlmsg_flags |= NLM_F_DUMP_INTR;
        if (kind == 4)
            header.nlmsg_type = NLMSG_OVERRUN;
        if (kind == 5)
            header.nlmsg_flags = 0;
        memcpy(broken.bytes, &header, sizeof(header));
        if (kind == 6) {
            struct rtattr attribute = {.rta_type = RTA_OIF, .rta_len = 3};
            memcpy(broken.bytes + sizeof(header) + sizeof(struct rtmsg), &attribute, sizeof(attribute));
        }
        if (kind == 7) {
            uint32_t unknown = 0;
            memcpy(broken.bytes + sizeof(header) + sizeof(struct rtmsg) + sizeof(struct rtattr), &unknown,
                sizeof(unknown));
        }
        struct dppd_device_safety_interfaces interfaces = {.count = 1, .indices = {42}};
        bool done = false;
        assert(dppd_device_safety_parse_routes(broken.bytes, broken.length, SEQUENCE, &interfaces, &done) < 0);
        assert(interfaces.count == 1 && interfaces.indices[0] == 42 && !done);
    }

    /* nexthop object 只有 ID 没有可证接口时保守拒绝，不能漏掉其后真正承载管理的设备 */
    uint8_t payload[64] = {0};
    struct rtmsg route_header = {.rtm_family = AF_INET, .rtm_type = RTN_UNICAST};
    memcpy(payload, &route_header, sizeof(route_header));
    struct packet packet = {0};
    append_message(&packet, RTM_NEWROUTE, NLM_F_MULTI, payload, sizeof(route_header));
    struct dppd_device_safety_interfaces interfaces = {0};
    bool done = false;
    assert(dppd_device_safety_parse_routes(packet.bytes, packet.length, SEQUENCE, &interfaces, &done) == -ENODATA);
    packet.length = 0;
    struct nlmsgerr error = {.error = -EPERM};
    append_message(&packet, NLMSG_ERROR, 0, &error, sizeof(error));
    assert(dppd_device_safety_parse_routes(packet.bytes, packet.length, SEQUENCE, &interfaces, &done) == -EPERM);
    packet.length = 0;
    int status = -EINTR;
    append_message(&packet, NLMSG_DONE, NLM_F_MULTI, &status, sizeof(status));
    assert(dppd_device_safety_parse_routes(packet.bytes, packet.length, SEQUENCE, &interfaces, &done) == -EINTR);

    /* 多路径头或内部属性被截断时整份数据报拒绝，消息计数耗尽也不能回绕后继续放行 */
    for (uint16_t size = 0; size <= sizeof(struct rtnexthop) + 2U; ++size) {
        packet.length = 0;
        memset(payload, 0, sizeof(payload));
        memcpy(payload, &route_header, sizeof(route_header));
        struct rtnexthop hop = {.rtnh_len = size, .rtnh_ifindex = 3};
        uint8_t nested[16] = {0};
        memcpy(nested, &hop, sizeof(hop));
        size_t length =
            sizeof(route_header) + attribute(payload + sizeof(route_header), RTA_MULTIPATH, nested, sizeof(hop));
        append_message(&packet, RTM_NEWROUTE, NLM_F_MULTI, payload, length);
        memset(&interfaces, 0, sizeof(interfaces));
        done = false;
        int rc = dppd_device_safety_parse_routes(packet.bytes, packet.length, SEQUENCE, &interfaces, &done);
        assert(rc == (size == sizeof(hop) ? 0 : -EBADMSG));
    }
    packet.length = 0;
    route(&packet, AF_INET, 0, RT_TABLE_MAIN, 3);
    interfaces.messages = UINT32_MAX;
    interfaces.count = 0;
    done = false;
    assert(dppd_device_safety_parse_routes(packet.bytes, packet.length, SEQUENCE, &interfaces, &done) == -E2BIG);
}

static void identity_parsing(void)
{
    char output[DPPD_DEVICE_SAFETY_BDF_SIZE];
    assert(dppd_device_safety_extract_bdf("ABCD:AF:1F.7", output) == 0 && strcmp(output, "abcd:af:1f.7") == 0);
    assert(dppd_device_safety_extract_bdf("0000:03:00.0_representor_c0pf0vf1", output) == 0 &&
        strcmp(output, "0000:03:00.0") == 0);
    const char *invalid[] = {"", "net_ring0", "03:00.0", "0000:03:00.00", "0000:03:00.0evil", "0000:03:20.0",
        "0000:03:00.8", "x0000:03:00.0", "/sys/0000:03:00.0", "0000:03:00.0_representor_", "0000:03:00.0_other",
        "0000:03:00.0_representor_a/b"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(dppd_device_safety_extract_bdf(invalid[i], output) == -ENODATA);
    assert(dppd_device_safety_pci_network_class("0x020000\n") == 0);
    assert(dppd_device_safety_pci_network_class("0x028000\n") == 0);
    const char *classes[] = {"", "0x060400", "0x030000", "0x020000junk", "0x100020000", "-1", "unknown"};
    for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); ++i)
        assert(dppd_device_safety_pci_network_class(classes[i]) == -ENODATA);
}

static void ssh_local_interfaces(void)
{
    struct dppd_device_safety_address addresses[3] = {
        {.ifindex = 3, .family = AF_INET, .interface_name = "management0"},
        {.ifindex = 7, .family = AF_INET6, .interface_name = "management1"},
        {.ifindex = 9, .family = AF_INET, .interface_name = "client0"}};
    assert(inet_pton(AF_INET, "192.0.2.5", addresses[0].address) == 1);
    assert(inet_pton(AF_INET6, "fe80::1234", addresses[1].address) == 1);
    assert(inet_pton(AF_INET, "192.0.2.9", addresses[2].address) == 1);
    const char *valid[] = {"192.0.2.9 5555 192.0.2.5 22", "2001:db8::9 5555 fe80::1234%management1 22",
        "2001:db8::9 5555 fe80::1234%7 22", "192.0.2.9 5555 ::ffff:192.0.2.5 22"};
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        struct dppd_device_safety_interfaces interfaces = {0};
        assert(dppd_device_safety_ssh_interfaces(valid[i], addresses, 3, &interfaces) == 0 && interfaces.count == 1);
        assert(interfaces.indices[0] == (i == 1 || i == 2 ? 7U : 3U));
    }
    const char *invalid[] = {"bad", "192.0.2.9 0 192.0.2.5 22", "192.0.2.9 5555 192.0.2.5 65536",
        "192.0.2.9 5555 192.0.2.6 22", "192.0.2.9 5555 fe80::1234%wrong 22", "192.0.2.9 5555 192.0.2.5 22 extra"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        struct dppd_device_safety_interfaces interfaces = {0};
        assert(dppd_device_safety_ssh_interfaces(invalid[i], addresses, 3, &interfaces) == -ENODATA);
    }
}

static struct {
    int route_error, ssh_error, interface_error, device_error;
    uint32_t route_count;
    bool ssh_present, same_parent;
    const char *parent;
} fake;

static int routes(void *context, struct dppd_device_safety_interfaces *interfaces)
{
    (void)context;
    interfaces->count = fake.route_count;
    for (uint32_t i = 0; i < interfaces->count; ++i)
        interfaces->indices[i] = i + 3U;
    return fake.route_error;
}

static int ssh(void *context, struct dppd_device_safety_interfaces *interfaces)
{
    (void)context;
    if (fake.ssh_present)
        interfaces->indices[interfaces->count++] = 7;
    return fake.ssh_error;
}

static int interface_parent(void *context, uint32_t index, char output[DPPD_DEVICE_SAFETY_BDF_SIZE])
{
    (void)context;
    assert(index >= 3);
    strcpy(output, fake.parent);
    return fake.interface_error;
}

static int device_parent(void *context, const char *name, char output[DPPD_DEVICE_SAFETY_BDF_SIZE])
{
    (void)context;
    assert(strncmp(name, "0000:03:00.", 11) == 0);
    strcpy(output, fake.same_parent ? "0000:02:00.0" : "0000:03:00.0");
    return fake.device_error;
}

/** 管理接口与 representor/VF 只要归到同一 parent 就拒绝，不因为接口名和 BDF 后缀不同而放行 */
static void management_protection(void)
{
    struct dppd_device_set devices = {.nb_ports = 1};
    devices.ports[0].configured = true;
    devices.ports[0].identity.device_name_known = true;
    strcpy(devices.ports[0].identity.device_name, "0000:03:00.1_representor_vf1");
    struct dppd_device_safety_reader reader = {.routes = routes,
        .ssh = ssh,
        .interface_parent = interface_parent,
        .device_parent = device_parent};
    fake.route_count = 1;
    fake.parent = "0000:02:00.0";
    assert(dppd_devices_check_reset_safety_with_reader(&devices, &reader) == 0);
    fake.same_parent = true;
    assert(dppd_devices_check_reset_safety_with_reader(&devices, &reader) == -EACCES);
    fake.same_parent = false;
    fake.route_count = 0;
    assert(dppd_devices_check_reset_safety_with_reader(&devices, &reader) == -ENODATA);
    fake.ssh_present = true;
    assert(dppd_devices_check_reset_safety_with_reader(&devices, &reader) == 0);
    fake.same_parent = true;
    assert(dppd_devices_check_reset_safety_with_reader(&devices, &reader) == -EACCES);
    fake.same_parent = false;
    fake.route_count = 1;
    fake.ssh_present = false;
    const int errors[] = {-EPERM, -ENODATA, -ETIMEDOUT, -EMSGSIZE, -EBADMSG};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        fake.route_error = errors[i];
        assert(dppd_devices_check_reset_safety_with_reader(&devices, &reader) == errors[i]);
    }
    fake.route_error = 0;
    fake.ssh_error = -ENODATA;
    assert(dppd_devices_check_reset_safety_with_reader(&devices, &reader) == -ENODATA);
    fake.ssh_error = 0;
    fake.interface_error = -ENODATA;
    assert(dppd_devices_check_reset_safety_with_reader(&devices, &reader) == -ENODATA);
    fake.interface_error = 0;
    fake.device_error = -ENODATA;
    assert(dppd_devices_check_reset_safety_with_reader(&devices, &reader) == -ENODATA);
    fake.device_error = 0;
    fake.parent = "not-a-pci";
    assert(dppd_devices_check_reset_safety_with_reader(&devices, &reader) == -ENODATA);
    fake.parent = "0000:02:00.0";
    devices.ports[0].identity.device_name_known = false;
    assert(dppd_devices_check_reset_safety_with_reader(&devices, &reader) == -ENODATA);
    devices.ports[0].identity.device_name_known = true;
    strcpy(devices.ports[0].identity.device_name, "0000:03:00.10");
    assert(dppd_devices_check_reset_safety_with_reader(&devices, &reader) == -ENODATA);
    assert(dppd_devices_check_reset_safety(NULL) == -EINVAL);
}

int main(void)
{
    route_inventory();
    route_corruption();
    identity_parsing();
    ssh_local_interfaces();
    management_protection();
    return 0;
}
