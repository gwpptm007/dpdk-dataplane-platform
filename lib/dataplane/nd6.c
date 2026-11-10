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
    /** 容器生命周期内固定的设备绑定，在线策略只能引用这些端口 */
    struct dppd_nd6_port ports[DPPD_MAX_PORTS];
    uint16_t port_count;
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
/** 版本触顶后保持最大值，后续写入口拒绝推进，旧版本不能因整数回绕重新命中 */
static void advance_revision(struct dppd_nd6 *table)
{
    if (table->status.revision != UINT64_MAX)
        table->status.revision++;
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
/** 按出口网口和邻居 IPv6 查动态缓存，只查已有记录，与 local 查本机地址分别使用 */
static struct dppd_nd6_entry *find(struct dppd_nd6 *table, uint16_t port, const uint8_t address[16])
{
    for (uint32_t i = 0; i < DPPD_ND6_LIMIT; ++i)
        if (table->entries[i].id != 0 && table->entries[i].port_id == port && equal(table->entries[i].address, address))
            return &table->entries[i];
    return NULL;
}
/** 更新邻居状态、变更时间和到期时间，同时递增管理查询版本 */
static void state(struct dppd_nd6 *table, struct dppd_nd6_entry *entry, uint8_t value, uint64_t due)
{
    entry->state = value;
    entry->changed_ms = table->clock;
    entry->due_ms = due;
    advance_revision(table);
}
static struct dppd_nd6_entry *insert(struct dppd_nd6 *table, uint16_t port, const uint8_t address[16])
{
    if (table->next_id == UINT64_MAX || table->status.revision == UINT64_MAX)
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
/** 用固定前缀 ff02::1:ff00:0/104 和目标地址的末 24 位生成 solicited-node 组播地址 */
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
/** 按网络字节序写入 16 位字段，高字节在前，不依赖主机字节序 */
static void put16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}
/**
 * 按 owner 的网口、MAC 和 VLAN 配置构造 NS 或 NA，将实际报文字节交给 send 回调
 * 以太网头 → IPv6 头 → ICMPv6 内容与校验和 → send(context, port, wire, length)
 * wire 是局部缓冲，回调必须在返回前完成复制或使用，不能保留其地址
 * 正式运行时 send 指向 worker.c 的 send_nd6，由它另分配 mbuf 并提交到 TX 队列
 * 返回回调结果，未提供回调返回 -ENOSYS，同时按结果更新 ND 发送统计
 *
 * 负责组装完整的 ND 报文，包括以太网头、IPv6 头和 ICMPv6 内容， 由send_nd6 回调发送, 装进 mbuf 并提交到 TX 队列 交给驱动
 */
static int transmit(struct dppd_nd6 *table, const struct local_address *owner, uint8_t type, const uint8_t source[16],
    const uint8_t destination[16], const uint8_t target[16], const uint8_t *destination_mac, uint8_t flags,
    dppd_nd6_send send, void *context)
{
    uint8_t wire[110] = {0};
    /** l2 是 IPv6 头的起始偏移：无 VLAN 为 14，每层标签再加 4 字节 */
    unsigned int l2 = 14 + 4 * owner->configuration.vlan_depth;
    if (destination_mac != NULL)
        memcpy(wire, destination_mac, 6);
    else {
        /** 未提供单播 MAC 时，按 IPv6 组播规则取 33:33 加目的 IPv6 的最后 4 字节 */
        wire[0] = wire[1] = 0x33;
        memcpy(wire + 2, destination + 12, 4);
    }
    memcpy(wire + 6, owner->mac, 6);
    for (unsigned int i = 0; i < owner->configuration.vlan_depth; ++i) {
        put16(wire + 12 + i * 4, owner->configuration.vlan_depth == 2 && i == 0 ? 0x88a8 : 0x8100);
        put16(wire + 14 + i * 4, owner->configuration.vlan[i]);
    }
    /**
     * 整包偏移  0              14                  54               78
                ├─ 以太网头 14 ─┼──── IPv6 头 40 ────┼─ ICMPv6 NS 24 ─┤
        wire    ↑              ↑                    ↑
                               ip                 body
     */
    put16(wire + l2 - 2, 0x86dd);
    /** 无 VLAN 时布局：[以太网 14 字节][IPv6 40 字节][ICMPv6，从整包偏移 54 开始] */
    uint8_t *ip = wire + l2, *body = ip + 40;
    /** 源为 :: 的 DAD NS 只有 24 字节基础内容；其他调用再带 8 字节 MAC 选项 */
    unsigned int length = equal(source, zero) ? 24 : 32;
    ip[0] = 0x60;
    put16(ip + 4, (uint16_t)length);
    /** Next Header 58 表示 ICMPv6，ND 的 Hop Limit 固定为 255 */
    ip[6] = 58;
    ip[7] = 255;
    memcpy(ip + 8, source, 16);
    memcpy(ip + 24, destination, 16);
    /** 基础内容：[类型 1][Code 1][校验和 2][保留位或 NA 标志 4][目标 IPv6 16] */
    body[0] = type;
    body[4] = flags;
    memcpy(body + 8, target, 16);
    if (length == 32) {
        body[24] = type == DPPD_ICMPV6_NEIGHBOR_SOLICITATION ? 1 : 2;
        /** 选项长度以 8 字节为单位，1 表示类型、长度和 MAC 共占 8 字节 */
        body[25] = 1;
        memcpy(body + 26, owner->mac, 6);
    }
    /** 校验和覆盖 IPv6 伪首部与 ICMPv6 内容；只发送实际帧长，不发送整个 wire 缓冲 */
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

/** 这里只核对策略本身，实际端口存在性和 MAC 由创建容器时的设备绑定另行核对 */
int dppd_nd6_policy_validate(const struct dppd_nd6_policy *policy)
{
    if (policy == NULL || policy->local_count > DPPD_ND6_LOCAL_LIMIT)
        return -EINVAL;
    if (policy->local_count == 0)
        return policy->reachable_ms == 0 ? 0 : -EINVAL;
    if (policy->reachable_ms < 1000 || policy->reachable_ms > 3600000)
        return -EINVAL;
    for (uint32_t i = 0; i < policy->local_count; ++i) {
        const struct dppd_nd6_local *local = &policy->locals[i];
        if (local->port_id == UINT16_MAX || !dppd_ipv6_unicast(local->address, true) || local->vlan_depth > 2)
            return -EINVAL;
        for (uint8_t tag = 0; tag < local->vlan_depth; ++tag)
            if (local->vlan[tag] == 0 || local->vlan[tag] > 4094)
                return -EINVAL;
        for (uint32_t j = 0; j < i; ++j) {
            const struct dppd_nd6_local *other = &policy->locals[j];
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

int dppd_nd6_create_policy(struct dppd_nd6 **result, const struct dppd_nd6_policy *policy,
    const struct dppd_nd6_port *ports, uint16_t count, uint64_t now)
{
    if (result == NULL || *result != NULL || ports == NULL || count == 0 || count > DPPD_MAX_PORTS)
        return -EINVAL;
    int rc = dppd_nd6_policy_validate(policy);
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
    struct dppd_nd6 *table = calloc(1, sizeof(*table));
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
    /** 随机种子用于打散检测和可达期限，端口 MAC 不随策略替换而改变 */
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
            dppd_nd6_destroy(table);
            return -ENODEV;
        }
        /** 首次检测安排在 now 到 now+1000 毫秒之间，实际发送要等 worker 驱动 tick */
        owner->due = now + random_value(table) % 1001;
    }
    /** 全部本机地址和 MAC 准备成功，才将表交给 runtime 持有 */
    *result = table;
    return 0;
}

/**
 * 兼容旧启动配置，MAC 数组仍按 configuration->ports 顺序排列
 * 只绑定实际启用 ND 的端口，其他端口缺少 MAC 不影响原本可用的 ND 配置
 * 本函数不发送报文，所有本机地址仍由 worker 的 tick 推进重复地址检测
 */
int dppd_nd6_create(struct dppd_nd6 **result, const struct dppd_config *configuration,
    const uint8_t macs[DPPD_MAX_PORTS][6], uint64_t now)
{
    if (result == NULL || *result != NULL || configuration == NULL || macs == NULL)
        return -EINVAL;
    /** 旧独立模式仍用 NULL 表示未启用，不能因为统一管理引入容器就改变旧入口行为 */
    if (configuration->nd6_count == 0)
        return 0;
    char error[128];
    if (dppd_config_validate(configuration, error, sizeof(error)) != 0)
        return -EINVAL;
    struct dppd_nd6_policy policy = {.local_count = configuration->nd6_count,
        .reachable_ms = configuration->nd6_reachable_ms};
    memcpy(policy.locals, configuration->nd6, policy.local_count * sizeof(policy.locals[0]));
    struct dppd_nd6_port ports[DPPD_MAX_PORTS] = {0};
    uint16_t count = 0;
    for (uint16_t i = 0; i < configuration->nb_ports; ++i) {
        bool used = false;
        for (uint32_t j = 0; j < policy.local_count; ++j)
            used |= policy.locals[j].port_id == configuration->ports[i];
        if (used) {
            ports[count].port_id = configuration->ports[i];
            memcpy(ports[count++].mac, macs[i], 6);
        }
    }
    return dppd_nd6_create_policy(result, &policy, ports, count, now);
}

int dppd_nd6_replace(struct dppd_nd6 *table, const struct dppd_nd6_policy *policy, bool reset_neighbors,
    uint64_t expected_epoch, uint64_t expected_revision, uint64_t now, int (*publish)(void *), void *context)
{
    if (table == NULL)
        return -ENODEV;
    if (expected_epoch == 0 || expected_revision == 0)
        return -EINVAL;
    struct dppd_nd6 *candidate = NULL;
    int rc = dppd_nd6_create_policy(&candidate, policy, table->ports, table->port_count, now);
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
    dppd_nd6_destroy(candidate);
    return rc;
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

void dppd_nd6_discard_pending(struct dppd_nd6 *table)
{
    if (table == NULL)
        return;
    struct rte_mbuf *retired[DPPD_ND6_PENDING_LIMIT] = {0};
    pthread_mutex_lock(&table->lock);
    for (uint32_t i = 0; i < DPPD_ND6_PENDING_LIMIT; ++i)
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
    for (uint32_t i = 0; i < DPPD_ND6_PENDING_LIMIT; ++i)
        if (retired[i] != NULL)
            rte_pktmbuf_free(retired[i]);
}

/** 该网口配置的本机 IPv6 地址全部为 PREFERRED 才就绪；未启用 ND 或该口无配置时不限制 */
bool dppd_nd6_port_ready(struct dppd_nd6 *table, uint16_t port)
{
    if (table == NULL)
        return true;
    pthread_mutex_lock(&table->lock);
    bool ready = !table->status.enabled || table->status.revision != UINT64_MAX;
    for (uint32_t i = 0; i < table->status.local_count; ++i)
        if (table->locals[i].configuration.port_id == port && table->locals[i].state != DPPD_ND6_PREFERRED)
            ready = false;
    pthread_mutex_unlock(&table->lock);
    return ready;
}

/**
 * 校验收到的 NS/NA，处理本机地址冲突、NS 应答及已有动态邻居的更新
 * 定位 ICMPv6 → 核对入口和 VLAN、报文格式、地址及选项 → 更新状态或回复 NA
 * packet 是前面的解析结果，mbuf 提供实际字节，send/context 沿用 worker 的发送回调
 * true 表示本包已处理，包含校验失败；false 表示不属于本次处理范围
 * 本函数只借用 mbuf，返回 true 后由 process_batch 释放，不把这个 mbuf 留入等待队列
 */
bool dppd_nd6_input(struct dppd_nd6 *table, uint16_t port, const struct rte_mbuf *mbuf,
    const struct dppd_packet *packet, uint64_t now, dppd_nd6_send send, void *context)
{
    if (table == NULL || packet->l3_type != DPPD_L3_IPV6 || packet->ip_protocol != 58)
        return false;
    pthread_mutex_lock(&table->lock);
    /** 常驻但关闭的容器与 NULL 语义相同，不读取或消费普通转发路径中的 ND 报文 */
    if (!table->status.enabled) {
        pthread_mutex_unlock(&table->lock);
        return false;
    }
    uint8_t first[1], ether[22], hop[1], body[256];
    /** l3_len 包含已解析的 IPv6 扩展头；无 VLAN、无扩展头时 ICMPv6 从 14+40=54 开始 */
    uint32_t offset = packet->l2_len + packet->l3_len;
    const uint8_t *kind = rte_pktmbuf_read(mbuf, offset, 1, first);
    /** 普通非 ND 报文交回原流程；ICMPv6 分片进入下面的无效处理，不按完整 ND 采用 */
    if (packet->l4_type != DPPD_L4_FRAGMENT && (kind == NULL || !dppd_icmpv6_is_nd(*kind))) {
        pthread_mutex_unlock(&table->lock);
        return false;
    }
    if (table->status.revision == UINT64_MAX)
        goto invalid;
    clock_advance(table, now);
    struct local_address *owner = local(table, port, NULL);
    if (packet->l2_len < 14 || packet->l2_len > 22 || packet->l3_len < 40 ||
        packet->ipv6_payload_len < packet->l3_len - 40)
        goto invalid;
    /** IPv6 Payload Length 包含扩展头，减去扩展头部分才是本次 ICMPv6 长度 */
    uint32_t length = 40U + packet->ipv6_payload_len - packet->l3_len;
    const uint8_t *link = rte_pktmbuf_read(mbuf, 0, packet->l2_len, ether);
    const uint8_t *ttl = rte_pktmbuf_read(mbuf, packet->l2_len + 7, 1, hop);
    const uint8_t *icmp = length <= sizeof(body) ? rte_pktmbuf_read(mbuf, offset, length, body) : NULL;
    /** 先核对入口与 VLAN、实际可读长度、Hop Limit、NS/NA 类型、目标地址和校验和 */
    if (!domain(owner, packet) || packet->l2_len > sizeof(ether) || link == NULL || ttl == NULL || *ttl != 255 ||
        packet->ipv6_unsupported || packet->l4_type == DPPD_L4_FRAGMENT || icmp == NULL || length < 24 ||
        (icmp[0] != DPPD_ICMPV6_NEIGHBOR_SOLICITATION && icmp[0] != DPPD_ICMPV6_NEIGHBOR_ADVERTISEMENT) ||
        icmp[1] != 0 || !mac_valid(link + 6) ||
        !dppd_ipv6_unicast(icmp + 8, true) || checksum(packet->ipv6_src, packet->ipv6_dst, icmp, length) != 0)
        goto invalid;
    /** 忽略源 MAC 与本机相同的帧，避免本机检测请求回环后被当成冲突 */
    if (memcmp(link + 6, owner->mac, 6) == 0)
        goto consumed;
    bool ns = icmp[0] == DPPD_ICMPV6_NEIGHBOR_SOLICITATION, unspecified = equal(packet->ipv6_src, zero);
    uint8_t group[16];
    solicited(icmp + 8, group);
    /** 用 ICMPv6 的目标字段查本机地址，不是用 IPv6 目的地址；后者在 DAD 中是组播地址 */
    struct local_address *target = local(table, port, icmp + 8);
    bool multicast = packet->ipv6_dst[0] == 255;
    const uint8_t all_nodes[16] = {255, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    /** 再核对 NS/NA 的源、目的地址，以及目的 IPv6 与二层目的 MAC 的对应关系 */
    if ((!unspecified && !dppd_ipv6_unicast(packet->ipv6_src, true)) ||
        (!ns && (unspecified || (multicast && (icmp[4] & 0x40U) != 0))) ||
        (ns && (multicast ? !equal(packet->ipv6_dst, group) : !equal(packet->ipv6_dst, icmp + 8))) ||
        (!ns && multicast && !equal(packet->ipv6_dst, all_nodes)) ||
        (!multicast && local(table, port, packet->ipv6_dst) == NULL) ||
        (multicast ? link[0] != 0x33 || link[1] != 0x33 || memcmp(link + 2, packet->ipv6_dst + 12, 4) != 0
                   : memcmp(link, owner->mac, 6) != 0) ||
        (ns && unspecified && !multicast))
        goto invalid;
    /** 选项从第 24 字节开始，长度单位为 8 字节；MAC 选项还需匹配以太网源地址 */
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
    /**
     * 目标为本机待检测地址时：NA 表示地址已被声明，源为 :: 的 NS 表示也有人在检测该地址
     * 两者都标记为 DUPLICATE；普通源地址的 NS 只是查询 MAC，不据此判断地址已被占用
     */
    if (target != NULL && ((!ns && !unspecified) || (ns && unspecified && target->state == DPPD_ND6_TENTATIVE))) {
        if (target->state == DPPD_ND6_TENTATIVE) {
            target->state = DPPD_ND6_DUPLICATE;
            advance_revision(table);
        } else
            table->status.invalid++;
        goto consumed;
    }
    if (ns) {
        /** 只有查询本机且已经可用的地址才应答，其他目标或待检测地址直接消费 */
        if (target == NULL || target->state != DPPD_ND6_PREFERRED)
            goto consumed;
        /** 有有效源地址和 MAC 选项时学习请求者，不能把源为 :: 的 DAD 请求当成邻居记录 */
        if (!unspecified && option_mac != NULL && local(table, port, packet->ipv6_src) == NULL) {
            struct dppd_nd6_entry *entry = find(table, port, packet->ipv6_src);
            if (entry == NULL)
                entry = insert(table, port, packet->ipv6_src);
            if (entry != NULL && (memcmp(entry->mac, option_mac, 6) != 0 || entry->state == DPPD_ND6_FAILED)) {
                memcpy(entry->mac, option_mac, 6);
                /** 已知请求者 MAC，但收到查询不等于确认双向可达，先标记 STALE */
                state(table, entry, DPPD_ND6_STALE, 0);
            }
        }
        /**
         * 普通 NS：单播回复请求者；源为 :: 的 DAD NS：发往 ff02::1，不能回复给 ::
         * NA 标志：[R 0x80][S 0x40][O 0x20]，分别表示路由器、请求应答、允许覆盖缓存
         * 当前固定 R=1、O=1；普通应答为 0xe0，组播 DAD 应答清 S 后为 0xa0
         */
        transmit(table, target, DPPD_ICMPV6_NEIGHBOR_ADVERTISEMENT, target->configuration.address,
            unspecified ? all_nodes : packet->ipv6_src,
            icmp + 8, unspecified ? NULL : link + 6, unspecified ? 0xa0 : 0xe0, send, context);
    } else {
        /** NA 的目标字段指出被通告的邻居地址；只查已有记录，不为未知通告分配槽位 */
        struct dppd_nd6_entry *entry = find(table, port, icmp + 8);
        if (entry == NULL || (option_mac == NULL && !mac_valid(entry->mac)))
            goto consumed;
        bool changed = option_mac != NULL && memcmp(entry->mac, option_mac, 6) != 0;
        /** 非 INCOMPLETE 记录的 MAC 改变但 O=0 时保留旧值；原来可达的记录降为 STALE */
        if (entry->state != DPPD_ND6_INCOMPLETE && changed && (icmp[4] & 0x20U) == 0) {
            if (entry->state == DPPD_ND6_REACHABLE)
                state(table, entry, DPPD_ND6_STALE, 0);
            goto consumed;
        }
        if (option_mac != NULL)
            memcpy(entry->mac, option_mac, 6);
        /**
         * S=1 时确认 REACHABLE，有效期在基准值约 0.5 到 1.5 倍间随机
         * S=0 时仅将新学或变化的 MAC 记为 STALE，未变化的已有记录保持原状态
         */
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
    /** 无效报文也返回已处理，mbuf 仍由 worker 释放，不能继续进入转发流程 */
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
    if (table->status.revision == UINT64_MAX) {
        rc = -EOVERFLOW;
        goto done;
    }
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
    /** 失败冷却 5 秒后，只有再次查询才重启解析；tick 不会自动把 FAILED 改回 INCOMPLETE */
    if (entry->state == DPPD_ND6_FAILED && entry->due_ms <= table->clock) {
        entry->probes = 0;
        state(table, entry, DPPD_ND6_INCOMPLETE, table->clock);
    }
    if (entry->state == DPPD_ND6_REACHABLE && entry->due_ms <= table->clock)
        state(table, entry, DPPD_ND6_STALE, 0);
    /** 实际使用 STALE 才安排 5 秒后的探测，当前包仍可沿用 MAC；无流量时不主动进入 DELAY */
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

/**
 * 推进本机 IPv6 地址的重复地址检测，以及动态邻居的过期和探测，状态保存在 table
 * worker 的队列 0 反复调用，每次到达扫描期限才处理；send 与 context 用于发送控制报文
 * 本机地址：检测 NS 提交成功 → 等待 1 秒 → 仍为 TENTATIVE 则改为 PREFERRED
 * 发送失败则保留待检测状态重试，接收路径发现冲突则改为 DUPLICATE
 * 动态邻居：维护可达期限，推进 DELAY 到期后的单播探测，以及 INCOMPLETE 的组播解析
 * 每次解析或探测最多尝试 3 次、间隔 1 秒；预算耗尽转为 FAILED，并保留 5 秒冷却期限
 */
void dppd_nd6_tick(struct dppd_nd6 *table, uint64_t now, dppd_nd6_send send, void *context)
{
    if (table == NULL)
        return;
    pthread_mutex_lock(&table->lock);
    clock_advance(table, now);
    if (!table->status.enabled || table->status.revision == UINT64_MAX || table->clock < table->next_tick)
        goto done;
    /** 这里只检查期限，不休眠；未到期直接返回，由 worker 后续循环再次调用 */
    table->next_tick = table->clock + 10;
    for (uint32_t i = 0; i < table->status.local_count && table->status.revision != UINT64_MAX; ++i) {
        struct local_address *owner = &table->locals[i];
        /** 只处理已经到期的待检测地址，PREFERRED 和 DUPLICATE 都跳过 */
        if (owner->state != DPPD_ND6_TENTATIVE || owner->due > table->clock)
            continue;
        /** 已成功提交检测 NS，等待期限也已结束；仍未发现冲突，才允许使用该地址 */
        if (owner->sent) {
            owner->state = DPPD_ND6_PREFERRED;
            advance_revision(table);
        } else {
            uint8_t group[16];
            /** 检测本机地址是否被占用：源 IPv6 为 ::，目标字段为本机待检测地址，目的为其组播地址 */
            solicited(owner->configuration.address, group);
            owner->sent =
                transmit(table, owner, DPPD_ICMPV6_NEIGHBOR_SOLICITATION, zero, group,
                    owner->configuration.address, NULL, 0, send, context) == 0;
            /** 成功后等待冲突报文，失败后等待重发，两种情况均至少间隔 1 秒再处理 */
            owner->due = table->clock + 1000;
        }
    }
    /** 以下维护邻居缓存，与上面的本机地址检测分别处理 */
    for (uint32_t i = 0; i < DPPD_ND6_LIMIT && table->status.revision != UINT64_MAX; ++i) {
        struct dppd_nd6_entry *entry = &table->entries[i];
        if (entry->id == 0)
            continue;
        /** 可达期限结束只转 STALE，不清掉 MAC，也不立即发探测 */
        if (entry->state == DPPD_ND6_REACHABLE && entry->due_ms <= table->clock)
            state(table, entry, DPPD_ND6_STALE, 0);
        /** STALE/FAILED 自创建或最近 resolve 查询起已闲置 60 秒，清理邻居槽位，不在此释放等待包 */
        if ((entry->state == DPPD_ND6_STALE || entry->state == DPPD_ND6_FAILED) &&
            table->clock - entry->used_ms >= 60000) {
            memset(entry, 0, sizeof(*entry));
            table->status.active--;
            advance_revision(table);
            continue;
        }
        /** DELAY 的 5 秒等待到期，清零本轮计数；下面可在同一次扫描中发送首次单播探测 */
        if (entry->state == DPPD_ND6_DELAY && entry->due_ms <= table->clock) {
            entry->probes = 0;
            state(table, entry, DPPD_ND6_PROBE, table->clock);
        }
        if ((entry->state != DPPD_ND6_INCOMPLETE && entry->state != DPPD_ND6_PROBE) || entry->due_ms > table->clock)
            continue;
        struct local_address *owner = local(table, entry->port_id, NULL);
        /** 用于发送 NS 的本机地址尚未可用时，暂不发探测，也不增加尝试计数 */
        if (owner == NULL || owner->state != DPPD_ND6_PREFERRED)
            continue;
        /** 第三次尝试后仍等满 1 秒，到下次期限时才判失败；已有等待包由 take 标记失效 */
        if (entry->probes == 3) {
            state(table, entry, DPPD_ND6_FAILED, table->clock + 5000);
            table->status.failed++;
            continue;
        }
        uint8_t destination[16];
        solicited(entry->address, destination);
        /** INCOMPLETE 用组播询问未知 MAC；PROBE 已有缓存 MAC，改用单播确认对方是否仍可达 */
        bool unicast = entry->state == DPPD_ND6_PROBE;
        transmit(table, owner, DPPD_ICMPV6_NEIGHBOR_SOLICITATION, owner->configuration.address,
            unicast ? entry->address : destination,
            entry->address, unicast ? entry->mac : NULL, 0, send, context);
        /** 计数的是发送尝试，回调失败也消耗预算；下一次至少 1 秒后处理 */
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
 * 返回 -EINVAL、-ENOBUFS 或 -EOVERFLOW：没有接管 mbuf，调用方负责后续释放
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
    if (table->status.revision == UINT64_MAX) {
        pthread_mutex_unlock(&table->lock);
        return -EOVERFLOW;
    }
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
bool dppd_nd6_take(struct dppd_nd6 *table, uint16_t queue, uint64_t now, struct dppd_nd6_pending *pending)
{
    if (table == NULL)
        return false;
    pthread_mutex_lock(&table->lock);
    clock_advance(table, now);
    bool found = false;
    for (uint32_t i = 0; i < DPPD_ND6_PENDING_LIMIT && table->status.revision != UINT64_MAX; ++i) {
        struct waiting_packet *waiting = &table->waiting[i];
        /** 只取本队列的有效记录，避免把同一 mbuf 同时交给不同处理队列 */
        if (waiting->packet.mbuf == NULL || waiting->packet.queue != queue)
            continue;
        /** 按入队时保存的邻居 ID 查找，不能仅凭槽位下标，否则复用槽位会串到另一个邻居 */
        struct dppd_nd6_entry *entry = NULL;
        for (uint32_t j = 0; j < DPPD_ND6_LIMIT; ++j)
            if (table->entries[j].id == waiting->neighbor_id)
                entry = &table->entries[j];
        /** 超时、邻居记录消失或解析已失败的包也要取出，标记 expired 后交给调用方释放 */
        bool expired = waiting->packet.expires_ms <= table->clock || entry == NULL || entry->state == DPPD_ND6_FAILED;
        /** 仍在等待 MAC 且未到重试时间才继续留队；MAC 已可用或记录失效时可提前取出 */
        if (!expired && entry->state == DPPD_ND6_INCOMPLETE && waiting->retry_ms > table->clock)
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
static int list_neighbors(struct dppd_nd6 *table, uint64_t after, uint64_t epoch, uint64_t revision,
    bool guarded, struct dppd_nd6_page *page)
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
int dppd_nd6_list(struct dppd_nd6 *table, uint64_t after, uint64_t revision, struct dppd_nd6_page *page)
{
    return list_neighbors(table, after, 0, revision, false, page);
}

int dppd_nd6_list_guarded(struct dppd_nd6 *table, uint64_t after, uint64_t epoch, uint64_t revision,
    struct dppd_nd6_page *page)
{
    return list_neighbors(table, after, epoch, revision, true, page);
}

static int flush_neighbors(struct dppd_nd6 *table, uint64_t epoch, uint64_t revision, bool guarded)
{
    if (table == NULL)
        return -ENODEV;
    struct rte_mbuf *retired[DPPD_ND6_PENDING_LIMIT] = {0};
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
            for (uint32_t i = 0; i < DPPD_ND6_PENDING_LIMIT; ++i)
                retired[i] = table->waiting[i].packet.mbuf;
            memset(table->waiting, 0, sizeof(table->waiting));
            table->status.queue_drops += table->status.pending;
            table->status.pending = 0;
        }
        advance_revision(table);
    }
    pthread_mutex_unlock(&table->lock);
    for (uint32_t i = 0; i < DPPD_ND6_PENDING_LIMIT; ++i)
        if (retired[i] != NULL)
            rte_pktmbuf_free(retired[i]);
    return rc;
}

int dppd_nd6_flush(struct dppd_nd6 *table, uint64_t revision)
{
    return flush_neighbors(table, 0, revision, false);
}

int dppd_nd6_flush_guarded(struct dppd_nd6 *table, uint64_t epoch, uint64_t revision)
{
    return flush_neighbors(table, epoch, revision, true);
}
