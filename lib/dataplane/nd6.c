#include "dppd/nd6.h"
#include "dppd/ipv6.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <rte_mbuf.h>

/** 本机 IPv6 地址的运行状态，通过重复地址检测后才能用于 ND 收发 */
struct local_address {
    /** 本机地址及其所属网口、VLAN 配置 */
    struct dppd_nd6_local configuration;
    /** 所属网口的 MAC，构造 NS 请求和 NA 通告时作为源 MAC */
    uint8_t mac[6];
    /** 本机地址状态：TENTATIVE 待检测、PREFERRED 可用、DUPLICATE 地址冲突 */
    uint8_t state;
    /** 重复地址检测的 NS 是否已发送成功，成功后还需等待检测期限结束 */
    bool sent;
    /** 毫秒时间点：首次检测的发送时间，发送后改为等待冲突响应的截止时间 */
    uint64_t due;
};
/** 等待下一跳 MAC 的一个槽位，将原始报文与动态邻居记录关联 */
struct waiting_packet {
    /** 原始 mbuf、入口队列和报文截止时间，packet.mbuf 为 NULL 表示空槽位 */
    struct dppd_nd6_pending packet;
    /** 对应 entries 中的 id，不是数组下标；槽位复用后新邻居不会接管旧邻居的等待包 */
    uint64_t neighbor_id;
    /** 毫秒重试时间，入队时设为当前时间加 100；邻居已解析或报文失效时可提前取出 */
    uint64_t retry_ms;
};
/** IPv6 动态邻居管理表，维护本机地址、邻居 IPv6 到 MAC 的映射及等待解析的报文 */
struct dppd_nd6 {
    /** 保护邻居、等待包和状态更新，防止多个处理队列重复创建邻居或取走同一个包 */
    pthread_mutex_t lock;
    /** 当前数量、累计事件和查询版本；其中 locals 在导出状态时从下方运行记录填充 */
    struct dppd_nd6_status status;
    /** 本机 IPv6 地址及重复地址检测进度，有效项为前 status.local_count 个 */
    struct local_address locals[DPPD_ND6_LOCAL_LIMIT];
    /** 动态邻居缓存，按网口和 IPv6 地址查找，id 为 0 的槽位可供新邻居使用 */
    struct dppd_nd6_entry entries[DPPD_ND6_LIMIT];
    /** 等待 MAC 的报文槽位，hold 成功后持有 mbuf，take 取出后交回原处理队列 */
    struct waiting_packet waiting[DPPD_ND6_PENDING_LIMIT];
    /** 内部毫秒时钟，只接受更大的调用方时间，避免多队列传入时间使计时倒退 */
    uint64_t clock;
    /** 下次允许扫描定时任务的毫秒时间点，tick 每次扫描后推迟 10 毫秒 */
    uint64_t next_tick;
    /** 上次分配的邻居 ID，新记录使用 ++next_id，删除记录或清空缓存也不复用旧 ID */
    uint64_t next_id;
    /** 伪随机数生成器状态，用于打散首次地址检测时间和邻居可达状态的有效期 */
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
        body[24] = type == DPPD_ICMPV6_NEIGHBOR_SOLICITATION ? 1 : 2;
        body[25] = 1;
        memcpy(body + 26, owner->mac, 6);
    }
    put16(body + 2, checksum(source, destination, body, length));
    int rc = send == NULL ? -ENOSYS : send(context, owner->configuration.port_id, wire, (uint16_t)(l2 + 40 + length));
    if (rc != 0)
        table->status.tx_errors++;
    else if (type == DPPD_ICMPV6_NEIGHBOR_SOLICITATION)
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
    if (packet->l4_type != DPPD_L4_FRAGMENT && (kind == NULL || !dppd_icmpv6_is_nd(*kind)))
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
        (icmp[0] != DPPD_ICMPV6_NEIGHBOR_SOLICITATION && icmp[0] != DPPD_ICMPV6_NEIGHBOR_ADVERTISEMENT) ||
        icmp[1] != 0 || !mac_valid(link + 6) ||
        !dppd_ipv6_unicast(icmp + 8, true) || checksum(packet->ipv6_src, packet->ipv6_dst, icmp, length) != 0)
        goto invalid;
    if (memcmp(link + 6, owner->mac, 6) == 0)
        goto consumed;
    bool ns = icmp[0] == DPPD_ICMPV6_NEIGHBOR_SOLICITATION, unspecified = equal(packet->ipv6_src, zero);
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
        transmit(table, target, DPPD_ICMPV6_NEIGHBOR_ADVERTISEMENT, target->configuration.address,
            unspecified ? all_nodes : packet->ipv6_src,
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

/**
 * 按出口 port 和下一跳 address 查询或建立动态邻居记录，packet 用于校验报文所属链路域
 * now 为当前毫秒时间，成功返回 0 并将下一跳 MAC 写入 mac，未完成解析返回 -EAGAIN
 * 不适用或失败返回 -EHOSTUNREACH，表满返回 -ENOSPC，本函数不接管报文
 * 缺记录 → 建立 INCOMPLETE 条目 → 返回 -EAGAIN → worker 尝试 hold 暂存
 * 有可用 MAC → 写入 mac → 返回 0 → worker 继续出口检查与改写
 * 邻居探测由 tick 驱动，这里不直接发送 NS，也不等待邻居应答
 */
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
                transmit(table, owner, DPPD_ICMPV6_NEIGHBOR_SOLICITATION, zero, group,
                    owner->configuration.address, NULL, 0, send, context) == 0;
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
        transmit(table, owner, DPPD_ICMPV6_NEIGHBOR_SOLICITATION, owner->configuration.address,
            unicast ? entry->address : destination,
            entry->address, unicast ? entry->mac : NULL, 0, send, context);
        entry->probes++;
        entry->due_ms = table->clock + 1000;
    }
done:
    pthread_mutex_unlock(&table->lock);
}

/**
 * 保存等待下一跳 MAC 的报文记录，成功后等待队列接管 pending.mbuf
 *
 * port 和 address 定位动态邻居，pending 保存原始 mbuf、入口、队列和截止时间
 *
 * 返回 0：已保存到 waiting，调用方继续处理下一个包，不再发送或释放此 mbuf
 * 返回 -EINVAL 或 -ENOBUFS：没有接管 mbuf，调用方负责后续释放
 *
 * 入队条件：邻居记录存在、同邻居等待包少于 4、总计 128 个槽位尚有空位、报文未超时
 */
int dppd_nd6_hold(struct dppd_nd6 *table, uint16_t port, const uint8_t address[16], struct dppd_nd6_pending pending,
    uint64_t now)
{
    if (table == NULL || pending.mbuf == NULL)
        return -EINVAL;
    pthread_mutex_lock(&table->lock);
    clock_advance(table, now);
    struct dppd_nd6_entry *entry = find(table, port, address);
    /** 记录首个空槽位，并统计已有多少个包在等待同一邻居，避免单个邻居占满队列 */
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
        /** 保存记录和原始 mbuf 指针，不复制报文字节；仍未解析完成时，最早 100 毫秒后取出重试 */
        table->waiting[available] =
            (struct waiting_packet){.packet = pending, .neighbor_id = entry->id, .retry_ms = table->clock + 100};
        table->status.pending++;
        rc = 0;
    } else
        table->status.queue_drops++;
    pthread_mutex_unlock(&table->lock);
    return rc;
}
/**
 * 从 ND 等待数组取出属于指定处理队列的一个报文，将 mbuf 交回调用方
 * queue 指定原处理队列，now 为当前毫秒时间，pending 是调用方提供的非空输出区
 *
 * 返回 true：已输出报文记录并清空对应等待槽位，pending->expired 表示是否须丢弃
 * 返回 false：当前没有可取出的记录，调用方不能使用 pending 中的内容
 *
 * 可取出情形：邻居已有 MAC、未完成解析但重试时间已到、报文或邻居状态已经失效
 * 因此返回 true 只表示取回一个包，不保证 MAC 已有或最终可以转发
 */
bool dppd_nd6_take(struct dppd_nd6 *table, uint16_t queue, uint64_t now, struct dppd_nd6_pending *pending)
{
    if (table == NULL)
        return false;
    pthread_mutex_lock(&table->lock);
    clock_advance(table, now);
    bool found = false;
    for (uint32_t i = 0; i < DPPD_ND6_PENDING_LIMIT; ++i) {
        struct waiting_packet *waiting = &table->waiting[i];
        /** 只取本队列的有效记录，避免把同一 mbuf 同时交给不同处理队列 */
        if (waiting->packet.mbuf == NULL || waiting->packet.queue != queue)
            continue;
        struct dppd_nd6_entry *entry = NULL;
        for (uint32_t j = 0; j < DPPD_ND6_LIMIT; ++j)
            if (table->entries[j].id == waiting->neighbor_id)
                entry = &table->entries[j];
        /** 超时、邻居记录消失或解析已失败的包也要取出，标记 expired 后交给调用方释放 */
        bool expired = waiting->packet.expires_ms <= table->clock || entry == NULL || entry->state == DPPD_ND6_FAILED;
        /** 仍在等待 MAC 且未到重试时间才继续留队；MAC 已可用或记录失效时可提前取出 */
        if (!expired && entry->state == DPPD_ND6_INCOMPLETE && waiting->retry_ms > table->clock)
            continue;
        /** 复制记录后释放等待槽位，mbuf 本身未释放，现在由调用方继续处理 */
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
