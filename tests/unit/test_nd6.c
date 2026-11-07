#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <rte_mbuf.h>
#include "dppd/nd6.h"

static unsigned int fail_allocation;
void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size);

void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_allocation != 0 && --fail_allocation == 0)
        return NULL;
    return __real_calloc(count, size);
}

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
    /** 等待包集合发生变化也推进版本，清理前必须重新查询，而不是沿用分页开始时的旧版本 */
    assert(dppd_nd6_flush(table, revision) == -ESTALE);
    struct dppd_nd6_status status;
    dppd_nd6_status(table, &status);
    assert(dppd_nd6_flush(table, status.revision) == 0);
    struct dppd_nd6_pending pending;
    assert(dppd_nd6_take(table, 1, 3060, &pending) && pending.expired && pending.mbuf == &queued);
    dppd_nd6_destroy(table);
}

static struct dppd_nd6_policy policy(void)
{
    struct dppd_nd6_policy result = {.local_count = 2, .reachable_ms = 1000};
    for (uint16_t i = 0; i < 2; ++i) {
        result.locals[i].port_id = i;
        address(result.locals[i].address, i == 0 ? "fe80::10" : "fe80::20");
    }
    return result;
}

static struct dppd_nd6 *create_policy(const struct dppd_nd6_policy *value)
{
    struct dppd_nd6_port ports[2] = {{.port_id = 0}, {.port_id = 1}};
    memcpy(ports[0].mac, macs[0], 6);
    memcpy(ports[1].mac, macs[1], 6);
    struct dppd_nd6 *table = NULL;
    assert(dppd_nd6_create_policy(&table, value, ports, 2, 1000) == 0);
    return table;
}

struct publication {
    unsigned int calls;
    int result;
};

static int publish_policy(void *context)
{
    struct publication *p = context;
    p->calls++;
    return p->result;
}

/** 保留一份外部引用，可在不使用真实 mempool 的单测中精确观察队列是否归还自己的引用 */
static struct rte_mbuf referenced_packet(void)
{
    struct rte_mbuf packet = {.nb_segs = 1};
    rte_mbuf_refcnt_set(&packet, 2);
    return packet;
}

/** 退出时归还所有仍在等待队列的引用，已取走的包和已保存的策略仍由原所有者处理 */
static void discard_pending(void)
{
    struct dppd_nd6_policy enabled = policy();
    struct dppd_nd6 *table = create_policy(&enabled);
    struct capture output = {0};
    dppd_nd6_tick(table, 2001, capture, &output);
    dppd_nd6_tick(table, 3001, capture, &output);
    struct dppd_packet packet = {0};
    uint8_t target[16], mac[6];
    address(target, "fe80::1");
    assert(dppd_nd6_resolve(table, 1, target, &packet, 3010, mac) == -EAGAIN);
    struct rte_mbuf taken = referenced_packet(), first = referenced_packet(), second = referenced_packet();
    assert(dppd_nd6_hold(table, 1, target, (struct dppd_nd6_pending){.mbuf = &taken}, 3010) == 0);
    struct dppd_nd6_pending returned;
    assert(dppd_nd6_take(table, 0, 3110, &returned) && returned.mbuf == &taken && !returned.expired);
    assert(dppd_nd6_hold(table, 1, target, (struct dppd_nd6_pending){.mbuf = &first, .queue = 1}, 3110) == 0);
    assert(dppd_nd6_hold(table, 1, target, (struct dppd_nd6_pending){.mbuf = &second, .queue = 2}, 3110) == 0);
    struct dppd_nd6_status before, after;
    dppd_nd6_status(table, &before);
    assert(before.pending == 2 && before.active == 1);
    dppd_nd6_discard_pending(table);
    dppd_nd6_status(table, &after);
    assert(after.pending == 0 && after.active == 1 && after.queue_drops == before.queue_drops + 2);
    assert(after.epoch == before.epoch && after.revision == before.revision + 1);
    assert(after.enabled && after.locals[1].state == DPPD_ND6_PREFERRED);
    assert(rte_mbuf_refcnt_read(&first) == 1 && rte_mbuf_refcnt_read(&second) == 1 && rte_mbuf_refcnt_read(&taken) == 2);
    assert(!dppd_nd6_take(table, 1, 4000, &returned) && !dppd_nd6_take(table, 2, 4000, &returned));
    dppd_nd6_discard_pending(table);
    dppd_nd6_destroy(table);
    assert(rte_mbuf_refcnt_read(&first) == 1 && rte_mbuf_refcnt_read(&second) == 1 && rte_mbuf_refcnt_read(&taken) == 2);
    rte_pktmbuf_free(&taken);
    assert(rte_mbuf_refcnt_read(&taken) == 1);
    dppd_nd6_discard_pending(NULL);
}

/** 更换策略的失败路径不得释放旧等待包，成功才重做 DAD，邻居 ID 和容器地址始终不复用 */
static void policy_replacement(void)
{
    struct dppd_nd6_policy enabled = policy(), disabled = {0};
    struct dppd_nd6 *table = create_policy(&disabled), *stable = table;
    struct dppd_nd6_status before, after;
    struct capture output = {0};
    dppd_nd6_status(table, &before);
    assert(!before.enabled && before.epoch != 0 && before.revision == 1 && before.local_count == 0);
    struct dppd_packet control = {.l3_type = DPPD_L3_IPV6, .ip_protocol = 58};
    assert(!dppd_nd6_input(table, 0, NULL, &control, 1000, capture, &output));
    assert(dppd_nd6_port_ready(table, 0));
    uint64_t other_epoch = before.epoch == UINT64_MAX ? 1 : before.epoch + 1;
    assert(dppd_nd6_replace(table, &enabled, false, 0, before.revision, 1000, NULL, NULL) == -EINVAL);
    assert(dppd_nd6_replace(table, &enabled, false, other_epoch, before.revision, 1000, NULL, NULL) == -ESTALE);
    struct publication publication = {0};
    assert(dppd_nd6_replace(table, &enabled, false, before.epoch, before.revision, 1000,
               publish_policy, &publication) == 0 && publication.calls == 1 && table == stable);
    assert(!dppd_nd6_port_ready(table, 0));
    dppd_nd6_tick(table, 2001, capture, &output);
    dppd_nd6_tick(table, 3001, capture, &output);
    assert(dppd_nd6_port_ready(table, 0) && dppd_nd6_port_ready(table, 1));
    uint8_t target[16], mac[6];
    address(target, "fe80::1");
    struct dppd_packet packet = {0};
    assert(dppd_nd6_resolve(table, 1, target, &packet, 3010, mac) == -EAGAIN);
    uint64_t old_id = entry(table).id;
    struct rte_mbuf queued = referenced_packet();
    dppd_nd6_status(table, &before);
    assert(dppd_nd6_hold(table, 1, target, (struct dppd_nd6_pending){.mbuf = &queued, .queue = 1}, 3010) == 0);
    assert(dppd_nd6_flush_guarded(table, before.epoch, before.revision) == -ESTALE);
    dppd_nd6_status(table, &before);
    struct dppd_nd6_page page;
    assert(dppd_nd6_list_guarded(table, 0, 0, 0, &page) == 0 && page.epoch == before.epoch && page.count == 1);
    assert(dppd_nd6_list_guarded(table, page.next_id, 0, 0, &page) == -EINVAL);
    assert(dppd_nd6_list_guarded(table, 0, other_epoch, before.revision, &page) == -ESTALE);
    assert(dppd_nd6_replace(table, &disabled, false, before.epoch, before.revision, 3020,
               publish_policy, &publication) == -EBUSY && publication.calls == 1);
    enabled.reachable_ms = 2000;
    enabled.locals[1].address[15] = 0x21;
    fail_allocation = 1;
    assert(dppd_nd6_replace(table, &enabled, true, before.epoch, before.revision, 3020,
               publish_policy, &publication) == -ENOMEM && publication.calls == 1);
    publication.result = -EIO;
    assert(dppd_nd6_replace(table, &enabled, true, before.epoch, before.revision, 3020,
               publish_policy, &publication) == -EIO && publication.calls == 2);
    dppd_nd6_status(table, &after);
    assert(after.revision == before.revision && after.pending == 1 && after.active == 1 &&
        after.locals[1].configuration.address[15] == 0x20 && rte_mbuf_refcnt_read(&queued) == 2);
    publication.result = 0;
    assert(dppd_nd6_replace(table, &enabled, true, before.epoch, before.revision, 3020,
               publish_policy, &publication) == 0 && publication.calls == 3);
    dppd_nd6_status(table, &after);
    assert(after.epoch == before.epoch && after.revision == before.revision + 1 && after.pending == 0 &&
        after.active == 0 && after.queue_drops == 1 && after.solicitations == before.solicitations &&
        after.reachable_ms == 2000 && after.locals[1].state == DPPD_ND6_TENTATIVE &&
        after.locals[1].configuration.address[15] == 0x21 && rte_mbuf_refcnt_read(&queued) == 1);
    struct dppd_nd6_pending returned;
    assert(!dppd_nd6_take(table, 1, 3030, &returned));
    assert(dppd_nd6_list_guarded(table, 0, before.epoch, before.revision, &page) == -ESTALE);
    dppd_nd6_tick(table, 4021, capture, &output);
    dppd_nd6_tick(table, 5021, capture, &output);
    assert(dppd_nd6_resolve(table, 1, target, &packet, 5030, mac) == -EAGAIN && entry(table).id > old_id);
    struct rte_mbuf taken = referenced_packet();
    assert(dppd_nd6_hold(table, 1, target, (struct dppd_nd6_pending){.mbuf = &taken}, 5030) == 0);
    assert(dppd_nd6_take(table, 0, 5130, &returned) && returned.mbuf == &taken && !returned.expired);
    dppd_nd6_status(table, &before);
    assert(dppd_nd6_flush_guarded(table, before.epoch, before.revision) == 0);
    assert(rte_mbuf_refcnt_read(&taken) == 2);
    rte_pktmbuf_free(&taken);
    assert(rte_mbuf_refcnt_read(&taken) == 1);
    dppd_nd6_status(table, &before);
    assert(dppd_nd6_replace(table, &disabled, false, before.epoch, before.revision, 5200, NULL, NULL) == 0);
    dppd_nd6_status(table, &after);
    assert(!after.enabled && after.local_count == 0 && after.queue_drops == 1 && dppd_nd6_port_ready(table, 1));
    assert(!dppd_nd6_input(table, 0, NULL, &control, 5200, capture, &output));
    struct dppd_nd6 *restarted = create_policy(&enabled);
    dppd_nd6_status(restarted, &after);
    assert(after.epoch != before.epoch && after.revision == 1);
    assert(dppd_nd6_flush_guarded(restarted, before.epoch, after.revision) == -ESTALE);
    dppd_nd6_destroy(restarted);
    dppd_nd6_destroy(table);
}

/** 精确清缓存立即归还等待包，但本机地址冲突必须通过新策略重新执行 DAD 才能解除 */
static void guarded_flush(void)
{
    struct capture output = {0};
    struct dppd_nd6 *table = create(0, &output);
    struct dppd_packet packet = {0};
    uint8_t target[16], mac[6];
    address(target, "fe80::1");
    assert(dppd_nd6_resolve(table, 1, target, &packet, 3010, mac) == -EAGAIN);
    struct rte_mbuf queued = referenced_packet();
    assert(dppd_nd6_hold(table, 1, target, (struct dppd_nd6_pending){.mbuf = &queued}, 3010) == 0);
    struct dppd_nd6_status status;
    dppd_nd6_status(table, &status);
    assert(dppd_nd6_flush_guarded(table, 0, status.revision) == -EINVAL);
    assert(dppd_nd6_flush_guarded(table, status.epoch, status.revision) == 0);
    assert(rte_mbuf_refcnt_read(&queued) == 1 && dppd_nd6_port_ready(table, 1));
    dppd_nd6_status(table, &status);
    assert(status.pending == 0 && status.active == 0 && status.queue_drops == 1);
    struct dppd_nd6_policy p = policy();
    assert(dppd_nd6_replace(table, &p, false, status.epoch, status.revision, 4000, NULL, NULL) == 0);
    uint8_t bytes[256];
    uint16_t length = frame(bytes, 0, 136, "fe80::1", "fe80::20", "fe80::20", 0x20, true);
    assert(input(table, 0, 1, bytes, length, 4001, &output));
    dppd_nd6_status(table, &status);
    assert(status.locals[1].state == DPPD_ND6_DUPLICATE);
    assert(dppd_nd6_flush_guarded(table, status.epoch, status.revision) == 0 && !dppd_nd6_port_ready(table, 1));
    dppd_nd6_status(table, &status);
    assert(status.locals[1].state == DPPD_ND6_DUPLICATE);
    dppd_nd6_destroy(table);
}

static void policy_validation(void)
{
    struct dppd_nd6_policy p = policy();
    assert(dppd_nd6_policy_validate(&p) == 0);
    p.locals[0].vlan[1] = 65535;
    p.locals[31].port_id = UINT16_MAX;
    assert(dppd_nd6_policy_validate(&p) == 0);
    p.reachable_ms = 999;
    assert(dppd_nd6_policy_validate(&p) == -EINVAL);
    p = policy();
    p.locals[1] = p.locals[0];
    assert(dppd_nd6_policy_validate(&p) == -EINVAL);
    p.locals[1].address[15]++;
    p.locals[1].vlan_depth = 1;
    p.locals[1].vlan[0] = 37;
    assert(dppd_nd6_policy_validate(&p) == -EINVAL);
    p = policy();
    p.locals[0].address[0] = 255;
    assert(dppd_nd6_policy_validate(&p) == -EINVAL);
    p = (struct dppd_nd6_policy){.reachable_ms = 1000};
    assert(dppd_nd6_policy_validate(&p) == -EINVAL);
    p.reachable_ms = 0;
    assert(dppd_nd6_policy_validate(&p) == 0);
    struct dppd_nd6 *table = NULL;
    struct dppd_nd6_port port = {.port_id = 0, .mac = {2, 0, 0, 0, 0, 10}};
    p = policy();
    assert(dppd_nd6_create_policy(&table, &p, &port, 1, 1000) == -ENODEV && table == NULL);
    port.mac[0] = 1;
    assert(dppd_nd6_create_policy(&table, &p, &port, 1, 1000) == -EINVAL && table == NULL);
    /** 旧入口没有给端口 1 启用 ND，不应因该口 MAC 为零而拒绝端口 0 的合法配置 */
    struct dppd_config configuration;
    dppd_config_defaults(&configuration);
    strcpy(configuration.routing_path, "/private/routes");
    configuration.nd6_count = 1;
    configuration.nd6[0] = policy().locals[0];
    uint8_t partial_macs[DPPD_MAX_PORTS][6] = {{0}};
    memcpy(partial_macs[0], macs[0], 6);
    assert(dppd_nd6_create(&table, &configuration, (const uint8_t(*)[6])partial_macs, 1000) == 0 && table != NULL);
    dppd_nd6_destroy(table);
}

static void *observe_policy(void *context)
{
    struct dppd_nd6 *table = context;
    for (unsigned int i = 0; i < 1000; ++i) {
        struct dppd_nd6_status status;
        dppd_nd6_status(table, &status);
        assert((status.enabled && status.local_count == 2 && status.reachable_ms == 1000) ||
            (!status.enabled && status.local_count == 0 && status.reachable_ms == 0));
        struct dppd_nd6_page page;
        int rc = dppd_nd6_list_guarded(table, 0, status.epoch, status.revision, &page);
        assert(rc == 0 || rc == -ESTALE);
    }
    return NULL;
}

/** 运行态查询必须得到完整旧策略或完整新策略，不能混读 locals 数量和计时参数 */
static void concurrent_observers(void)
{
    struct dppd_nd6_policy enabled = policy(), disabled = {0};
    struct dppd_nd6 *table = create_policy(&disabled);
    pthread_t reader;
    assert(pthread_create(&reader, NULL, observe_policy, table) == 0);
    for (unsigned int i = 0; i < 100; ++i) {
        struct dppd_nd6_status status;
        dppd_nd6_status(table, &status);
        assert(dppd_nd6_replace(table, i % 2 ? &disabled : &enabled, false, status.epoch, status.revision,
                   1000 + i, NULL, NULL) == 0);
    }
    assert(pthread_join(reader, NULL) == 0);
    dppd_nd6_destroy(table);
}

int main(void)
{
    for (unsigned int vlan = 0; vlan <= 2; ++vlan)
        lifecycle(vlan);
    invalid_packets();
    local_neighbors();
    overrides_and_bounds();
    policy_validation();
    policy_replacement();
    guarded_flush();
    discard_pending();
    concurrent_observers();
    return 0;
}
