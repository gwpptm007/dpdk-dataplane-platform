#define _POSIX_C_SOURCE 200809L
#include "dppd/conntrack.h"
#include "dppd/ipv6.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct connection {
    struct dppd_connection_view view;
    uint32_t next[2], sequence[2];
    uint8_t fin_seen;
};

/** 首版用一把短期表锁串行保护双向索引和端口池，跨 RX 队列也不能创建重复会话 */
struct dppd_conntrack {
    pthread_mutex_t lock;
    bool initialized, pending_new;
    struct connection *entries, pending;
    uint32_t *buckets, *free_slots, bucket_count, free_count, cursor, pending_slot, pending_bytes;
    uint16_t port_cursor[2][2];
    uint8_t ports[2][2][65536];
    uint64_t clock_ms, next_id, full_gc_after_ms;
    struct dppd_conntrack_status status;
    struct dppd_dnat_rule dnat[DPPD_MAX_DNAT_RULES];
};

uint64_t dppd_monotonic_ms(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        abort();
    return (uint64_t)value.tv_sec * 1000 + (uint64_t)value.tv_nsec / 1000000;
}

static struct dppd_flow_tuple tuple(const struct dppd_packet *packet)
{
    struct dppd_flow_tuple value = {.source_be = packet->ipv4_src_be,
        .destination_be = packet->ipv4_dst_be,
        .source_port_be = packet->l4_src_port_be,
        .destination_port_be = packet->l4_dst_port_be,
        .protocol = packet->ip_protocol,
        .ipv6 = packet->l3_type == DPPD_L3_IPV6};
    if (value.ipv6) {
        memcpy(value.source6, packet->ipv6_src, 16);
        memcpy(value.destination6, packet->ipv6_dst, 16);
    }
    return value;
}

static struct dppd_flow_tuple reverse(struct dppd_flow_tuple value)
{
    struct dppd_flow_tuple result = value;
    result.source_be = value.destination_be;
    result.destination_be = value.source_be;
    result.source_port_be = value.destination_port_be;
    result.destination_port_be = value.source_port_be;
    memcpy(result.source6, value.destination6, 16);
    memcpy(result.destination6, value.source6, 16);
    return result;
}

static bool equal(const struct dppd_flow_tuple *a, const struct dppd_flow_tuple *b)
{
    return a->ipv6 == b->ipv6 && a->protocol == b->protocol && a->source_port_be == b->source_port_be &&
        a->destination_port_be == b->destination_port_be &&
        (a->ipv6 ? memcmp(a->source6, b->source6, 16) == 0 && memcmp(a->destination6, b->destination6, 16) == 0
                 : a->source_be == b->source_be && a->destination_be == b->destination_be);
}

/** 同一个进程的两种地址族共用状态机，但转换策略和端口占用不能串到另一种地址族 */
static const struct dppd_snat_config *source_nat(const struct dppd_conntrack *table, bool ipv6)
{
    return ipv6 ? &table->status.snat6 : &table->status.snat;
}

static bool same_address(bool ipv6, uint32_t left, const uint8_t left6[16], uint32_t right, const uint8_t right6[16])
{
    return ipv6 ? memcmp(left6, right6, 16) == 0 : left == right;
}

static bool has_payload(const struct dppd_packet *packet)
{
    uint32_t total = packet->l3_type == DPPD_L3_IPV6 ? 40U + packet->ipv6_payload_len : packet->ipv4_total_len;
    return total != (uint32_t)packet->l3_len + packet->l4_len;
}

static uint32_t bucket(const struct dppd_conntrack *table, const struct dppd_flow_tuple *value, uint16_t ingress)
{
    uint32_t hash = (2166136261U ^ value->ipv6) * 16777619U;
    if (value->ipv6)
        for (unsigned int i = 0; i < 16; ++i) {
            hash = (hash ^ value->source6[i]) * 16777619U;
            hash = (hash ^ value->destination6[i]) * 16777619U;
        }
    const uint32_t words[] = {value->ipv6 ? 0 : value->source_be, value->ipv6 ? 0 : value->destination_be,
        (uint32_t)value->source_port_be << 16 | value->destination_port_be, ingress, value->protocol};
    for (size_t i = 0; i < sizeof(words) / sizeof(*words); ++i)
        for (unsigned int byte = 0; byte < 4; ++byte)
            hash = (hash ^ ((words[i] >> (8 * byte)) & 255U)) * 16777619U;
    return hash & (table->bucket_count - 1);
}

/** 链接编号同时编码槽位和方向，同一个会话只保存一份状态，却可以从两个方向找到 */
static uint32_t find(struct dppd_conntrack *table, const struct dppd_flow_tuple *key, uint16_t ingress)
{
    uint32_t link = table->buckets[bucket(table, key, ingress)];
    while (link != 0) {
        struct connection *entry = &table->entries[(link - 1) / 2];
        unsigned int direction = (link - 1) % 2;
        struct dppd_flow_tuple candidate = direction ? reverse(entry->view.translated) : entry->view.original;
        if ((direction ? entry->view.egress : entry->view.ingress) == ingress && equal(&candidate, key))
            return link;
        link = entry->next[direction];
    }
    return 0;
}

static void remove_connection(struct dppd_conntrack *table, uint32_t slot, bool expired)
{
    struct connection *entry = &table->entries[slot];
    for (unsigned int direction = 0; direction < 2; ++direction) {
        struct dppd_flow_tuple key = direction ? reverse(entry->view.translated) : entry->view.original;
        uint32_t *link = &table->buckets[bucket(table, &key, direction ? entry->view.egress : entry->view.ingress)];
        uint32_t wanted = slot * 2 + direction + 1;
        while (*link != wanted && *link != 0)
            link = &table->entries[(*link - 1) / 2].next[(*link - 1) % 2];
        if (*link != 0)
            *link = entry->next[direction];
    }
    if (entry->view.nat && entry->view.dnat_rule_id == 0)
        table->ports[entry->view.original.ipv6][entry->view.original.protocol == 6]
                    [ntohs(entry->view.translated.source_port_be)] = 0;
    memset(entry, 0, sizeof(*entry));
    table->free_slots[table->free_count++] = slot;
    table->status.active--;
    table->status.revision++;
    if (expired)
        table->status.expired++;
}

/** 使用调用方的单调时间，时钟回退时保持上次时间，不让无符号减法提前清空会话 */
static void collect(struct dppd_conntrack *table, uint64_t now, uint32_t budget)
{
    if (now > table->clock_ms)
        table->clock_ms = now;
    if (budget > table->status.capacity)
        budget = table->status.capacity;
    for (uint32_t i = 0; i < budget; ++i) {
        uint32_t slot = table->cursor++ % table->status.capacity;
        if (table->entries[slot].view.id != 0 && table->entries[slot].view.expires_ms <= table->clock_ms)
            remove_connection(table, slot, true);
    }
}

int dppd_conntrack_create_dual(struct dppd_conntrack **result, uint32_t capacity, uint32_t timeout_s,
    const struct dppd_snat_config *snat, const struct dppd_snat_config *snat6, const struct dppd_dnat_rule *dnat,
    uint32_t dnat_count)
{
    if (result == NULL || *result != NULL || capacity == 0 || capacity > 65536 || timeout_s == 0 || timeout_s > 86400)
        return -EINVAL;
    if ((snat != NULL && snat->enabled && snat->ipv6) || (snat6 != NULL && snat6->enabled && !snat6->ipv6))
        return -EINVAL;
    int validation = dppd_nat_validate(snat, dnat, dnat_count);
    if (validation == 0)
        validation = dppd_nat_validate(snat6, dnat, dnat_count);
    if (validation != 0)
        return validation;
    struct dppd_conntrack *table = calloc(1, sizeof(*table));
    if (table == NULL)
        return -ENOMEM;
    table->bucket_count = 4;
    while (table->bucket_count < capacity * 4)
        table->bucket_count *= 2;
    table->entries = calloc(capacity, sizeof(*table->entries));
    table->buckets = calloc(table->bucket_count, sizeof(*table->buckets));
    table->free_slots = calloc(capacity, sizeof(*table->free_slots));
    if (table->entries == NULL || table->buckets == NULL || table->free_slots == NULL) {
        dppd_conntrack_destroy(table);
        return -ENOMEM;
    }
    if (pthread_mutex_init(&table->lock, NULL) != 0) {
        dppd_conntrack_destroy(table);
        return -EIO;
    }
    table->initialized = true;
    table->status =
        (struct dppd_conntrack_status){.enabled = true, .capacity = capacity, .timeout_s = timeout_s, .revision = 1};
    if (snat != NULL)
        table->status.snat = *snat;
    if (snat6 != NULL)
        table->status.snat6 = *snat6;
    table->status.dnat_count = dnat_count;
    if (dnat_count != 0)
        memcpy(table->dnat, dnat, dnat_count * sizeof(*dnat));
    /** 静态公网端口在进程存活期间一直保留，清会话也不能把它们借给动态 SNAT */
    for (uint32_t i = 0; i < dnat_count; ++i) {
        const struct dppd_dnat_rule *rule = &dnat[i];
        const struct dppd_snat_config *nat = source_nat(table, rule->ipv6);
        if (!nat->enabled || nat->outside_port != rule->outside_port ||
            !same_address(rule->ipv6, nat->address_be, nat->address6, rule->public_address_be, rule->public_address6))
            continue;
        for (uint32_t port = rule->public_first; port <= rule->public_last; ++port)
            table->ports[rule->ipv6][rule->protocol == 6][port] = 2;
    }
    table->free_count = capacity;
    for (uint32_t i = 0; i < capacity; ++i)
        table->free_slots[i] = capacity - i - 1;
    *result = table;
    return 0;
}

int dppd_conntrack_create(struct dppd_conntrack **result, uint32_t capacity, uint32_t timeout_s,
    const struct dppd_snat_config *snat, const struct dppd_dnat_rule *dnat, uint32_t dnat_count)
{
    return dppd_conntrack_create_dual(result, capacity, timeout_s, snat, NULL, dnat, dnat_count);
}

void dppd_conntrack_destroy(struct dppd_conntrack *table)
{
    if (table == NULL)
        return;
    if (table->initialized)
        pthread_mutex_destroy(&table->lock);
    free(table->entries);
    free(table->buckets);
    free(table->free_slots);
    free(table);
}

/** TCP 先核对握手序号，已建立阶段只做状态与标志检查，不模拟端系统的完整窗口验证 */
static int tcp_transition(struct connection *entry, const struct dppd_packet *packet, bool reply)
{
    uint8_t flags = packet->tcp_flags;
    uint32_t sequence = ntohl(packet->tcp_sequence_be), ack = ntohl(packet->tcp_acknowledgment_be);
    /** 不支持 Fast Open，SYN 与 SYN/ACK 的首次发送和重传都不能携带负载 */
    if ((flags & 2U) != 0 && has_payload(packet))
        return -EPROTO;
    if ((flags & 3U) == 3U || (flags & 5U) == 5U || ((flags & 2U) != 0 && (flags & 4U) != 0))
        return -EPROTO;
    if ((flags & 4U) != 0) {
        if (entry->view.tcp_state <= DPPD_TCP_SYN_ACK && reply &&
            ((flags & 16U) == 0 || ack != entry->sequence[0] + 1U))
            return -EPROTO;
        entry->view.tcp_state = DPPD_TCP_CLOSED;
        return 0;
    }
    switch (entry->view.tcp_state) {
    case DPPD_TCP_SYN:
        if (!reply && (flags & 0x3fU) == 2 && sequence == entry->sequence[0])
            return 0;
        if (reply && (flags & 0x3fU) == 18 && ack == entry->sequence[0] + 1U) {
            entry->sequence[1] = sequence;
            entry->view.tcp_state = DPPD_TCP_SYN_ACK;
            return 0;
        }
        return -EPROTO;
    case DPPD_TCP_SYN_ACK:
        if (reply && (flags & 0x3fU) == 18 && sequence == entry->sequence[1] && ack == entry->sequence[0] + 1U)
            return 0;
        if (!reply && (flags & 0x3fU) == 2 && sequence == entry->sequence[0])
            return 0;
        if (!reply && (flags & 0x17U) == 16 && ack == entry->sequence[1] + 1U && sequence == entry->sequence[0] + 1U) {
            entry->view.tcp_state = DPPD_TCP_OPEN;
            return 0;
        }
        return -EPROTO;
    case DPPD_TCP_OPEN:
    case DPPD_TCP_FIN:
        if ((flags & 2U) != 0 || (flags & 16U) == 0)
            return -EPROTO;
        if ((entry->fin_seen & (1U << reply)) != 0 && has_payload(packet))
            return -EPROTO;
        if ((flags & 1U) != 0) {
            entry->fin_seen |= (uint8_t)(1U << reply);
            entry->view.tcp_state = DPPD_TCP_FIN;
        }
        return 0;
    case DPPD_TCP_CLOSED:
        return (flags & 0x17U) == 16 && !has_payload(packet) ? 0 : -EPROTO;
    default:
        return -EPROTO;
    }
}

static int allocate_port(struct dppd_conntrack *table, bool ipv6, uint8_t protocol, uint16_t *port_be)
{
    const struct dppd_snat_config *nat = source_nat(table, ipv6);
    uint32_t count = (uint32_t)nat->last_port - nat->first_port + 1;
    unsigned int group = protocol == 6;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t port = (uint16_t)(nat->first_port + table->port_cursor[ipv6][group]);
        table->port_cursor[ipv6][group] = (uint16_t)(((uint32_t)table->port_cursor[ipv6][group] + 1) % count);
        if (table->ports[ipv6][group][port] == 0) {
            table->ports[ipv6][group][port] = 1;
            *port_be = htons(port);
            return 0;
        }
    }
    table->status.pool_full++;
    return -EADDRNOTAVAIL;
}

/** 只为显式外口的静态服务新建入站会话，旧后端回包不能被重新解释成动态出站连接 */
static int new_destination(struct dppd_conntrack *table, const struct dppd_flow_tuple *key, uint16_t ingress,
    uint32_t *rule_id)
{
    uint16_t source = ntohs(key->source_port_be), destination = ntohs(key->destination_port_be);
    bool wrong_ingress = false, backend_reply = false, family_dnat = false;
    for (uint32_t i = 0; i < table->status.dnat_count; ++i) {
        const struct dppd_dnat_rule *rule = &table->dnat[i];
        if (key->ipv6 != rule->ipv6)
            continue;
        family_dnat = true;
        if (key->protocol != rule->protocol)
            continue;
        if (same_address(key->ipv6, key->destination_be, key->destination6, rule->public_address_be,
                rule->public_address6) &&
            destination >= rule->public_first && destination <= rule->public_last) {
            if (ingress == rule->outside_port)
                *rule_id = i + 1;
            else
                wrong_ingress = true;
        }
        uint32_t last = (uint32_t)rule->private_first + rule->public_last - rule->public_first;
        if (ingress == rule->inside_port &&
            same_address(key->ipv6, key->source_be, key->source6, rule->private_address_be, rule->private_address6) &&
            source >= rule->private_first && source <= last)
            backend_reply = true;
    }
    if (*rule_id != 0)
        return 0;
    if (wrong_ingress)
        return -EXDEV;
    if (backend_reply)
        return -ENOENT;
    const struct dppd_snat_config *nat = source_nat(table, key->ipv6);
    if (nat->enabled)
        return ingress == nat->inside_port ? 0 : -ENOENT;
    return family_dnat ? -ENOENT : 0;
}

int dppd_conntrack_begin(struct dppd_conntrack *table, uint16_t ingress, const struct dppd_packet *packet,
    uint64_t now_ms, struct dppd_connection_attempt *attempt)
{
    if (attempt == NULL || packet == NULL)
        return -EINVAL;
    memset(attempt, 0, sizeof(*attempt));
    attempt->state = DPPD_CT_UNTRACKED;
    attempt->policy = attempt->rewrite = tuple(packet);
    if (table == NULL)
        return 0;
    bool ipv6 = packet->l3_type == DPPD_L3_IPV6;
    bool nat_enabled = source_nat(table, ipv6)->enabled;
    for (uint32_t i = 0; i < table->status.dnat_count; ++i)
        nat_enabled |= table->dnat[i].ipv6 == ipv6;
    /** 不支持的扩展头不是普通未跟踪流量，不能借 UNTRACKED 策略隐藏协议拒绝原因 */
    if (ipv6 && packet->ipv6_unsupported)
        return -ENOTSUP;
    if ((packet->l3_type != DPPD_L3_IPV4 && !ipv6) ||
        (packet->l4_type != DPPD_L4_TCP && packet->l4_type != DPPD_L4_UDP))
        return nat_enabled || packet->l4_type == DPPD_L4_FRAGMENT ? -ENOTSUP : 0;
    pthread_mutex_lock(&table->lock);
    table->pending_bytes = packet->packet_len;
    collect(table, now_ms, 8);
    uint32_t source = ntohl(packet->ipv4_src_be), destination = ntohl(packet->ipv4_dst_be);
    bool invalid_address = ipv6
        ? (!dppd_ipv6_unicast(packet->ipv6_src, false) || !dppd_ipv6_unicast(packet->ipv6_dst, false))
        : (source < 0x01000000U || destination < 0x01000000U || (source >> 24) == 127 || (destination >> 24) == 127 ||
              source >= 0xe0000000U || destination >= 0xe0000000U);
    if (invalid_address || ingress == UINT16_MAX || packet->ip_protocol != (packet->l4_type == DPPD_L4_TCP ? 6 : 17)) {
        attempt->state = DPPD_CT_INVALID;
        table->status.invalid++;
        pthread_mutex_unlock(&table->lock);
        return -EPROTO;
    }
    uint32_t link = find(table, &attempt->policy, ingress);
    if (link != 0 && table->entries[(link - 1) / 2].view.expires_ms <= table->clock_ms) {
        remove_connection(table, (link - 1) / 2, true);
        link = 0;
    }
    const struct dppd_snat_config *nat = source_nat(table, ipv6);
    int rc = 0;
    table->pending_new = link == 0;
    if (link != 0) {
        table->pending_slot = (link - 1) / 2;
        table->pending = table->entries[table->pending_slot];
        attempt->reply = (link - 1) % 2;
        if (packet->l4_type == DPPD_L4_TCP)
            rc = tcp_transition(&table->pending, packet, attempt->reply);
    } else {
        uint32_t dnat_id = 0;
        struct dppd_flow_tuple reversed = reverse(attempt->policy);
        if (equal(&attempt->policy, &reversed))
            rc = -EPROTO;
        if (rc == 0)
            rc = new_destination(table, &attempt->policy, ingress, &dnat_id);
        if (rc == 0 && packet->l4_type == DPPD_L4_TCP && ((packet->tcp_flags & 0x3fU) != 2 || has_payload(packet)))
            rc = -EPROTO;
        if (rc == 0 && table->free_count == 0) {
            /** 满表探测限速，不能让每个新五元组都强迫工作线程扫描整张大表 */
            if (table->clock_ms >= table->full_gc_after_ms) {
                collect(table, table->clock_ms, table->status.capacity);
                table->full_gc_after_ms = UINT64_MAX - table->clock_ms < 100 ? UINT64_MAX : table->clock_ms + 100;
            }
            if (table->free_count == 0) {
                table->status.table_full++;
                rc = -ENOSPC;
            }
        }
        if (rc == 0 && table->next_id == UINT64_MAX)
            rc = -EOVERFLOW;
        memset(&table->pending, 0, sizeof(table->pending));
        table->pending.view = (struct dppd_connection_view){.id = table->next_id + 1,
            .original = attempt->policy,
            .translated = attempt->policy,
            .ingress = ingress,
            .nat = nat->enabled || dnat_id != 0,
            .dnat_rule_id = dnat_id,
            .tcp_state = packet->l4_type == DPPD_L4_TCP ? DPPD_TCP_SYN : DPPD_TCP_NONE};
        table->pending.sequence[0] = ntohl(packet->tcp_sequence_be);
        if (rc == 0 && dnat_id != 0) {
            const struct dppd_dnat_rule *rule = &table->dnat[dnat_id - 1];
            table->pending.view.egress = rule->inside_port;
            table->pending.view.translated.destination_be = rule->private_address_be;
            memcpy(table->pending.view.translated.destination6, rule->private_address6, 16);
            table->pending.view.translated.destination_port_be =
                htons((uint16_t)(rule->private_first + ntohs(packet->l4_dst_port_be) - rule->public_first));
        } else if (rc == 0 && nat->enabled) {
            table->pending.view.translated.source_be = nat->address_be;
            memcpy(table->pending.view.translated.source6, nat->address6, 16);
            rc = allocate_port(table, ipv6, packet->ip_protocol, &table->pending.view.translated.source_port_be);
        }
        if (rc == 0)
            table->pending_slot = table->free_slots[table->free_count - 1];
    }
    if (rc != 0) {
        attempt->state = DPPD_CT_INVALID;
        table->status.invalid++;
        pthread_mutex_unlock(&table->lock);
        return rc;
    }
    attempt->owner = table;
    table->pending.view.seen_reply |= attempt->reply;
    attempt->state = table->pending.view.seen_reply ? DPPD_CT_ESTABLISHED : DPPD_CT_NEW;
    attempt->nat = table->pending.view.nat;
    /** ACL 看 DNAT 后的后端地址，也看 SNAT 前的内网地址，正反向保持同一逻辑身份 */
    struct dppd_flow_tuple logical =
        table->pending.view.dnat_rule_id != 0 ? table->pending.view.translated : table->pending.view.original;
    attempt->policy = attempt->reply ? reverse(logical) : logical;
    attempt->rewrite = attempt->reply ? reverse(table->pending.view.original) : table->pending.view.translated;
    return 0;
}

/** 只有策略、路由和报文改写成功才提交候选状态，失败的新连接立即归还临时端口 */
int dppd_conntrack_finish(struct dppd_connection_attempt *attempt, uint16_t egress, bool accept)
{
    if (attempt == NULL || attempt->owner == NULL)
        return 0;
    struct dppd_conntrack *table = attempt->owner;
    struct dppd_connection_view *view = &table->pending.view;
    int rc = 0;
    const struct dppd_snat_config *nat = source_nat(table, view->original.ipv6);
    if (accept &&
        (((!table->pending_new || view->dnat_rule_id != 0) &&
             egress != (attempt->reply ? view->ingress : view->egress)) ||
            (view->nat && view->dnat_rule_id == 0 &&
                egress != (attempt->reply ? nat->inside_port : nat->outside_port))))
        rc = -EXDEV;
    if (accept && rc == 0 && table->pending_new) {
        view->egress = egress;
        struct dppd_flow_tuple reply = reverse(view->translated);
        if (find(table, &reply, egress) != 0)
            rc = -EEXIST;
    }
    if (accept && rc == 0) {
        uint32_t seconds = table->status.timeout_s;
        if ((!view->seen_reply || view->tcp_state == DPPD_TCP_SYN || view->tcp_state == DPPD_TCP_SYN_ACK) &&
            seconds > 30)
            seconds = 30;
        if (view->tcp_state == DPPD_TCP_FIN && seconds > 30)
            seconds = 30;
        if (view->tcp_state == DPPD_TCP_CLOSED && seconds > 2)
            seconds = 2;
        view->last_seen_ms = table->clock_ms;
        uint64_t duration = (uint64_t)seconds * 1000;
        view->expires_ms = UINT64_MAX - table->clock_ms < duration ? UINT64_MAX : table->clock_ms + duration;
        view->packets[attempt->reply]++;
        view->bytes[attempt->reply] += table->pending_bytes;
        if (table->pending_new) {
            table->free_count--;
            table->next_id = view->id;
            for (unsigned int direction = 0; direction < 2; ++direction) {
                struct dppd_flow_tuple key = direction ? reverse(view->translated) : view->original;
                uint32_t index = bucket(table, &key, direction ? view->egress : view->ingress);
                table->pending.next[direction] = table->buckets[index];
                table->buckets[index] = table->pending_slot * 2 + direction + 1;
            }
            table->status.active++;
            if (table->status.active > table->status.peak)
                table->status.peak = table->status.active;
            table->status.created++;
            table->status.revision++;
        }
        table->entries[table->pending_slot] = table->pending;
    } else if (table->pending_new && view->nat && view->dnat_rule_id == 0)
        table->ports[view->original.ipv6][view->original.protocol == 6][ntohs(view->translated.source_port_be)] = 0;
    attempt->owner = NULL;
    pthread_mutex_unlock(&table->lock);
    return rc;
}

void dppd_conntrack_gc(struct dppd_conntrack *table, uint64_t now_ms, uint32_t budget)
{
    if (table != NULL) {
        pthread_mutex_lock(&table->lock);
        collect(table, now_ms, budget);
        pthread_mutex_unlock(&table->lock);
    }
}

void dppd_conntrack_status(struct dppd_conntrack *table, uint64_t now_ms, struct dppd_conntrack_status *status)
{
    memset(status, 0, sizeof(*status));
    if (table == NULL)
        return;
    pthread_mutex_lock(&table->lock);
    collect(table, now_ms, table->status.capacity);
    *status = table->status;
    pthread_mutex_unlock(&table->lock);
}

/** 页内按不会复用的会话 ID 排序，修订号只保护成员集合，流量计数和状态允许继续前进 */
int dppd_conntrack_list(struct dppd_conntrack *table, uint64_t now_ms, uint64_t after_id, uint64_t expected_revision,
    struct dppd_connection_page *page)
{
    if (table == NULL || page == NULL)
        return -ENODEV;
    pthread_mutex_lock(&table->lock);
    collect(table, now_ms, table->status.capacity);
    int rc = expected_revision != 0 && expected_revision != table->status.revision ? -ESTALE : 0;
    memset(page, 0, sizeof(*page));
    page->revision = table->status.revision;
    for (uint32_t i = 0; rc == 0 && i < table->status.capacity; ++i) {
        const struct dppd_connection_view *entry = &table->entries[i].view;
        if (entry->id <= after_id)
            continue;
        uint32_t position = 0;
        while (position < page->count && page->entries[position].id < entry->id)
            position++;
        if (page->count == DPPD_CT_PAGE_SIZE) {
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

int dppd_conntrack_flush(struct dppd_conntrack *table, uint64_t expected_revision)
{
    if (table == NULL)
        return -ENODEV;
    pthread_mutex_lock(&table->lock);
    int rc = expected_revision == 0 ? -EINVAL : expected_revision != table->status.revision ? -ESTALE : 0;
    if (rc == 0)
        for (uint32_t i = 0; i < table->status.capacity; ++i)
            if (table->entries[i].view.id != 0) {
                remove_connection(table, i, false);
                table->status.flushed++;
            }
    pthread_mutex_unlock(&table->lock);
    return rc;
}

int dppd_conntrack_dnat_list(struct dppd_conntrack *table, uint64_t after_id, struct dppd_dnat_page *page)
{
    if (table == NULL || page == NULL)
        return -ENODEV;
    memset(page, 0, sizeof(*page));
    /** 启动后规则不可变，查询不需要占住逐包会话锁，游标超过末尾时返回空页 */
    for (uint32_t i = 0; i < table->status.dnat_count; ++i) {
        if (i + 1 <= after_id)
            continue;
        if (page->count == DPPD_CT_PAGE_SIZE) {
            page->more = true;
            break;
        }
        page->entries[page->count].id = i + 1;
        page->entries[page->count++].rule = table->dnat[i];
        page->next_id = i + 1;
    }
    return 0;
}
