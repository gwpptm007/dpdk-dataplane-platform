#include "dppd/arp.h"

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <rte_mbuf.h>

static const struct dppd_arp_port port = {.port_id = 7, .mac = {2, 0, 0, 0, 0, 7}};
static const uint8_t peer[6] = {2, 0, 0, 0, 0, 9}, address[4] = {192, 0, 2, 9};
static unsigned int probes, announcements, requests, replies;
static bool fail_send;

/** 独立按线缆偏移检查探测和通告，不能只比较引擎自己的状态计数 */
static int capture(void *context, uint16_t id, const uint8_t *wire, uint16_t length)
{
    (void)context;
    assert(id == 7 && length == 60 && memcmp(wire + 6, port.mac, 6) == 0);
    assert(wire[12] == 8 && wire[13] == 6);
    const uint8_t *body = wire + 14;
    assert(memcmp(body, (uint8_t[]){0, 1, 8, 0, 6, 4, 0}, 7) == 0);
    if (fail_send)
        return -EIO;
    if (body[7] == 2)
        replies++;
    else if (memcmp(body + 14, (uint8_t[4]){0}, 4) == 0)
        probes++;
    else if (memcmp(body + 14, body + 24, 4) == 0)
        announcements++;
    else
        requests++;
    return 0;
}

static struct dppd_arp_policy policy(void)
{
    return (struct dppd_arp_policy){.local_count = 1, .reachable_ms = 1000,
        .locals = {{.port_id = 7, .address = {192, 0, 2, 1}}}};
}

static struct dppd_arp_status status(struct dppd_arp *table)
{
    struct dppd_arp_status value;
    dppd_arp_status(table, &value);
    return value;
}

static struct dppd_arp *create(void)
{
    struct dppd_arp *table = NULL;
    struct dppd_arp_policy value = policy();
    assert(dppd_arp_create_policy(&table, &value, &port, 1, 0) == 0);
    assert(!dppd_arp_port_ready(table, 7));
    for (uint64_t now = 1000; now <= 10000; now += 1000)
        dppd_arp_tick(table, now, capture, NULL);
    assert(dppd_arp_port_ready(table, 7));
    return table;
}

static void input(struct dppd_arp *table, uint8_t wire[60], uint64_t now)
{
    struct rte_mbuf packet = {.buf_addr = wire, .data_len = 60, .pkt_len = 60, .nb_segs = 1};
    struct dppd_packet parsed;
    assert(dppd_packet_parse_mbuf(&packet, &parsed) == DPPD_PARSE_OK);
    assert(dppd_arp_input(table, 7, &packet, &parsed, now, capture, NULL));
}

static void response(uint8_t wire[60])
{
    memset(wire, 0, 60);
    memcpy(wire, port.mac, 6);
    memcpy(wire + 6, peer, 6);
    memcpy(wire + 12, (uint8_t[]){8, 6, 0, 1, 8, 0, 6, 4, 0, 2}, 10);
    memcpy(wire + 22, peer, 6);
    memcpy(wire + 28, address, 4);
    memcpy(wire + 32, port.mac, 6);
    memcpy(wire + 38, (uint8_t[]){192, 0, 2, 1}, 4);
}

static int reject_publish(void *context)
{
    (void)context;
    return -EIO;
}

static void lifecycle(void)
{
    probes = announcements = requests = replies = 0;
    struct dppd_arp *table = create();
    assert(probes == 3 && announcements == 2);
    struct dppd_packet packet = {0};
    uint8_t mac[6], wire[60];
    response(wire);
    input(table, wire, 10001);
    assert(status(table).active == 0);
    assert(dppd_arp_resolve(table, 7, address, &packet, 10002, mac) == -EAGAIN);
    struct rte_mbuf queued = {.nb_segs = 1};
    rte_mbuf_refcnt_set(&queued, 2);
    struct dppd_arp_status before = status(table);
    assert(dppd_arp_hold(table, 7, address, (struct dppd_arp_pending){.mbuf = &queued, .queue = 2}, 10002) == 0);
    assert(dppd_arp_flush_guarded(table, before.epoch, before.revision) == -ESTALE);
    before = status(table);
    struct dppd_arp_policy value = policy();
    assert(dppd_arp_replace(table, &value, false, before.epoch, before.revision, 10003, NULL, NULL) == -EBUSY);
    assert(dppd_arp_replace(table, &value, true, before.epoch, before.revision, 10003, reject_publish, NULL) == -EIO);
    assert(rte_mbuf_refcnt_read(&queued) == 2 && status(table).pending == 1);
    wire[22] ^= 2;
    input(table, wire, 10004);
    assert(status(table).resolved == 0 && status(table).invalid == 1);
    response(wire);
    input(table, wire, 10005);
    assert(dppd_arp_resolve(table, 7, address, &packet, 10006, mac) == 0 && memcmp(mac, peer, 6) == 0);
    struct dppd_arp_pending taken;
    assert(!dppd_arp_take(table, 1, 10006, &taken));
    assert(dppd_arp_take(table, 2, 10006, &taken) && !taken.expired && taken.mbuf == &queued);
    rte_pktmbuf_free(taken.mbuf);
    assert(rte_mbuf_refcnt_read(&queued) == 1);
    assert(dppd_arp_resolve(table, 7, address, &packet, 12000, mac) == 0);
    for (uint64_t now = 17000; now <= 20000; now += 1000)
        dppd_arp_tick(table, now, capture, NULL);
    assert(requests == 3 && status(table).failed == 1);
    assert(dppd_arp_resolve(table, 7, address, &packet, 20001, mac) == -EHOSTUNREACH);
    assert(dppd_arp_resolve(table, 7, address, &packet, 25001, mac) == -EAGAIN);
    assert(dppd_arp_hold(table, 7, address, (struct dppd_arp_pending){.mbuf = &queued}, 25001) == 0);
    rte_mbuf_refcnt_set(&queued, 2);
    dppd_arp_discard_pending(table);
    dppd_arp_discard_pending(table);
    assert(status(table).pending == 0 && rte_mbuf_refcnt_read(&queued) == 1);
    dppd_arp_destroy(table);
}

static void conflict_and_guards(void)
{
    struct dppd_arp *table = create();
    uint8_t wire[60];
    response(wire);
    wire[21] = 1;
    input(table, wire, 10001);
    assert(replies != 0);
    memcpy(wire + 28, wire + 38, 4);
    input(table, wire, 10002);
    assert(!dppd_arp_port_ready(table, 7) && status(table).locals[0].state == DPPD_ARP_DUPLICATE);
    struct dppd_arp_status old = status(table);
    assert(dppd_arp_flush_guarded(table, old.epoch, old.revision) == 0);
    assert(!dppd_arp_port_ready(table, 7));
    struct dppd_arp_page page;
    assert(dppd_arp_list_guarded(table, 0, old.epoch, old.revision, &page) == -ESTALE);
    old = status(table);
    struct dppd_arp_policy disabled = {0};
    assert(dppd_arp_replace(table, &disabled, true, old.epoch, old.revision, 10003, NULL, NULL) == 0);
    struct dppd_packet packet = {.l3_type = DPPD_L3_ARP};
    assert(!dppd_arp_input(table, 7, NULL, &packet, 10004, capture, NULL));
    assert(dppd_arp_port_ready(table, 7));
    dppd_arp_destroy(table);
    struct dppd_arp_policy invalid = policy();
    invalid.locals[0].address[0] = 224;
    assert(dppd_arp_policy_validate(&invalid) == -EINVAL);
    invalid = policy();
    invalid.local_count = 2;
    invalid.locals[1] = invalid.locals[0];
    assert(dppd_arp_policy_validate(&invalid) == -EINVAL);
}

/** 发包失败不能提前启用地址，容量和分页上限也不能靠超时碰巧满足 */
static void failure_and_capacity(void)
{
    struct dppd_arp *table = NULL;
    struct dppd_arp_policy value = policy();
    assert(dppd_arp_create_policy(&table, &value, &port, 1, 0) == 0);
    fail_send = true;
    for (uint64_t now = 1000; now <= 10000; now += 1000)
        dppd_arp_tick(table, now, capture, NULL);
    assert(!dppd_arp_port_ready(table, 7) && status(table).tx_errors == 10);
    fail_send = false;
    probes = announcements = 0;
    for (uint64_t now = 11000; now <= 21000; now += 1000)
        dppd_arp_tick(table, now, capture, NULL);
    assert(dppd_arp_port_ready(table, 7) && probes == 3 && announcements == 2);
    struct dppd_packet packet = {0};
    uint8_t destination[4] = {198, 51, 0, 1}, mac[6];
    for (uint32_t i = 0; i < DPPD_ARP_LIMIT; ++i) {
        destination[2] = i / 254;
        destination[3] = 1 + i % 254;
        assert(dppd_arp_resolve(table, 7, destination, &packet, 21001, mac) == -EAGAIN);
    }
    destination[2] = 9;
    assert(dppd_arp_resolve(table, 7, destination, &packet, 21001, mac) == -ENOSPC);
    assert(status(table).active == DPPD_ARP_LIMIT);
    struct dppd_arp_page page;
    struct dppd_arp_status snapshot = status(table);
    uint64_t after = 0;
    uint32_t seen = 0;
    do {
        assert(dppd_arp_list_guarded(table, after, snapshot.epoch, snapshot.revision, &page) == 0);
        assert(page.count == DPPD_ARP_PAGE_SIZE);
        for (uint32_t i = 0; i < page.count; ++i)
            assert(page.entries[i].id == ++seen);
        after = page.next_id;
    } while (page.more);
    assert(seen == DPPD_ARP_LIMIT);
    assert(dppd_arp_flush_guarded(table, snapshot.epoch, snapshot.revision) == 0);
    assert(dppd_arp_list_guarded(table, after, snapshot.epoch, snapshot.revision, &page) == -ESTALE);
    dppd_arp_destroy(table);
}

static int capture_vlan(void *context, uint16_t id, const uint8_t *wire, uint16_t length)
{
    unsigned int *sent = context;
    assert(id == 7 && length == 60 && memcmp(wire + 12, (uint8_t[]){0x88, 0xa8, 0, 37, 0x81, 0, 0, 38, 8, 6}, 10) == 0);
    (*sent)++;
    return 0;
}

/** 双标签既影响发出的 ARP，也限制可学习的应答，不能只校验配置文本 */
static void vlan_domain(void)
{
    struct dppd_arp *table = NULL;
    struct dppd_arp_policy value = policy();
    value.locals[0].vlan_depth = 2;
    value.locals[0].vlan[0] = 37;
    value.locals[0].vlan[1] = 38;
    assert(dppd_arp_create_policy(&table, &value, &port, 1, 0) == 0);
    unsigned int sent = 0;
    for (uint64_t now = 1000; now <= 10000; now += 1000)
        dppd_arp_tick(table, now, capture_vlan, &sent);
    assert(sent == 5 && dppd_arp_port_ready(table, 7));
    struct dppd_packet packet = {.vlan_depth = 2, .vlan_tpid = {0x88a8, 0x8100}, .vlan_tci = {37, 38}};
    uint8_t mac[6], wire[60];
    assert(dppd_arp_resolve(table, 7, address, &packet, 10001, mac) == -EAGAIN);
    response(wire);
    memmove(wire + 22, wire + 14, 28);
    memcpy(wire + 12, (uint8_t[]){0x88, 0xa8, 0, 37, 0x81, 0, 0, 38, 8, 6}, 10);
    struct rte_mbuf mbuf = {.buf_addr = wire, .data_len = 60, .pkt_len = 60, .nb_segs = 1};
    struct dppd_packet parsed;
    wire[19] = 39;
    assert(dppd_packet_parse_mbuf(&mbuf, &parsed) == DPPD_PARSE_OK);
    assert(dppd_arp_input(table, 7, &mbuf, &parsed, 10002, capture_vlan, &sent));
    assert(status(table).invalid == 1 && status(table).resolved == 0);
    wire[19] = 38;
    assert(dppd_packet_parse_mbuf(&mbuf, &parsed) == DPPD_PARSE_OK);
    assert(dppd_arp_input(table, 7, &mbuf, &parsed, 10003, capture_vlan, &sent));
    assert(dppd_arp_resolve(table, 7, address, &packet, 10004, mac) == 0 && memcmp(mac, peer, 6) == 0);
    dppd_arp_destroy(table);
}

int main(void)
{
    lifecycle();
    conflict_and_guards();
    failure_and_capacity();
    vlan_domain();
    return 0;
}
