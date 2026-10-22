#include "dppd/exact.h"

#include <errno.h>
#include <string.h>

/** 键逐字段写入固定字节数组，避免结构体填充或主机字节序影响哈希结果 */
#define EXACT_KEY_SIZE 40U

struct exact_row {
    uint8_t key[EXACT_KEY_SIZE];
    uint64_t id;
    uint32_t group, priority, tag, next;
};

struct dppd_exact {
    uint32_t capacity, count;
    struct exact_row rows[];
};

/** 桶数组放在行数组之后，每个桶保存行号加一，零表示没有候选 */
static uint32_t *buckets(const struct dppd_exact *table)
{
    return (uint32_t *)(table->rows + table->capacity);
}

size_t dppd_exact_size(uint32_t capacity)
{
    size_t bytes = sizeof(struct dppd_exact) + (size_t)capacity * (sizeof(struct exact_row) + sizeof(uint32_t));
    return (bytes + 7U) & ~(size_t)7U;
}

struct dppd_exact *dppd_exact_init(void *memory, uint32_t capacity)
{
    struct dppd_exact *table = memory;
    memset(memory, 0, dppd_exact_size(capacity));
    table->capacity = capacity;
    return table;
}

/**
 * 用 FNV-1a 算法将 40 字节 key 汇总为 32 位哈希值，调用方再取模选择桶
 * 2166136261 是初始值，16777619 是逐字节异或后使用的乘数，均为算法常量
 * 不同 key 可能得到相同桶下标，查询时仍须比较完整 key
 */
static uint32_t hash_key(const uint8_t *key)
{
    uint32_t hash = 2166136261U;
    for (unsigned int i = 0; i < EXACT_KEY_SIZE; ++i)
        hash = (hash ^ key[i]) * 16777619U;
    return hash;
}

static bool before(const struct exact_row *a, const struct exact_row *b)
{
    if (a->group != b->group)
        return a->group < b->group;
    if (a->priority != b->priority)
        return a->priority < b->priority;
    if (a->id != b->id)
        return a->id < b->id;
    return a->tag < b->tag;
}

/** 入口保留两个字节，地址族和传输层各占一个字节，地址区统一预留 IPv6 宽度 */
static void key_prefix(uint8_t *key, uint16_t ingress, uint8_t family, uint8_t transport)
{
    key[0] = (uint8_t)(ingress >> 8);
    key[1] = (uint8_t)ingress;
    key[2] = family;
    key[3] = transport;
}

/**
 * 将完整源/目的 IP 和 TCP/UDP 端口组成精确匹配键，把规则索引加入 table 的哈希桶
 * rule 提供匹配条件和优先顺序，tag 是调用方规则槽位下标加 1，供命中后查回动作和计数器
 * 返回 0 表示入表成功；-EINVAL 参数或规则无效，-ENOTSUP 匹配条件不支持，-ENOSPC 表已满
 *
 * 完整地址和端口、无额外不支持的条件 → exact 建键入表
 * 地址掩码、端口通配或范围等条件     → -ENOTSUP → publish_locked 尝试加入 ACL
 * 本函数在规则安装时调用，只建立匹配索引，不执行 DROP、MARK 或 COUNT 动作
 */
int dppd_exact_add(struct dppd_exact *table, const struct dppd_rule *rule, uint32_t tag)
{
    char error[128];
    if (table == NULL || rule == NULL || tag == 0 || rule->domain != DPPD_RULE_DOMAIN_INGRESS ||
        dppd_rule_validate(rule, error, sizeof(error)) != 0)
        return -EINVAL;
    /**
     * 将规则的匹配字段拼成 40 字节 key，下面的偏移相对 key 起点，不是报文偏移
     * 偏移  0～1   2       3       4～19    20～35    36～37   38～39
     * 内容  入口   地址族  传输层  源地址   目的地址  源端口   目的端口
     * 地址区各留 16 字节以兼容 IPv6；IPv4 只写前 4 字节，后 12 字节保持为 0
     * 本行初始化未列出的成员也会清零，因此 key 的未使用字节和 next 初始均为 0
     *
     * 例：入口 2，IPv4 / UDP，192.0.2.1:1234 → 198.51.100.2:9000
     * key[0～3]   = 00 02、DPPD_L3_IPV4、DPPD_L4_UDP
     * key[4～19]  = c0 00 02 01 + 12 个零字节
     * key[20～35] = c6 33 64 02 + 12 个零字节
     * key[36～39] = 04 d2 23 28，即网络字节序的 1234 和 9000
     * 地址族、传输层使用项目枚举；地址和端口从已有网络字节序字段直接复制
     *
     * 安装规则 → 从 rule 拼 key ─┐
     * 收到报文 → 从 packet 拼 key ┴→ 相同字段得到相同字节，供哈希定位和完整比较
     * id、group、priority、tag 不放入 key，命中后再用于选规则及查回动作
     */
    struct exact_row row = {.id = rule->id, .group = rule->group, .priority = rule->priority, .tag = tag};
    uint8_t family = 0, transport = 0;
    for (uint16_t i = 0; i < rule->nb_matches; ++i) {
        const struct dppd_match *match = &rule->matches[i];
        switch (match->type) {
        case DPPD_MATCH_ETH:
            break;
        case DPPD_MATCH_IPV4:
            /**
             * 掩码中的 1 表示该位必须匹配，exact 要求源、目的地址的全部 32 位都参与比较
             * 255.255.255.255 = 0xffffffff → 精确到一个 IPv4 地址，允许继续建键
             * 255.255.255.0   = 0xffffff00 → 最后 8 位不限，表示一个网段，交给 ACL
             */
            if (match->spec.ipv4.src_mask_be != UINT32_MAX || match->spec.ipv4.dst_mask_be != UINT32_MAX)
                return -ENOTSUP;
            family = DPPD_L3_IPV4;
            memcpy(row.key + 4, &match->spec.ipv4.src_be, 4);
            memcpy(row.key + 20, &match->spec.ipv4.dst_be, 4);
            break;
        case DPPD_MATCH_IPV6:
            /** IPv6 同样要求完整 128 位匹配，因此源、目的掩码各 16 个字节都必须为 0xff */
            for (unsigned int byte = 0; byte < 16; ++byte)
                if (match->spec.ipv6.src_mask[byte] != 255 || match->spec.ipv6.dst_mask[byte] != 255)
                    return -ENOTSUP;
            family = DPPD_L3_IPV6;
            memcpy(row.key + 4, match->spec.ipv6.src, 16);
            memcpy(row.key + 20, match->spec.ipv6.dst, 16);
            break;
        case DPPD_MATCH_TCP:
        case DPPD_MATCH_UDP:
            /**
             * 源、目的端口掩码都须为 0xffff，表示完整比较各自的 16 位端口值
             * 例：UDP 源端口任意、目的端口 9000 → 源掩码为 0 → exact 不支持，改用 ACL
             */
            if (match->spec.l4.src_mask_be != UINT16_MAX || match->spec.l4.dst_mask_be != UINT16_MAX)
                return -ENOTSUP;
            transport = match->type == DPPD_MATCH_TCP ? DPPD_L4_TCP : DPPD_L4_UDP;
            memcpy(row.key + 36, &match->spec.l4.src_be, 2);
            memcpy(row.key + 38, &match->spec.l4.dst_be, 2);
            break;
        default:
            return -ENOTSUP;
        }
    }
    /** 缺少 IP 地址族或 TCP/UDP 条件就不能组成完整键，例如仅指定 IPv4 地址的规则交给 ACL */
    if (family == 0 || transport == 0)
        return -ENOTSUP;
    if (table->count == table->capacity)
        return -ENOSPC;
    /** 前面的循环已填入地址和端口，这里补齐 key[0～3] 的入口、地址族和传输层类型 */
    key_prefix(row.key, rule->install_port_id, family, transport);
    /**
     * 哈希值 % capacity 选出桶下标，新规则存入 rows[count]，并接在该桶链表头部
     * 桶值和 next 保存行下标加 1，保留 0 表示空链；它们与查回规则动作的 tag 无关
     *
     * 例：capacity = 8，count = 3，本次取模得到桶 2，原桶值为 1
     * 插入前：桶 2 → 编号 1 → rows[0] → 0，结束
     * row.next = 1          → 新行接住原链头
     * rows[3] = row         → 将局部 row 的内容复制到表内空位
     * 桶值 = ++count = 4    → 总行数变为 4，桶头改指向编号 4，即 rows[3]
     * 插入后：桶 2 → 编号 4 → rows[3] → 编号 1 → rows[0] → 0，结束
     * 若原桶为空，row.next 就是 0；头插只决定遍历顺序，规则优先级由查询时比较
     */
    uint32_t bucket = hash_key(row.key) % table->capacity;
    row.next = buckets(table)[bucket];
    table->rows[table->count] = row;
    buckets(table)[bucket] = ++table->count;
    return 0;
}

/**
 * 用入口、地址族、TCP/UDP 类型及完整源/目的地址和端口组成精确匹配键
 * 哈希定位桶后比较完整键，同键多条规则按优先顺序选出一条，返回其 tag
 * 未命中、报文类型不适用或参数无效返回 0，仅查找候选，不执行规则动作
 */
uint32_t dppd_exact_classify(const struct dppd_exact *table, uint16_t ingress, const struct dppd_packet *packet)
{
    if (table == NULL || table->capacity == 0 || packet == NULL ||
        (packet->l3_type != DPPD_L3_IPV4 && packet->l3_type != DPPD_L3_IPV6) ||
        (packet->l4_type != DPPD_L4_TCP && packet->l4_type != DPPD_L4_UDP))
        return 0;
    /**
     * 将报文字段组合成 40 字节查找键，下列偏移均相对 key 起点
     * 偏移  0～1   2       3       4～19    20～35   36～37   38～39
     * 内容  入口   地址族  传输层  源地址   目的地址 源端口   目的端口
     * 地址族和传输层使用项目枚举，地址和端口保留网络字节序
     * IPv4 地址各写 4 字节，其余地址空间保留为 0；IPv6 地址各写满 16 字节
     * 规则入表与报文查询使用同一布局，清零保证未使用字节也能一致比较
     */
    uint8_t key[EXACT_KEY_SIZE] = {0};
    key_prefix(key, ingress, (uint8_t)packet->l3_type, (uint8_t)packet->l4_type);
    if (packet->l3_type == DPPD_L3_IPV4) {
        memcpy(key + 4, &packet->ipv4_src_be, 4);
        memcpy(key + 20, &packet->ipv4_dst_be, 4);
    } else {
        memcpy(key + 4, packet->ipv6_src, 16);
        memcpy(key + 20, packet->ipv6_dst, 16);
    }
    memcpy(key + 36, &packet->l4_src_port_be, 2);
    memcpy(key + 38, &packet->l4_dst_port_be, 2);
    /**
     * 桶值和 next 均为行下标加一，0 表示链结束，与规则 tag 含义不同
     * 不同键可能落入同一桶，须比较完整 40 字节，不能仅凭哈希认定命中
     *
     * hash_key(key)                  → 算出哈希值
     * 哈希值 % table->capacity       → 得到桶下标
     * buckets(table)[桶下标]         → 取得桶内第一条规则的行编号
     * 沿桶值 4 → rows[3] → next 为 1 → rows[0] → next 为 0，结束
     */
    const struct exact_row *selected = NULL;
    for (uint32_t index = buckets(table)[hash_key(key) % table->capacity]; index != 0;) {
        const struct exact_row *row = &table->rows[index - 1];
        /** 同键规则可能有多条，保留优先的一条，不能遇到首次命中就返回 */
        if (memcmp(key, row->key, sizeof(key)) == 0 && (selected == NULL || before(row, selected)))
            selected = row;
        index = row->next;
    }
    /** 返回本精确表选出的候选 tag，无命中返回 0，交由软件后端与 ACL 候选比较 */
    return selected == NULL ? 0 : selected->tag;
}
