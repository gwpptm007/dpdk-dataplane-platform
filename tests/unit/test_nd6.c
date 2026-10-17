#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <rte_mbuf.h>
#include "dppd/nd6.h"

static const uint8_t macs[DPPD_MAX_PORTS][6] = {{2, 0, 0, 0, 0, 10}, {2, 0, 0, 0, 0, 20}};
static const uint8_t peer[6] = {2, 0, 0, 0, 1, 1};
struct capture {
    uint32_t count;
    uint16_t port, length;
    uint8_t wire[256];
};
static int capture(void *context, uint16_t port, const uint8_t *wire, uint16_t length)
{
    struct capture *result = context;
    result->count++;
    result->port = port;
    result->length = length;
    memcpy(result->wire, wire, length);
    return 0;
}
static void address(uint8_t output[16], const char *text)
{
    assert(inet_pton(AF_INET6, text, output) == 1);
}
/** 独立拼接伪首部逐个网络字求和，不调用生产 ND 的校验和函数 */
static void crc(uint8_t *ip, unsigned int length)
{
    uint8_t bytes[320] = {0};
    memcpy(bytes, ip + 8, 32);
    bytes[35] = (uint8_t)length;
    bytes[39] = 58;
    ip[42] = ip[43] = 0;
    memcpy(bytes + 40, ip + 40, length);
    uint32_t sum = 0;
    for (unsigned int i = 0; i < length + 40; i += 2)
        sum += (uint32_t)bytes[i] * 256 + bytes[i + 1];
    while (sum > 65535)
        sum = (sum & 65535U) + (sum >> 16);
    uint16_t value = (uint16_t)~sum;
    ip[42] = (uint8_t)(value >> 8);
    ip[43] = (uint8_t)value;
}
static uint16_t frame(uint8_t bytes[256], unsigned int vlan, uint8_t type, const char *source, const char *destination,
    const char *target, uint8_t flags, bool option)
{
    memset(bytes, 0, 256);
    unsigned int l2 = 14 + 4 * vlan;
    memcpy(bytes, macs[1], 6);
    memcpy(bytes + 6, peer, 6);
    for (unsigned int i = 0; i < vlan; ++i) {
        bytes[12 + 4 * i] = vlan == 2 && i == 0 ? 0x88 : 0x81;
        bytes[13 + 4 * i] = vlan == 2 && i == 0 ? 0xa8 : 0;
        bytes[15 + 4 * i] = (uint8_t)(37 + i);
    }
    bytes[l2 - 2] = 0x86;
    bytes[l2 - 1] = 0xdd;
    uint8_t *ip = bytes + l2, *body = ip + 40;
    ip[0] = 0x60;
    ip[5] = option ? 32 : 24;
    ip[6] = 58;
    ip[7] = 255;
    address(ip + 8, source);
    address(ip + 24, destination);
    if (ip[24] == 255) {
        bytes[0] = bytes[1] = 0x33;
        memcpy(bytes + 2, ip + 36, 4);
    }
    body[0] = type;
    body[4] = flags;
    address(body + 8, target);
    if (option) {
        body[24] = type == 135 ? 1 : 2;
        body[25] = 1;
        memcpy(body + 26, peer, 6);
    }
    crc(ip, ip[5]);
    return (uint16_t)(l2 + 40 + ip[5]);
}
static bool input(struct dppd_nd6 *table, unsigned int vlan, uint16_t port, uint8_t *wire, uint16_t length,
    uint64_t now, struct capture *output)
{
    struct rte_mbuf mbuf = {.buf_addr = wire, .buf_len = 256, .pkt_len = length, .data_len = length, .nb_segs = 1};
    struct dppd_packet packet;
    assert(dppd_packet_parse_mbuf(&mbuf, &packet) != DPPD_PARSE_MALFORMED);
    assert(packet.vlan_depth == vlan);
    return dppd_nd6_input(table, port, &mbuf, &packet, now, capture, output);
}
static struct dppd_nd6 *create(unsigned int vlan, struct capture *output)
{
    struct dppd_config cfg;
    dppd_config_defaults(&cfg);
    strcpy(cfg.routing_path, "/private/routes");
    cfg.nd6_count = 2;
    cfg.nd6_reachable_ms = 1000;
    for (unsigned int i = 0; i < 2; ++i) {
        cfg.nd6[i].port_id = (uint16_t)i;
        cfg.nd6[i].vlan_depth = (uint8_t)vlan;
        for (unsigned int tag = 0; tag < vlan; ++tag)
            cfg.nd6[i].vlan[tag] = (uint16_t)(37 + tag);
        address(cfg.nd6[i].address, i == 0 ? "fe80::10" : "fe80::20");
    }
    struct dppd_nd6 *table = NULL;
    assert(dppd_nd6_create(&table, &cfg, macs, 1000) == 0);
    assert(!dppd_nd6_port_ready(table, 0));
    dppd_nd6_tick(table, 2001, capture, output);
    assert(output->count == 2 && output->length == 78 + 4 * vlan);
    assert(memcmp(output->wire + 14 + 4 * vlan + 8, (uint8_t[16]){0}, 16) == 0);
    dppd_nd6_tick(table, 3001, capture, output);
    assert(dppd_nd6_port_ready(table, 0) && dppd_nd6_port_ready(table, 1));
    return table;
}
static struct dppd_nd6_entry entry(struct dppd_nd6 *table)
{
    struct dppd_nd6_page page;
    assert(dppd_nd6_list(table, 0, 0, &page) == 0 && page.count == 1);
    return page.entries[0];
}

/** 首包等待、跨队列提取、完整 128 位键与可达性状态都使用可控时间，不靠长时间睡眠 */
static void lifecycle(unsigned int vlan)
{
    struct capture output = {0};
    struct dppd_nd6 *table = create(vlan, &output);
    struct dppd_packet packet = {.vlan_depth = (uint8_t)vlan,
        .vlan_tci = {37, 38},
        .vlan_tpid = {vlan == 2 ? 0x88a8 : 0x8100, 0x8100}};
    uint8_t target[16], mac[6], bytes[256];
    address(target, "fe80::1");
    assert(dppd_nd6_resolve(table, 1, target, &packet, 3010, mac) == -EAGAIN);
    struct rte_mbuf waiting[5];
    memset(waiting, 0, sizeof(waiting));
    for (uint16_t i = 0; i < 4; ++i)
        assert(dppd_nd6_hold(table, 1, target, (struct dppd_nd6_pending){.mbuf = &waiting[i], .queue = i}, 3010) == 0);
    assert(dppd_nd6_hold(table, 1, target, (struct dppd_nd6_pending){.mbuf = waiting + 4}, 3010) == -ENOBUFS);
    dppd_nd6_tick(table, 3020, capture, &output);
    assert(output.count == 3 && output.port == 1 && output.wire[0] == 0x33);
    dppd_nd6_tick(table, 3050, capture, &output);
    assert(output.count == 3);
    uint16_t length = frame(bytes, vlan, 136, "fe80::1", "fe80::20", "fe80::1", 0x60, true);
    assert(input(table, vlan, 1, bytes, length, 3060, &output));
    assert(dppd_nd6_resolve(table, 1, target, &packet, 3061, mac) == 0 && memcmp(mac, peer, 6) == 0);
    for (uint16_t i = 0; i < 4; ++i) {
        struct dppd_nd6_pending pending;
        assert(dppd_nd6_take(table, i, 3062, &pending) && pending.mbuf == &waiting[i] && !pending.expired);
    }
    struct dppd_nd6_entry current = entry(table);
    dppd_nd6_tick(table, current.due_ms + 1, capture, &output);
    assert(entry(table).state == DPPD_ND6_STALE);
    assert(dppd_nd6_resolve(table, 1, target, &packet, current.due_ms + 2, mac) == 0);
    current = entry(table);
    assert(current.state == DPPD_ND6_DELAY);
    dppd_nd6_tick(table, current.due_ms + 1, capture, &output);
    assert(entry(table).state == DPPD_ND6_PROBE && memcmp(output.wire, peer, 6) == 0);
    for (unsigned int i = 0; i < 3; ++i)
        dppd_nd6_tick(table, current.due_ms + 1001 + i * 1000, capture, &output);
    current = entry(table);
    assert(current.state == DPPD_ND6_FAILED && current.probes == 3);
    assert(dppd_nd6_resolve(table, 1, target, &packet, current.due_ms - 1, mac) == -EHOSTUNREACH);
    assert(dppd_nd6_resolve(table, 1, target, &packet, current.due_ms + 1, mac) == -EAGAIN);
    struct dppd_nd6_status status;
    dppd_nd6_status(table, &status);
    assert(status.failed == 1 && status.pending == 0 && status.queue_drops == 1);
    assert(dppd_nd6_flush(table, status.revision - 1) == -ESTALE);
    assert(dppd_nd6_flush(table, status.revision) == 0);
    assert(dppd_nd6_list(table, 0, status.revision, &(struct dppd_nd6_page){0}) == -ESTALE);
    dppd_nd6_destroy(table);
}

/** 坏 Hop Limit、校验和、选项、地址和错误作用域不能把 INCOMPLETE 变成可用邻居 */
static void invalid_packets(void)
{
    struct capture output = {0};
    struct dppd_nd6 *table = create(0, &output);
    uint8_t target[16], mac[6];
    address(target, "fe80::1");
    struct dppd_packet packet = {0};
    assert(dppd_nd6_resolve(table, 1, target, &packet, 3010, mac) == -EAGAIN);
    for (unsigned int mode = 0; mode < 10; ++mode) {
        uint8_t bytes[256];
        uint16_t length = frame(bytes, 0, 136, "fe80::1", "fe80::20", "fe80::1", 0x60, true);
        uint8_t *ip = bytes + 14;
        if (mode == 0)
            ip[7] = 254;
        if (mode == 1)
            ip[41] = 1;
        if (mode == 2)
            ip[65] = 0;
        if (mode == 3)
            ip[66] = 1;
        if (mode == 4)
            memset(ip + 8, 0, 16);
        if (mode == 5)
            ip[48] = 255;
        if (mode == 6)
            bytes[0] = 4;
        if (mode == 7) {
            address(ip + 24, "ff02::1");
            bytes[0] = bytes[1] = 0x33;
            memcpy(bytes + 2, ip + 36, 4);
        }
        crc(ip, 32);
        if (mode == 8)
            ip[43] ^= 1;
        if (mode == 9) {
            /** 即使原子分片携带完整有效 NA，也不能进入邻居学习路径 */
            memmove(ip + 48, ip + 40, 32);
            memset(ip + 40, 0, 8);
            ip[6] = 44;
            ip[40] = 58;
            ip[5] = 40;
            length += 8;
        }
        assert(input(table, 0, 1, bytes, length, 3020, &output));
        assert(entry(table).state == DPPD_ND6_INCOMPLETE);
    }
    uint8_t bytes[256];
    uint16_t length = frame(bytes, 0, 136, "fe80::1", "fe80::20", "fe80::1", 0x60, false);
    assert(input(table, 0, 1, bytes, length, 3020, &output));
    assert(entry(table).state == DPPD_ND6_INCOMPLETE);
    struct dppd_nd6_status status;
    dppd_nd6_status(table, &status);
    assert(status.invalid == 10 && status.resolved == 0);
    dppd_nd6_destroy(table);
}

/** 本机 NS 回应和 DAD 独立于数据 ACL，检测到重复后不能靠清缓存解除本机地址冲突 */
static void local_neighbors(void)
{
    struct capture output = {0};
    struct dppd_nd6 *table = create(0, &output);
    uint8_t bytes[256];
    uint16_t length = frame(bytes, 0, 135, "fe80::1", "ff02::1:ff00:20", "fe80::20", 0, true);
    assert(input(table, 0, 1, bytes, length, 3010, &output));
    assert(output.wire[54] == 136 && output.wire[58] == 0xe0 && output.count == 3);
    assert(entry(table).state == DPPD_ND6_STALE);
    length = frame(bytes, 0, 135, "::", "ff02::1:ff00:20", "fe80::20", 0, false);
    assert(input(table, 0, 1, bytes, length, 3020, &output));
    assert(dppd_nd6_port_ready(table, 1) && output.wire[58] == 0xa0 && output.wire[0] == 0x33);
    dppd_nd6_destroy(table);
    struct dppd_config cfg;
    dppd_config_defaults(&cfg);
    strcpy(cfg.routing_path, "/private/routes");
    cfg.nd6_count = 1;
    cfg.nd6[0].port_id = 1;
    address(cfg.nd6[0].address, "fe80::20");
    table = NULL;
    assert(dppd_nd6_create(&table, &cfg, macs, 0) == 0);
    assert(input(table, 0, 1, bytes, length, 10, &output));
    dppd_nd6_tick(table, 10000, capture, &output);
    assert(!dppd_nd6_port_ready(table, 1));
    struct dppd_nd6_status status;
    dppd_nd6_status(table, &status);
    assert(status.locals[0].state == DPPD_ND6_DUPLICATE);
    assert(dppd_nd6_flush(table, status.revision) == 0 && !dppd_nd6_port_ready(table, 1));
    dppd_nd6_destroy(table);
}

/** 覆盖标志不能偷偷换 MAC，缓存满、分页与清空也不能产生重复 ID 或遗留等待包 */
static void overrides_and_bounds(void)
{
    struct capture output = {0};
    struct dppd_nd6 *table = create(0, &output);
    uint8_t target[16], mac[6], bytes[256];
    struct dppd_packet packet = {0};
    address(target, "fe80::1");
    assert(dppd_nd6_resolve(table, 1, target, &packet, 3010, mac) == -EAGAIN);
    uint16_t length = frame(bytes, 0, 136, "fe80::1", "fe80::20", "fe80::1", 0x60, true);
    assert(input(table, 0, 1, bytes, length, 3020, &output));
    bytes[11] = bytes[85] = 9;
    bytes[58] = 0x40;
    crc(bytes + 14, 32);
    assert(input(table, 0, 1, bytes, length, 3030, &output));
    assert(entry(table).state == DPPD_ND6_STALE && entry(table).mac[5] == 1);
    bytes[58] = 0x60;
    crc(bytes + 14, 32);
    assert(input(table, 0, 1, bytes, length, 3040, &output));
    assert(entry(table).state == DPPD_ND6_REACHABLE && entry(table).mac[5] == 9);
    for (uint32_t i = 0; i < DPPD_ND6_LIMIT - 1; ++i) {
        address(target, "2001:db8:ffff::");
        target[14] = (uint8_t)(i >> 8);
        target[15] = (uint8_t)i;
        assert(dppd_nd6_resolve(table, 0, target, &packet, 3050, mac) == -EAGAIN);
    }
    target[14] = 1;
    assert(dppd_nd6_resolve(table, 0, target, &packet, 3050, mac) == -ENOSPC);
    uint64_t after = 0, revision = 0;
    unsigned int total = 0;
    struct dppd_nd6_page page;
    do {
        assert(dppd_nd6_list(table, after, revision, &page) == 0);
        for (uint32_t i = 0; i < page.count; ++i) {
            assert(page.entries[i].id > after);
            after = page.entries[i].id;
        }
        total += page.count;
        revision = page.revision;
    } while (page.more);
    assert(total == DPPD_ND6_LIMIT);
    address(target, "2001:db8:ffff::1");
    struct rte_mbuf queued;
    memset(&queued, 0, sizeof(queued));
    assert(dppd_nd6_hold(table, 0, target, (struct dppd_nd6_pending){.mbuf = &queued, .queue = 1}, 3060) == 0);
    assert(dppd_nd6_flush(table, revision) == 0);
    struct dppd_nd6_pending pending;
    assert(dppd_nd6_take(table, 1, 3060, &pending) && pending.expired && pending.mbuf == &queued);
    dppd_nd6_destroy(table);
}
int main(void)
{
    for (unsigned int vlan = 0; vlan <= 2; ++vlan)
        lifecycle(vlan);
    invalid_packets();
    local_neighbors();
    overrides_and_bounds();
    return 0;
}
