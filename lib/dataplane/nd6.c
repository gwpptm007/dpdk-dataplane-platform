#include "dppd/nd6.h"
#include "dppd/ipv6.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <rte_mbuf.h>

struct local_address {
    struct dppd_nd6_local configuration;
    uint8_t mac[6], state;
    bool sent;
    uint64_t due;
};
struct waiting_packet {
    struct dppd_nd6_pending packet;
    uint64_t neighbor_id, retry_ms;
};
/** 缓存、状态机和等待队列共用锁，跨 RX 队列不能重复创建邻居或重复交付首包 */
struct dppd_nd6 {
    pthread_mutex_t lock;
    struct dppd_nd6_status status;
    struct local_address locals[DPPD_ND6_LOCAL_LIMIT];
    struct dppd_nd6_entry entries[DPPD_ND6_LIMIT];
    struct waiting_packet waiting[DPPD_ND6_PENDING_LIMIT];
    uint64_t clock, next_tick, next_id;
    uint32_t random;
};
static const uint8_t zero[16] = {0};

static bool equal(const uint8_t a[16], const uint8_t b[16])
{
    return memcmp(a, b, 16) == 0;
}
static bool mac_valid(const uint8_t mac[6])
{
    return (mac[0] & 1U) == 0 && memcmp(mac, zero, 6) != 0;
}
static uint64_t clock_advance(struct dppd_nd6 *table, uint64_t now)
{
    if (now > table->clock)
        table->clock = now;
    return table->clock;
}
static uint32_t random_value(struct dppd_nd6 *table)
{
    table->random ^= table->random << 13;
    table->random ^= table->random >> 17;
    table->random ^= table->random << 5;
    return table->random;
}
static struct local_address *local(struct dppd_nd6 *table, uint16_t port, const uint8_t *address)
{
    for (uint32_t i = 0; i < table->status.local_count; ++i)
        if (table->locals[i].configuration.port_id == port &&
            (address == NULL || equal(table->locals[i].configuration.address, address)))
            return &table->locals[i];
    return NULL;
}
static bool domain(const struct local_address *address, const struct dppd_packet *packet)
{
    return address != NULL && packet->vlan_depth == address->configuration.vlan_depth &&
        (packet->vlan_depth < 1 || packet->vlan_tpid[0] == (packet->vlan_depth == 2 ? 0x88a8 : 0x8100)) &&
        (packet->vlan_depth < 2 || packet->vlan_tpid[1] == 0x8100) &&
        (packet->vlan_depth < 1 || (packet->vlan_tci[0] & 4095U) == address->configuration.vlan[0]) &&
        (packet->vlan_depth < 2 || (packet->vlan_tci[1] & 4095U) == address->configuration.vlan[1]);
}
static struct dppd_nd6_entry *find(struct dppd_nd6 *table, uint16_t port, const uint8_t address[16])
{
    for (uint32_t i = 0; i < DPPD_ND6_LIMIT; ++i)
        if (table->entries[i].id != 0 && table->entries[i].port_id == port && equal(table->entries[i].address, address))
            return &table->entries[i];
    return NULL;
}
static void state(struct dppd_nd6 *table, struct dppd_nd6_entry *entry, uint8_t value, uint64_t due)
{
    entry->state = value;
    entry->changed_ms = table->clock;
    entry->due_ms = due;
    table->status.revision++;
}
static struct dppd_nd6_entry *insert(struct dppd_nd6 *table, uint16_t port, const uint8_t address[16])
{
    if (table->next_id == UINT64_MAX)
        return NULL;
    for (uint32_t i = 0; i < DPPD_ND6_LIMIT; ++i)
        if (table->entries[i].id == 0) {
            struct dppd_nd6_entry *entry = &table->entries[i];
            *entry = (struct dppd_nd6_entry){.id = ++table->next_id, .port_id = port, .used_ms = table->clock};
            memcpy(entry->address, address, 16);
            state(table, entry, DPPD_ND6_INCOMPLETE, table->clock);
            table->status.active++;
            return entry;
        }
    return NULL;
}
static void solicited(const uint8_t target[16], uint8_t destination[16])
{
    memcpy(destination, (uint8_t[]){255, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 255, 0, 0, 0}, 16);
    memcpy(destination + 13, target + 13, 3);
}
static uint32_t sum(uint32_t value, const uint8_t *bytes, uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i)
        value += (uint32_t)bytes[i] << (i % 2 == 0 ? 8 : 0);
    while (value >> 16)
        value = (value & 65535U) + (value >> 16);
    return value;
}
/** ND 的伪首部允许链路本地和组播，不能复用只允许转发普通单播的 TCP/UDP 检查 */
static uint16_t checksum(const uint8_t source[16], const uint8_t destination[16], const uint8_t *body, uint32_t length)
{
    return (uint16_t)~sum(sum(sum(0, source, 16), destination, 16) + 58 + length, body, length);
}
static void put16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}
/** 控制报文单独分配，不修改收到的共享 mbuf，也不继承数据流的 NAT 和 TX 卸载标记 */
static int transmit(struct dppd_nd6 *table, const struct local_address *owner, uint8_t type, const uint8_t source[16],
    const uint8_t destination[16], const uint8_t target[16], const uint8_t *destination_mac, uint8_t flags,
    dppd_nd6_send send, void *context)
{
    uint8_t wire[110] = {0};
    unsigned int l2 = 14 + 4 * owner->configuration.vlan_depth;
    if (destination_mac != NULL)
        memcpy(wire, destination_mac, 6);
    else {
        wire[0] = wire[1] = 0x33;
        memcpy(wire + 2, destination + 12, 4);
    }
    memcpy(wire + 6, owner->mac, 6);
    for (unsigned int i = 0; i < owner->configuration.vlan_depth; ++i) {
        put16(wire + 12 + i * 4, owner->configuration.vlan_depth == 2 && i == 0 ? 0x88a8 : 0x8100);
        put16(wire + 14 + i * 4, owner->configuration.vlan[i]);
    }
    put16(wire + l2 - 2, 0x86dd);
    uint8_t *ip = wire + l2, *body = ip + 40;
    unsigned int length = equal(source, zero) ? 24 : 32;
    ip[0] = 0x60;
    put16(ip + 4, (uint16_t)length);
    ip[6] = 58;
    ip[7] = 255;
    memcpy(ip + 8, source, 16);
    memcpy(ip + 24, destination, 16);
    body[0] = type;
    body[4] = flags;
    memcpy(body + 8, target, 16);
    if (length == 32) {
        body[24] = type == 135 ? 1 : 2;
        body[25] = 1;
        memcpy(body + 26, owner->mac, 6);
    }
    put16(body + 2, checksum(source, destination, body, length));
    int rc = send == NULL ? -ENOSYS : send(context, owner->configuration.port_id, wire, (uint16_t)(l2 + 40 + length));
    if (rc != 0)
        table->status.tx_errors++;
    else if (type == 135)
        table->status.solicitations++;
    else
        table->status.advertisements++;
    return rc;
}

int dppd_nd6_create(struct dppd_nd6 **result, const struct dppd_config *configuration,
    const uint8_t macs[DPPD_MAX_PORTS][6], uint64_t now)
{
    if (result == NULL || *result != NULL || configuration == NULL || macs == NULL)
        return -EINVAL;
    if (configuration->nd6_count == 0)
        return 0;
    char error[128];
    if (dppd_config_validate(configuration, error, sizeof(error)) != 0)
        return -EINVAL;
    struct dppd_nd6 *table = calloc(1, sizeof(*table));
    if (table == NULL)
        return -ENOMEM;
    if (getrandom(&table->random, sizeof(table->random), 0) != sizeof(table->random) ||
        pthread_mutex_init(&table->lock, NULL) != 0) {
        free(table);
        return -EIO;
    }
    if (table->random == 0)
        table->random = 1;
    table->clock = now;
    table->status = (struct dppd_nd6_status){.enabled = true,
        .revision = 1,
        .local_count = configuration->nd6_count,
        .reachable_ms = configuration->nd6_reachable_ms};
    for (uint32_t i = 0; i < configuration->nd6_count; ++i) {
        struct local_address *owner = &table->locals[i];
        owner->configuration = configuration->nd6[i];
        for (uint16_t j = 0; j < configuration->nb_ports; ++j)
            if (configuration->ports[j] == owner->configuration.port_id)
                memcpy(owner->mac, macs[j], 6);
        if (!mac_valid(owner->mac)) {
            dppd_nd6_destroy(table);
            return -EINVAL;
        }
        owner->due = now + random_value(table) % 1001;
    }
    *result = table;
    return 0;
}
void dppd_nd6_destroy(struct dppd_nd6 *table)
{
    if (table == NULL)
        return;
    for (uint32_t i = 0; i < DPPD_ND6_PENDING_LIMIT; ++i)
        if (table->waiting[i].packet.mbuf != NULL)
            rte_pktmbuf_free(table->waiting[i].packet.mbuf);
    pthread_mutex_destroy(&table->lock);
    free(table);
}
bool dppd_nd6_port_ready(struct dppd_nd6 *table, uint16_t port)
{
    if (table == NULL)
        return true;
    pthread_mutex_lock(&table->lock);
    bool ready = true;
    for (uint32_t i = 0; i < table->status.local_count; ++i)
        if (table->locals[i].configuration.port_id == port && table->locals[i].state != DPPD_ND6_PREFERRED)
            ready = false;
    pthread_mutex_unlock(&table->lock);
    return ready;
}

/** NS 仅学习发给本机的源地址，NA 仅更新已有候选，未知通告不能灌满缓存 */
bool dppd_nd6_input(struct dppd_nd6 *table, uint16_t port, const struct rte_mbuf *mbuf,
    const struct dppd_packet *packet, uint64_t now, dppd_nd6_send send, void *context)
{
    if (table == NULL || packet->l3_type != DPPD_L3_IPV6 || packet->ip_protocol != 58)
        return false;
    uint8_t first[1], ether[22], hop[1], body[256];
    uint32_t offset = packet->l2_len + packet->l3_len;
    const uint8_t *kind = rte_pktmbuf_read(mbuf, offset, 1, first);
    if (packet->l4_type != DPPD_L4_FRAGMENT && (kind == NULL || *kind < 133 || *kind > 137))
        return false;
    pthread_mutex_lock(&table->lock);
    clock_advance(table, now);
    struct local_address *owner = local(table, port, NULL);
    if (packet->l2_len < 14 || packet->l2_len > 22 || packet->l3_len < 40 ||
        packet->ipv6_payload_len < packet->l3_len - 40)
        goto invalid;
    uint32_t length = 40U + packet->ipv6_payload_len - packet->l3_len;
    const uint8_t *link = rte_pktmbuf_read(mbuf, 0, packet->l2_len, ether);
    const uint8_t *ttl = rte_pktmbuf_read(mbuf, packet->l2_len + 7, 1, hop);
    const uint8_t *icmp = length <= sizeof(body) ? rte_pktmbuf_read(mbuf, offset, length, body) : NULL;
    if (!domain(owner, packet) || packet->l2_len > sizeof(ether) || link == NULL || ttl == NULL || *ttl != 255 ||
        packet->ipv6_unsupported || packet->l4_type == DPPD_L4_FRAGMENT || icmp == NULL || length < 24 ||
        (icmp[0] != 135 && icmp[0] != 136) || icmp[1] != 0 || !mac_valid(link + 6) ||
        !dppd_ipv6_unicast(icmp + 8, true) || checksum(packet->ipv6_src, packet->ipv6_dst, icmp, length) != 0)
        goto invalid;
    if (memcmp(link + 6, owner->mac, 6) == 0)
        goto consumed;
    bool ns = icmp[0] == 135, unspecified = equal(packet->ipv6_src, zero);
    uint8_t group[16];
    solicited(icmp + 8, group);
    struct local_address *target = local(table, port, icmp + 8);
    bool multicast = packet->ipv6_dst[0] == 255;
    const uint8_t all_nodes[16] = {255, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if ((!unspecified && !dppd_ipv6_unicast(packet->ipv6_src, true)) ||
        (!ns && (unspecified || (multicast && (icmp[4] & 0x40U) != 0))) ||
        (ns && (multicast ? !equal(packet->ipv6_dst, group) : !equal(packet->ipv6_dst, icmp + 8))) ||
        (!ns && multicast && !equal(packet->ipv6_dst, all_nodes)) ||
        (!multicast && local(table, port, packet->ipv6_dst) == NULL) ||
        (multicast ? link[0] != 0x33 || link[1] != 0x33 || memcmp(link + 2, packet->ipv6_dst + 12, 4) != 0
                   : memcmp(link, owner->mac, 6) != 0) ||
        (ns && unspecified && !multicast))
        goto invalid;
    const uint8_t *option_mac = NULL;
    for (uint32_t option = 24; option < length;) {
        if (length - option < 2 || icmp[option + 1] == 0 || (uint32_t)icmp[option + 1] * 8 > length - option)
            goto invalid;
        if (icmp[option] == 1 || icmp[option] == 2) {
            if (icmp[option] != (ns ? 1 : 2) || icmp[option + 1] != 1 || option_mac != NULL)
                goto invalid;
            option_mac = icmp + option + 2;
            if (!mac_valid(option_mac) || memcmp(option_mac, link + 6, 6) != 0)
                goto invalid;
        }
        option += (uint32_t)icmp[option + 1] * 8;
    }
    if (ns && unspecified && option_mac != NULL)
        goto invalid;
    if (target != NULL && ((!ns && !unspecified) || (ns && unspecified && target->state == DPPD_ND6_TENTATIVE))) {
        if (target->state == DPPD_ND6_TENTATIVE) {
            target->state = DPPD_ND6_DUPLICATE;
            table->status.revision++;
        } else
            table->status.invalid++;
        goto consumed;
    }
    if (ns) {
        if (target == NULL || target->state != DPPD_ND6_PREFERRED)
            goto consumed;
        if (!unspecified && option_mac != NULL && local(table, port, packet->ipv6_src) == NULL) {
            struct dppd_nd6_entry *entry = find(table, port, packet->ipv6_src);
            if (entry == NULL)
                entry = insert(table, port, packet->ipv6_src);
            if (entry != NULL && (memcmp(entry->mac, option_mac, 6) != 0 || entry->state == DPPD_ND6_FAILED)) {
                memcpy(entry->mac, option_mac, 6);
                state(table, entry, DPPD_ND6_STALE, 0);
            }
        }
        transmit(table, target, 136, target->configuration.address, unspecified ? all_nodes : packet->ipv6_src,
            icmp + 8, unspecified ? NULL : link + 6, unspecified ? 0xa0 : 0xe0, send, context);
    } else {
        struct dppd_nd6_entry *entry = find(table, port, icmp + 8);
        if (entry == NULL || (option_mac == NULL && !mac_valid(entry->mac)))
            goto consumed;
        bool changed = option_mac != NULL && memcmp(entry->mac, option_mac, 6) != 0;
        if (entry->state != DPPD_ND6_INCOMPLETE && changed && (icmp[4] & 0x20U) == 0) {
            if (entry->state == DPPD_ND6_REACHABLE)
                state(table, entry, DPPD_ND6_STALE, 0);
            goto consumed;
        }
        if (option_mac != NULL)
            memcpy(entry->mac, option_mac, 6);
        if ((icmp[4] & 0x40U) != 0) {
            uint32_t base = table->status.reachable_ms;
            state(table, entry, DPPD_ND6_REACHABLE, table->clock + base / 2 + random_value(table) % (base + 1));
            table->status.resolved++;
        } else if (changed || entry->state == DPPD_ND6_INCOMPLETE)
            state(table, entry, DPPD_ND6_STALE, 0);
    }
consumed:
    pthread_mutex_unlock(&table->lock);
    return true;
invalid:
    table->status.invalid++;
    pthread_mutex_unlock(&table->lock);
    return true;
}

int dppd_nd6_resolve(struct dppd_nd6 *table, uint16_t port, const uint8_t address[16], const struct dppd_packet *packet,
    uint64_t now, uint8_t mac[6])
{
    if (table == NULL)
        return -EHOSTUNREACH;
    pthread_mutex_lock(&table->lock);
    clock_advance(table, now);
    int rc = -EHOSTUNREACH;
    if (!domain(local(table, port, NULL), packet) || !dppd_ipv6_unicast(address, true) ||
        local(table, port, address) != NULL)
        goto done;
    struct dppd_nd6_entry *entry = find(table, port, address);
    if (entry == NULL)
        entry = insert(table, port, address);
    if (entry == NULL) {
        rc = -ENOSPC;
        goto done;
    }
    entry->used_ms = table->clock;
    if (entry->state == DPPD_ND6_FAILED && entry->due_ms <= table->clock) {
        entry->probes = 0;
        state(table, entry, DPPD_ND6_INCOMPLETE, table->clock);
    }
    if (entry->state == DPPD_ND6_REACHABLE && entry->due_ms <= table->clock)
        state(table, entry, DPPD_ND6_STALE, 0);
    if (entry->state == DPPD_ND6_STALE)
        state(table, entry, DPPD_ND6_DELAY, table->clock + 5000);
    if (entry->state == DPPD_ND6_INCOMPLETE)
        rc = -EAGAIN;
    else if (entry->state != DPPD_ND6_FAILED) {
        memcpy(mac, entry->mac, 6);
        rc = 0;
    }
done:
    pthread_mutex_unlock(&table->lock);
    return rc;
}

/** 重试由一个工作队列统一驱动，不创建额外 TX 写线程，也不会因每个首包而重复发 NS */
void dppd_nd6_tick(struct dppd_nd6 *table, uint64_t now, dppd_nd6_send send, void *context)
{
    if (table == NULL)
        return;
    pthread_mutex_lock(&table->lock);
    clock_advance(table, now);
    if (table->clock < table->next_tick)
        goto done;
    table->next_tick = table->clock + 10;
    for (uint32_t i = 0; i < table->status.local_count; ++i) {
        struct local_address *owner = &table->locals[i];
        if (owner->state != DPPD_ND6_TENTATIVE || owner->due > table->clock)
            continue;
        if (owner->sent) {
            owner->state = DPPD_ND6_PREFERRED;
            table->status.revision++;
        } else {
            uint8_t group[16];
            solicited(owner->configuration.address, group);
            owner->sent =
                transmit(table, owner, 135, zero, group, owner->configuration.address, NULL, 0, send, context) == 0;
            owner->due = table->clock + 1000;
        }
    }
    for (uint32_t i = 0; i < DPPD_ND6_LIMIT; ++i) {
        struct dppd_nd6_entry *entry = &table->entries[i];
        if (entry->id == 0)
            continue;
        if (entry->state == DPPD_ND6_REACHABLE && entry->due_ms <= table->clock)
            state(table, entry, DPPD_ND6_STALE, 0);
        if ((entry->state == DPPD_ND6_STALE || entry->state == DPPD_ND6_FAILED) &&
            table->clock - entry->used_ms >= 60000) {
            memset(entry, 0, sizeof(*entry));
            table->status.active--;
            table->status.revision++;
            continue;
        }
        if (entry->state == DPPD_ND6_DELAY && entry->due_ms <= table->clock) {
            entry->probes = 0;
            state(table, entry, DPPD_ND6_PROBE, table->clock);
        }
        if ((entry->state != DPPD_ND6_INCOMPLETE && entry->state != DPPD_ND6_PROBE) || entry->due_ms > table->clock)
            continue;
        struct local_address *owner = local(table, entry->port_id, NULL);
        if (owner == NULL || owner->state != DPPD_ND6_PREFERRED)
            continue;
        if (entry->probes == 3) {
            state(table, entry, DPPD_ND6_FAILED, table->clock + 5000);
            table->status.failed++;
            continue;
        }
        uint8_t destination[16];
        solicited(entry->address, destination);
        bool unicast = entry->state == DPPD_ND6_PROBE;
        transmit(table, owner, 135, owner->configuration.address, unicast ? entry->address : destination,
            entry->address, unicast ? entry->mac : NULL, 0, send, context);
        entry->probes++;
        entry->due_ms = table->clock + 1000;
    }
done:
    pthread_mutex_unlock(&table->lock);
}

int dppd_nd6_hold(struct dppd_nd6 *table, uint16_t port, const uint8_t address[16], struct dppd_nd6_pending pending,
    uint64_t now)
{
    if (table == NULL || pending.mbuf == NULL)
        return -EINVAL;
    pthread_mutex_lock(&table->lock);
    clock_advance(table, now);
    struct dppd_nd6_entry *entry = find(table, port, address);
    uint32_t same = 0, available = DPPD_ND6_PENDING_LIMIT;
    for (uint32_t i = 0; i < DPPD_ND6_PENDING_LIMIT; ++i) {
        if (table->waiting[i].packet.mbuf == NULL) {
            if (available == DPPD_ND6_PENDING_LIMIT)
                available = i;
        } else if (entry != NULL && table->waiting[i].neighbor_id == entry->id)
            same++;
    }
    int rc = -ENOBUFS;
    if (entry != NULL && same < 4 && available != DPPD_ND6_PENDING_LIMIT &&
        (pending.expires_ms == 0 || pending.expires_ms > table->clock)) {
        if (pending.expires_ms == 0)
            pending.expires_ms = table->clock + 5000;
        table->waiting[available] =
            (struct waiting_packet){.packet = pending, .neighbor_id = entry->id, .retry_ms = table->clock + 100};
        table->status.pending++;
        rc = 0;
    } else
        table->status.queue_drops++;
    pthread_mutex_unlock(&table->lock);
    return rc;
}
bool dppd_nd6_take(struct dppd_nd6 *table, uint16_t queue, uint64_t now, struct dppd_nd6_pending *pending)
{
    if (table == NULL)
        return false;
    pthread_mutex_lock(&table->lock);
    clock_advance(table, now);
    bool found = false;
    for (uint32_t i = 0; i < DPPD_ND6_PENDING_LIMIT; ++i) {
        struct waiting_packet *waiting = &table->waiting[i];
        if (waiting->packet.mbuf == NULL || waiting->packet.queue != queue)
            continue;
        struct dppd_nd6_entry *entry = NULL;
        for (uint32_t j = 0; j < DPPD_ND6_LIMIT; ++j)
            if (table->entries[j].id == waiting->neighbor_id)
                entry = &table->entries[j];
        bool expired = waiting->packet.expires_ms <= table->clock || entry == NULL || entry->state == DPPD_ND6_FAILED;
        if (!expired && entry->state == DPPD_ND6_INCOMPLETE && waiting->retry_ms > table->clock)
            continue;
        *pending = waiting->packet;
        pending->expired = expired;
        memset(waiting, 0, sizeof(*waiting));
        table->status.pending--;
        table->status.queue_drops += expired;
        found = true;
        break;
    }
    pthread_mutex_unlock(&table->lock);
    return found;
}
void dppd_nd6_status(struct dppd_nd6 *table, struct dppd_nd6_status *status)
{
    memset(status, 0, sizeof(*status));
    if (table == NULL)
        return;
    pthread_mutex_lock(&table->lock);
    *status = table->status;
    for (uint32_t i = 0; i < status->local_count; ++i) {
        status->locals[i].configuration = table->locals[i].configuration;
        status->locals[i].state = table->locals[i].state;
    }
    pthread_mutex_unlock(&table->lock);
}
int dppd_nd6_list(struct dppd_nd6 *table, uint64_t after, uint64_t revision, struct dppd_nd6_page *page)
{
    if (table == NULL)
        return -ENODEV;
    memset(page, 0, sizeof(*page));
    pthread_mutex_lock(&table->lock);
    page->revision = table->status.revision;
    int rc = revision != 0 && revision != page->revision ? -ESTALE : 0;
    for (uint32_t i = 0; rc == 0 && i < DPPD_ND6_LIMIT; ++i) {
        const struct dppd_nd6_entry *entry = &table->entries[i];
        if (entry->id <= after)
            continue;
        uint32_t position = 0;
        while (position < page->count && page->entries[position].id < entry->id)
            position++;
        if (page->count == DPPD_ND6_PAGE_SIZE) {
            page->more = true;
            if (position == page->count)
                continue;
        } else
            page->count++;
        memmove(page->entries + position + 1, page->entries + position, (page->count - position - 1) * sizeof(*entry));
        page->entries[position] = *entry;
    }
    if (page->count != 0)
        page->next_id = page->entries[page->count - 1].id;
    pthread_mutex_unlock(&table->lock);
    return rc;
}
int dppd_nd6_flush(struct dppd_nd6 *table, uint64_t revision)
{
    if (table == NULL)
        return -ENODEV;
    pthread_mutex_lock(&table->lock);
    int rc = revision == 0 ? -EINVAL : revision != table->status.revision ? -ESTALE : 0;
    if (rc == 0) {
        memset(table->entries, 0, sizeof(table->entries));
        table->status.active = 0;
        table->status.revision++;
    }
    pthread_mutex_unlock(&table->lock);
    return rc;
}
