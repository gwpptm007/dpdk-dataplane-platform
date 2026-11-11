#include "dppd/arp.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <rte_mbuf.h>

/** 本机 IPv4 地址的运行状态，通过重复地址检测后才能用于 ARP 收发 */
struct local_address {
    /** 本机地址及其所属网口、VLAN 配置 */
    struct dppd_arp_local configuration;
    /** 所属网口的 MAC，构造 ARP 请求和应答时作为源 MAC */
    uint8_t mac[6];
    /** 本机地址状态：TENTATIVE 待检测、PREFERRED 可用、DUPLICATE 地址冲突 */
    uint8_t state;
    /** 已成功发送的冲突探测与地址通告次数，失败不推进检测进度 */
    uint8_t probes, announcements;
    /** 毫秒时间点：首次检测的发送时间，发送后改为等待冲突响应的截止时间 */
    uint64_t due;
};
/** 等待下一跳 MAC 的一个槽位，将原始报文与动态邻居记录关联 */
struct waiting_packet {
    /** 原始 mbuf、入口队列和报文截止时间，packet.mbuf 为 NULL 表示空槽位 */
    struct dppd_arp_pending packet;
    /** 对应 entries 中的 id，不是数组下标；槽位复用后新邻居不会接管旧邻居的等待包 */
    uint64_t neighbor_id;
    /** 毫秒重试时间，入队时设为当前时间加 100；邻居已解析或报文失效时可提前取出 */
    uint64_t retry_ms;
};
/** IPv4 动态邻居管理表，维护本机地址、邻居 IPv4 到 MAC 的映射及等待解析的报文 */
struct dppd_arp {
    /** 保护邻居、等待包和状态更新，防止多个处理队列重复创建邻居或取走同一个包 */
    pthread_mutex_t lock;
    /** 当前数量、累计事件和查询版本；其中 locals 在导出状态时从下方运行记录填充 */
    struct dppd_arp_status status;
    /** 本机 IPv4 地址及重复地址检测进度，有效项为前 status.local_count 个 */
    struct local_address locals[DPPD_ARP_LOCAL_LIMIT];
    /** 动态邻居缓存，按网口和 IPv4 地址查找，id 为 0 的槽位可供新邻居使用 */
    struct dppd_arp_entry entries[DPPD_ARP_LIMIT];
    /** 等待 MAC 的报文槽位，hold 成功后持有 mbuf，take 取出后交回原处理队列 */
    struct waiting_packet waiting[DPPD_ARP_PENDING_LIMIT];
    /** 容器生命周期内固定的设备绑定，在线策略只能引用这些端口 */
    struct dppd_arp_port ports[DPPD_MAX_PORTS];
    uint16_t port_count;
    /** 内部毫秒时钟，只接受更大的调用方时间，避免多队列传入时间使计时倒退 */
    uint64_t clock;
    /** 下次允许扫描定时任务的毫秒时间点，tick 每次扫描后推迟 10 毫秒 */
    uint64_t next_tick;
    /** 上次分配的邻居 ID，新记录使用 ++next_id，删除记录或清空缓存也不复用旧 ID */
    uint64_t next_id;
    /** 伪随机数生成器状态，用于打散首次地址检测时间及连续探测的间隔 */
    uint32_t random;
};
static const uint8_t zero[16] = {0};
static const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
/** 不把未指定、回环、组播或广播地址当作可学习的 IPv4 主机 */
static bool unicast(const uint8_t address[4])
{
    return address[0] != 0 && address[0] != 127 && address[0] < 224;
}

static bool equal(const uint8_t a[4], const uint8_t b[4])
{
    return memcmp(a, b, 4) == 0;
}
static bool mac_valid(const uint8_t mac[6])
{
    return (mac[0] & 1U) == 0 && memcmp(mac, zero, 6) != 0;
}
/** 版本触顶后保持最大值，后续写入口拒绝推进，旧版本不能因整数回绕重新命中 */
static void advance_revision(struct dppd_arp *table)
{
    if (table->status.revision != UINT64_MAX)
        table->status.revision++;
}
static uint64_t clock_advance(struct dppd_arp *table, uint64_t now)
{
    if (now > table->clock)
        table->clock = now;
    return table->clock;
}
static uint32_t random_value(struct dppd_arp *table)
{
    table->random ^= table->random << 13;
    table->random ^= table->random >> 17;
    table->random ^= table->random << 5;
    return table->random;
}
static struct local_address *local(struct dppd_arp *table, uint16_t port, const uint8_t *address)
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
/** 按出口网口和邻居 IPv4 查动态缓存，只查已有记录，与 local 查本机地址分别使用 */
static struct dppd_arp_entry *find(struct dppd_arp *table, uint16_t port, const uint8_t address[4])
{
    for (uint32_t i = 0; i < DPPD_ARP_LIMIT; ++i)
        if (table->entries[i].id != 0 && table->entries[i].port_id == port && equal(table->entries[i].address, address))
            return &table->entries[i];
    return NULL;
}
/** 更新邻居状态、变更时间和到期时间，同时递增管理查询版本 */
static void state(struct dppd_arp *table, struct dppd_arp_entry *entry, uint8_t value, uint64_t due)
{
    entry->state = value;
    entry->changed_ms = table->clock;
    entry->due_ms = due;
    advance_revision(table);
}
static struct dppd_arp_entry *insert(struct dppd_arp *table, uint16_t port, const uint8_t address[4])
{
    if (table->next_id == UINT64_MAX || table->status.revision == UINT64_MAX)
        return NULL;
    for (uint32_t i = 0; i < DPPD_ARP_LIMIT; ++i)
        if (table->entries[i].id == 0) {
            struct dppd_arp_entry *entry = &table->entries[i];
            *entry = (struct dppd_arp_entry){.id = ++table->next_id, .port_id = port, .used_ms = table->clock};
            memcpy(entry->address, address, 4);
            state(table, entry, DPPD_ARP_INCOMPLETE, table->clock);
            table->status.active++;
            return entry;
        }
    return NULL;
}

static void put16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}
/** 构造以太网 IPv4 ARP，标签属于出口域，补零到最短帧长度而不泄露栈内容 */
static int transmit(struct dppd_arp *table, const struct local_address *owner, uint16_t operation,
    const uint8_t source[4], const uint8_t target[4], const uint8_t *destination,
    dppd_arp_send send, void *context)
{
    uint8_t wire[60] = {0};
    unsigned int l2 = 14 + 4 * owner->configuration.vlan_depth;
    memcpy(wire, destination == NULL ? broadcast : destination, 6);
    memcpy(wire + 6, owner->mac, 6);
    for (unsigned int i = 0; i < owner->configuration.vlan_depth; ++i) {
        put16(wire + 12 + i * 4, owner->configuration.vlan_depth == 2 && i == 0 ? 0x88a8 : 0x8100);
        put16(wire + 14 + i * 4, owner->configuration.vlan[i]);
    }
    put16(wire + l2 - 2, 0x0806);
    uint8_t *arp = wire + l2;
    put16(arp, 1);
    put16(arp + 2, 0x0800);
    arp[4] = 6;
    arp[5] = 4;
    put16(arp + 6, operation);
    memcpy(arp + 8, owner->mac, 6);
    memcpy(arp + 14, source, 4);
    if (operation == 2 && destination != NULL)
        memcpy(arp + 18, destination, 6);
    memcpy(arp + 24, target, 4);
    int rc = send == NULL ? -ENOSYS : send(context, owner->configuration.port_id, wire, sizeof(wire));
    if (rc != 0)
        table->status.tx_errors++;
    else if (operation == 1)
        table->status.requests++;
    else
        table->status.replies++;
    return rc;
}
/** 这里只核对策略本身，实际端口存在性和 MAC 由创建容器时的设备绑定另行核对 */
int dppd_arp_policy_validate(const struct dppd_arp_policy *policy)
{
    if (policy == NULL || policy->local_count > DPPD_ARP_LOCAL_LIMIT)
        return -EINVAL;
    if (policy->local_count == 0)
        return policy->reachable_ms == 0 ? 0 : -EINVAL;
    if (policy->reachable_ms < 1000 || policy->reachable_ms > 3600000)
        return -EINVAL;
    for (uint32_t i = 0; i < policy->local_count; ++i) {
        const struct dppd_arp_local *local = &policy->locals[i];
        if (local->port_id == UINT16_MAX || !unicast(local->address) || local->vlan_depth > 2)
            return -EINVAL;
        for (uint8_t tag = 0; tag < local->vlan_depth; ++tag)
            if (local->vlan[tag] == 0 || local->vlan[tag] > 4094)
                return -EINVAL;
        for (uint32_t j = 0; j < i; ++j) {
            const struct dppd_arp_local *other = &policy->locals[j];
            if (local->port_id == other->port_id && (equal(local->address, other->address) ||
                    local->vlan_depth != other->vlan_depth ||
                    memcmp(local->vlan, other->vlan, local->vlan_depth * sizeof(local->vlan[0])) != 0))
                return -EINVAL;
        }
    }
    return 0;
}

/** 中断时继续读取随机字节，不用时钟或递增计数冒充跨进程启动身份 */
static int random_bytes(void *value, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = getrandom((uint8_t *)value + offset, size - offset, 0);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (count == 0)
            return -EIO;
        offset += (size_t)count;
    }
    return 0;
}

int dppd_arp_create_policy(struct dppd_arp **result, const struct dppd_arp_policy *policy,
    const struct dppd_arp_port *ports, uint16_t count, uint64_t now)
{
    if (result == NULL || *result != NULL || ports == NULL || count == 0 || count > DPPD_MAX_PORTS)
        return -EINVAL;
    int rc = dppd_arp_policy_validate(policy);
    if (rc != 0)
        return rc;
    for (uint16_t i = 0; i < count; ++i) {
        if (ports[i].port_id == UINT16_MAX || !mac_valid(ports[i].mac))
            return -EINVAL;
        for (uint16_t j = 0; j < i; ++j)
            if (ports[i].port_id == ports[j].port_id)
                return -EINVAL;
    }
    /** 清零后邻居和等待包槽位为空，本机地址 state 为枚举值 0，即 TENTATIVE */
    struct dppd_arp *table = calloc(1, sizeof(*table));
    if (table == NULL)
        return -ENOMEM;
    rc = random_bytes(&table->random, sizeof(table->random));
    while (rc == 0 && table->status.epoch == 0)
        rc = random_bytes(&table->status.epoch, sizeof(table->status.epoch));
    if (rc != 0) {
        free(table);
        return rc;
    }
    rc = pthread_mutex_init(&table->lock, NULL);
    if (rc != 0) {
        free(table);
        return -rc;
    }
    /** 随机种子用于打散地址检测时间，端口 MAC 不随策略替换而改变 */
    if (table->random == 0)
        table->random = 1;
    table->clock = now;
    table->status.enabled = policy->local_count != 0;
    table->status.revision = 1;
    table->status.local_count = policy->local_count;
    table->status.reachable_ms = policy->reachable_ms;
    memcpy(table->ports, ports, count * sizeof(*ports));
    table->port_count = count;
    for (uint32_t i = 0; i < policy->local_count; ++i) {
        struct local_address *owner = &table->locals[i];
        owner->configuration = policy->locals[i];
        for (uint16_t j = 0; j < count; ++j)
            if (ports[j].port_id == owner->configuration.port_id)
                memcpy(owner->mac, ports[j].mac, 6);
        if (!mac_valid(owner->mac)) {
            dppd_arp_destroy(table);
            return -ENODEV;
        }
        /** 首次检测安排在 now 到 now+1000 毫秒之间，实际发送要等 worker 驱动 tick */
        owner->due = now + random_value(table) % 1001;
    }
    /** 全部本机地址和 MAC 准备成功，才将表交给 runtime 持有 */
    *result = table;
    return 0;
}

int dppd_arp_replace(struct dppd_arp *table, const struct dppd_arp_policy *policy, bool reset_neighbors,
    uint64_t expected_epoch, uint64_t expected_revision, uint64_t now, int (*publish)(void *), void *context)
{
    if (table == NULL)
        return -ENODEV;
    if (expected_epoch == 0 || expected_revision == 0)
        return -EINVAL;
    struct dppd_arp *candidate = NULL;
    int rc = dppd_arp_create_policy(&candidate, policy, table->ports, table->port_count, now);
    if (rc != 0)
        return rc;
    pthread_mutex_lock(&table->lock);
    if (expected_epoch != table->status.epoch || expected_revision != table->status.revision)
        rc = -ESTALE;
    else if (!reset_neighbors && (table->status.active != 0 || table->status.pending != 0))
        rc = -EBUSY;
    else if (table->status.revision == UINT64_MAX || UINT64_MAX - table->status.queue_drops < table->status.pending)
        rc = -EOVERFLOW;
    else if (publish != NULL)
        rc = publish(context);
    if (rc == 0) {
        /** 回调成功前不动旧对象，成功后把旧等待包交给候选析构，地址和 mutex 始终保持不变 */
        memcpy(candidate->waiting, table->waiting, sizeof(table->waiting));
        memset(table->waiting, 0, sizeof(table->waiting));
        memset(table->entries, 0, sizeof(table->entries));
        memcpy(table->locals, candidate->locals, sizeof(table->locals));
        clock_advance(table, now);
        for (uint32_t i = 0; i < candidate->status.local_count; ++i)
            table->locals[i].due = table->clock + random_value(table) % 1001;
        table->status.queue_drops += table->status.pending;
        table->status.pending = table->status.active = 0;
        table->status.enabled = candidate->status.enabled;
        table->status.local_count = candidate->status.local_count;
        table->status.reachable_ms = candidate->status.reachable_ms;
        table->next_tick = 0;
        advance_revision(table);
    }
    pthread_mutex_unlock(&table->lock);
    dppd_arp_destroy(candidate);
    return rc;
}
void dppd_arp_destroy(struct dppd_arp *table)
{
    if (table == NULL)
        return;
    for (uint32_t i = 0; i < DPPD_ARP_PENDING_LIMIT; ++i)
        if (table->waiting[i].packet.mbuf != NULL)
            rte_pktmbuf_free(table->waiting[i].packet.mbuf);
    pthread_mutex_destroy(&table->lock);
    free(table);
}

void dppd_arp_discard_pending(struct dppd_arp *table)
{
    if (table == NULL)
        return;
    struct rte_mbuf *retired[DPPD_ARP_PENDING_LIMIT] = {0};
    pthread_mutex_lock(&table->lock);
    for (uint32_t i = 0; i < DPPD_ARP_PENDING_LIMIT; ++i)
        retired[i] = table->waiting[i].packet.mbuf;
    memset(table->waiting, 0, sizeof(table->waiting));
    if (table->status.pending != 0) {
        /** 结束清理不能因统计或版本触顶而留下借用内存，达到上限就保留最大值 */
        table->status.queue_drops = UINT64_MAX - table->status.queue_drops < table->status.pending ? UINT64_MAX :
            table->status.queue_drops + table->status.pending;
        advance_revision(table);
    }
    table->status.pending = 0;
    pthread_mutex_unlock(&table->lock);
    /** 锁内先摘掉指针，锁外归还引用，之后销毁容器或再次结束清理都不会重复释放 */
    for (uint32_t i = 0; i < DPPD_ARP_PENDING_LIMIT; ++i)
        if (retired[i] != NULL)
            rte_pktmbuf_free(retired[i]);
}

/** 该网口配置的本机 IPv4 地址全部为 PREFERRED 才就绪；未启用 ARP 或该口无配置时不限制 */
bool dppd_arp_port_ready(struct dppd_arp *table, uint16_t port)
{
    if (table == NULL)
        return true;
    pthread_mutex_lock(&table->lock);
    bool ready = !table->status.enabled || table->status.revision != UINT64_MAX;
    for (uint32_t i = 0; i < table->status.local_count; ++i)
        if (table->locals[i].configuration.port_id == port && table->locals[i].state != DPPD_ARP_PREFERRED)
            ready = false;
    pthread_mutex_unlock(&table->lock);
    return ready;
}


/** 先核对线缆格式与地址域，再处理冲突和学习，只回应明确配置的本机地址 */
bool dppd_arp_input(struct dppd_arp *table, uint16_t port, const struct rte_mbuf *mbuf,
    const struct dppd_packet *packet, uint64_t now, dppd_arp_send send, void *context)
{
    if (table == NULL || packet->l3_type != DPPD_L3_ARP)
        return false;
    pthread_mutex_lock(&table->lock);
    if (!table->status.enabled) {
        pthread_mutex_unlock(&table->lock);
        return false;
    }
    clock_advance(table, now);
    struct local_address *owner = local(table, port, NULL);
    uint8_t buffer[50];
    if (packet->l2_len < 14 || packet->l2_len > 22)
        goto invalid;
    const uint8_t *wire = rte_pktmbuf_read(mbuf, 0, packet->l2_len + 28, buffer);
    if (table->status.revision == UINT64_MAX || !domain(owner, packet) || wire == NULL)
        goto invalid;
    const uint8_t *arp = wire + packet->l2_len;
    if (memcmp(arp, (uint8_t[]){0, 1, 8, 0, 6, 4, 0}, 7) != 0 || (arp[7] != 1 && arp[7] != 2) ||
        !mac_valid(arp + 8) || memcmp(wire + 6, arp + 8, 6) != 0 ||
        (memcmp(wire, owner->mac, 6) != 0 && memcmp(wire, broadcast, 6) != 0) ||
        (!unicast(arp + 14) && !equal(arp + 14, zero)) ||
        (!unicast(arp + 24) && !equal(arp + 24, zero)))
        goto invalid;
    /** 自己的广播回环不能被当成另一台主机的冲突证据 */
    if (memcmp(arp + 8, owner->mac, 6) == 0)
        goto done;
    bool conflict = false;
    for (uint32_t i = 0; i < table->status.local_count; ++i) {
        struct local_address *candidate = &table->locals[i];
        if (candidate->configuration.port_id != port)
            continue;
        if (equal(arp + 14, candidate->configuration.address) ||
            (candidate->state == DPPD_ARP_TENTATIVE && arp[7] == 1 && equal(arp + 14, zero) &&
                equal(arp + 24, candidate->configuration.address))) {
            conflict = true;
            if (candidate->state != DPPD_ARP_DUPLICATE) {
                candidate->state = DPPD_ARP_DUPLICATE;
                advance_revision(table);
            }
        }
    }
    if (conflict)
        goto done;
    struct local_address *target = local(table, port, arp + 24);
    if (target == NULL || target->state != DPPD_ARP_PREFERRED)
        goto done;
    if (arp[7] == 2 && (equal(arp + 14, zero) || memcmp(arp + 18, target->mac, 6) != 0))
        goto invalid;
    /** 只刷新已由转发请求创建的条目，未请求的广播不能凭空填满缓存或替换已确认 MAC */
    struct dppd_arp_entry *entry = find(table, port, arp + 14);
    if (entry != NULL && (entry->state == DPPD_ARP_INCOMPLETE || entry->state == DPPD_ARP_PROBE)) {
        memcpy(entry->mac, arp + 8, 6);
        state(table, entry, DPPD_ARP_REACHABLE, table->clock + table->status.reachable_ms);
        table->status.resolved++;
    }
    if (arp[7] == 1)
        (void)transmit(table, target, 2, target->configuration.address, arp + 14, arp + 8, send, context);
    goto done;
invalid:
    table->status.invalid++;
done:
    pthread_mutex_unlock(&table->lock);
    return true;
}
int dppd_arp_resolve(struct dppd_arp *table, uint16_t port, const uint8_t address[4], const struct dppd_packet *packet,
    uint64_t now, uint8_t mac[6])
{
    if (table == NULL)
        return -EHOSTUNREACH;
    pthread_mutex_lock(&table->lock);
    clock_advance(table, now);
    int rc = -EHOSTUNREACH;
    if (table->status.revision == UINT64_MAX) {
        rc = -EOVERFLOW;
        goto done;
    }
    if (!domain(local(table, port, NULL), packet) || !unicast(address) ||
        local(table, port, address) != NULL)
        goto done;
    struct dppd_arp_entry *entry = find(table, port, address);
    if (entry == NULL)
        entry = insert(table, port, address);
    if (entry == NULL) {
        rc = -ENOSPC;
        goto done;
    }
    entry->used_ms = table->clock;
    /** 失败冷却 5 秒后，只有再次查询才重启解析；tick 不会自动把 FAILED 改回 INCOMPLETE */
    if (entry->state == DPPD_ARP_FAILED && entry->due_ms <= table->clock) {
        entry->probes = 0;
        state(table, entry, DPPD_ARP_INCOMPLETE, table->clock);
    }
    if (entry->state == DPPD_ARP_REACHABLE && entry->due_ms <= table->clock)
        state(table, entry, DPPD_ARP_STALE, 0);
    /** 实际使用 STALE 才安排 5 秒后的探测，当前包仍可沿用 MAC；无流量时不主动进入 DELAY */
    if (entry->state == DPPD_ARP_STALE)
        state(table, entry, DPPD_ARP_DELAY, table->clock + 5000);
    if (entry->state == DPPD_ARP_INCOMPLETE)
        rc = -EAGAIN;
    else if (entry->state != DPPD_ARP_FAILED) {
        memcpy(mac, entry->mac, 6);
        rc = 0;
    }
done:
    pthread_mutex_unlock(&table->lock);
    return rc;
}


/** 三次探测和两次通告分步执行，发送失败保持等待，检测出冲突后由管理员修正配置 */
void dppd_arp_tick(struct dppd_arp *table, uint64_t now, dppd_arp_send send, void *context)
{
    if (table == NULL)
        return;
    pthread_mutex_lock(&table->lock);
    clock_advance(table, now);
    if (!table->status.enabled || table->status.revision == UINT64_MAX || table->clock < table->next_tick)
        goto done;
    table->next_tick = table->clock + 10;
    for (uint32_t i = 0; i < table->status.local_count && table->status.revision != UINT64_MAX; ++i) {
        struct local_address *owner = &table->locals[i];
        if (owner->state == DPPD_ARP_DUPLICATE || owner->due > table->clock || owner->announcements == 2)
            continue;
        bool probing = owner->probes < 3;
        int rc = transmit(table, owner, 1, probing ? zero : owner->configuration.address,
            owner->configuration.address, NULL, send, context);
        owner->due = table->clock + 1000;
        if (rc != 0)
            continue;
        if (probing) {
            owner->probes++;
            owner->due = table->clock + (owner->probes == 3 ? 2000 : 1000 + random_value(table) % 1001);
        } else {
            owner->announcements++;
            owner->due = table->clock + 2000;
            if (owner->state != DPPD_ARP_PREFERRED) {
                owner->state = DPPD_ARP_PREFERRED;
                advance_revision(table);
            }
        }
    }
    for (uint32_t i = 0; i < DPPD_ARP_LIMIT && table->status.revision != UINT64_MAX; ++i) {
        struct dppd_arp_entry *entry = &table->entries[i];
        if (entry->id == 0)
            continue;
        if (entry->state == DPPD_ARP_REACHABLE && entry->due_ms <= table->clock)
            state(table, entry, DPPD_ARP_STALE, 0);
        if ((entry->state == DPPD_ARP_STALE || entry->state == DPPD_ARP_FAILED) && table->clock - entry->used_ms >= 60000) {
            memset(entry, 0, sizeof(*entry));
            table->status.active--;
            advance_revision(table);
            continue;
        }
        if (entry->state == DPPD_ARP_DELAY && entry->due_ms <= table->clock) {
            entry->probes = 0;
            state(table, entry, DPPD_ARP_PROBE, table->clock);
        }
        if ((entry->state != DPPD_ARP_INCOMPLETE && entry->state != DPPD_ARP_PROBE) || entry->due_ms > table->clock)
            continue;
        struct local_address *owner = local(table, entry->port_id, NULL);
        if (owner == NULL || owner->state != DPPD_ARP_PREFERRED)
            continue;
        if (entry->probes == 3) {
            state(table, entry, DPPD_ARP_FAILED, table->clock + 5000);
            table->status.failed++;
            continue;
        }
        /** 刷新时重新广播，允许下一跳换网卡后返回新 MAC，重试次数仍然有限 */
        (void)transmit(table, owner, 1, owner->configuration.address, entry->address, NULL, send, context);
        entry->probes++;
        entry->due_ms = table->clock + 1000;
    }
done:
    pthread_mutex_unlock(&table->lock);
}
int dppd_arp_hold(struct dppd_arp *table, uint16_t port, const uint8_t address[4], struct dppd_arp_pending pending,
    uint64_t now)
{
    if (table == NULL || pending.mbuf == NULL)
        return -EINVAL;
    pthread_mutex_lock(&table->lock);
    clock_advance(table, now);
    if (table->status.revision == UINT64_MAX) {
        pthread_mutex_unlock(&table->lock);
        return -EOVERFLOW;
    }
    struct dppd_arp_entry *entry = find(table, port, address);
    /** 记录首个空槽位，并统计已有多少个包在等待同一邻居，避免单个邻居占满队列 */
    uint32_t same = 0, available = DPPD_ARP_PENDING_LIMIT;
    for (uint32_t i = 0; i < DPPD_ARP_PENDING_LIMIT; ++i) {
        if (table->waiting[i].packet.mbuf == NULL) {
            if (available == DPPD_ARP_PENDING_LIMIT)
                available = i;
        } else if (entry != NULL && table->waiting[i].neighbor_id == entry->id)
            same++;
    }
    int rc = -ENOBUFS;
    if (entry != NULL && same < 4 && available != DPPD_ARP_PENDING_LIMIT &&
        (pending.expires_ms == 0 || pending.expires_ms > table->clock)) {
        if (pending.expires_ms == 0)
            pending.expires_ms = table->clock + 5000;
        /** 保存记录和原始 mbuf 指针，不复制报文字节；仍未解析完成时，最早 100 毫秒后取出重试 */
        table->waiting[available] =
            (struct waiting_packet){.packet = pending, .neighbor_id = entry->id, .retry_ms = table->clock + 100};
        table->status.pending++;
        advance_revision(table);
        rc = 0;
    } else
        table->status.queue_drops++;
    pthread_mutex_unlock(&table->lock);
    return rc;
}
/**
 * 按 queue 取出本队列一个可处理的等待包，now 用于判断重试和超时，pending 接收记录
 * 可取出：邻居已有 MAC、INCOMPLETE 重试到期、报文或邻居记录失效
 * true → 复制记录并清空槽位，mbuf 交回调用方，expired 表示须丢弃；不保证可直接转发
 * false → 当前没有可取记录，不等待，调用方不能使用 pending 中的内容
 */
bool dppd_arp_take(struct dppd_arp *table, uint16_t queue, uint64_t now, struct dppd_arp_pending *pending)
{
    if (table == NULL)
        return false;
    pthread_mutex_lock(&table->lock);
    clock_advance(table, now);
    bool found = false;
    for (uint32_t i = 0; i < DPPD_ARP_PENDING_LIMIT && table->status.revision != UINT64_MAX; ++i) {
        struct waiting_packet *waiting = &table->waiting[i];
        /** 只取本队列的有效记录，避免把同一 mbuf 同时交给不同处理队列 */
        if (waiting->packet.mbuf == NULL || waiting->packet.queue != queue)
            continue;
        /** 按入队时保存的邻居 ID 查找，不能仅凭槽位下标，否则复用槽位会串到另一个邻居 */
        struct dppd_arp_entry *entry = NULL;
        for (uint32_t j = 0; j < DPPD_ARP_LIMIT; ++j)
            if (table->entries[j].id == waiting->neighbor_id)
                entry = &table->entries[j];
        /** 超时、邻居记录消失或解析已失败的包也要取出，标记 expired 后交给调用方释放 */
        bool expired = waiting->packet.expires_ms <= table->clock || entry == NULL || entry->state == DPPD_ARP_FAILED;
        /** 仍在等待 MAC 且未到重试时间才继续留队；MAC 已可用或记录失效时可提前取出 */
        if (!expired && entry->state == DPPD_ARP_INCOMPLETE && waiting->retry_ms > table->clock)
            continue;
        /** 只复制记录和 mbuf 指针，不复制报文字节；清空槽位后，等待数组不再持有这个包 */
        *pending = waiting->packet;
        pending->expired = expired;
        memset(waiting, 0, sizeof(*waiting));
        table->status.pending--;
        table->status.queue_drops += expired;
        advance_revision(table);
        found = true;
        break;
    }
    pthread_mutex_unlock(&table->lock);
    return found;
}
void dppd_arp_status(struct dppd_arp *table, struct dppd_arp_status *status)
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
static int list_neighbors(struct dppd_arp *table, uint64_t after, uint64_t epoch, uint64_t revision,
    bool guarded, struct dppd_arp_page *page)
{
    if (table == NULL || page == NULL)
        return -ENODEV;
    if (guarded && ((epoch == 0) != (revision == 0) || (after != 0 && epoch == 0)))
        return -EINVAL;
    memset(page, 0, sizeof(*page));
    pthread_mutex_lock(&table->lock);
    page->epoch = table->status.epoch;
    page->revision = table->status.revision;
    int rc = (revision != 0 && revision != page->revision) || (guarded && epoch != 0 && epoch != page->epoch)
        ? -ESTALE : 0;
    for (uint32_t i = 0; rc == 0 && i < DPPD_ARP_LIMIT; ++i) {
        const struct dppd_arp_entry *entry = &table->entries[i];
        if (entry->id <= after)
            continue;
        uint32_t position = 0;
        while (position < page->count && page->entries[position].id < entry->id)
            position++;
        if (page->count == DPPD_ARP_PAGE_SIZE) {
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
int dppd_arp_list(struct dppd_arp *table, uint64_t after, uint64_t revision, struct dppd_arp_page *page)
{
    return list_neighbors(table, after, 0, revision, false, page);
}

int dppd_arp_list_guarded(struct dppd_arp *table, uint64_t after, uint64_t epoch, uint64_t revision,
    struct dppd_arp_page *page)
{
    return list_neighbors(table, after, epoch, revision, true, page);
}

static int flush_neighbors(struct dppd_arp *table, uint64_t epoch, uint64_t revision, bool guarded)
{
    if (table == NULL)
        return -ENODEV;
    struct rte_mbuf *retired[DPPD_ARP_PENDING_LIMIT] = {0};
    pthread_mutex_lock(&table->lock);
    int rc = revision == 0 || (guarded && epoch == 0) ? -EINVAL :
        revision != table->status.revision || (guarded && epoch != table->status.epoch) ? -ESTALE : 0;
    if (rc == 0 && (table->status.revision == UINT64_MAX ||
            (guarded && UINT64_MAX - table->status.queue_drops < table->status.pending)))
        rc = -EOVERFLOW;
    if (rc == 0) {
        memset(table->entries, 0, sizeof(table->entries));
        table->status.active = 0;
        if (guarded) {
            /** 只释放仍属于队列的包，已经由 take 交还给 worker 的包不会出现在这里 */
            for (uint32_t i = 0; i < DPPD_ARP_PENDING_LIMIT; ++i)
                retired[i] = table->waiting[i].packet.mbuf;
            memset(table->waiting, 0, sizeof(table->waiting));
            table->status.queue_drops += table->status.pending;
            table->status.pending = 0;
        }
        advance_revision(table);
    }
    pthread_mutex_unlock(&table->lock);
    for (uint32_t i = 0; i < DPPD_ARP_PENDING_LIMIT; ++i)
        if (retired[i] != NULL)
            rte_pktmbuf_free(retired[i]);
    return rc;
}

int dppd_arp_flush(struct dppd_arp *table, uint64_t revision)
{
    return flush_neighbors(table, 0, revision, false);
}

int dppd_arp_flush_guarded(struct dppd_arp *table, uint64_t epoch, uint64_t revision)
{
    return flush_neighbors(table, epoch, revision, true);
}
