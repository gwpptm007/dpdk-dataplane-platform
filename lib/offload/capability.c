#include "dppd/capability.h"
#include "dppd/config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** 每个端口有独立的失效版本和累计统计，不能把另一个端口的校验结果套用过来 */
struct probe_port {
    bool occupied;
    uint16_t port_id;
    struct dppd_flow_probe_statistics statistics;
};

/** 保存完整规则语义，逐字段比较有效内容，避免填充字节或无效 union 字段影响缓存命中 */
struct probe_entry {
    bool occupied;
    uint64_t recorded_ns;
    struct dppd_rule rule;
    struct dppd_flow_probe_result result;
};

/** 全部端口共享六十四个结果槽位，满时循环替换，不再为新探测分配内存 */
struct dppd_flow_probe_cache {
    struct probe_port ports[DPPD_MAX_PORTS];
    struct probe_entry entries[DPPD_FLOW_PROBE_CACHE_CAPACITY];
    uint32_t next_victim;
};

/** 只读查找，不为一次统计查询创建端口或改变缓存 */
static const struct probe_port *find_port(const struct dppd_flow_probe_cache *cache,
                                          uint16_t port_id)
{
    for (uint32_t i = 0; i < DPPD_MAX_PORTS; ++i) {
        if (cache->ports[i].occupied && cache->ports[i].port_id == port_id)
            return &cache->ports[i];
    }
    return NULL;
}

/** 时钟不可用、数值异常或纳秒整数溢出时禁用本次缓存使用，不影响驱动校验 */
static bool monotonic_now(uint64_t *now_ns)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
        now.tv_nsec < 0 || now.tv_nsec >= 1000000000L ||
        (uint64_t)now.tv_sec >
        (UINT64_MAX - (uint64_t)now.tv_nsec) / UINT64_C(1000000000))
        return false;
    *now_ns = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
    return true;
}

/** ENOTSUP 和 ENOSYS 是不支持，EINVAL 与冲突是拒绝，其余错误不代表能力缺失 */
static enum dppd_flow_probe_status classify(int code)
{
    if (code == 0)
        return DPPD_FLOW_PROBE_SUPPORTED;
    if (code == -ENOTSUP || code == -ENOSYS)
        return DPPD_FLOW_PROBE_UNSUPPORTED;
    if (code == -EINVAL || code == -EEXIST)
        return DPPD_FLOW_PROBE_REJECTED;
    return DPPD_FLOW_PROBE_UNAVAILABLE;
}

/** 一次性建立固定容量缓存，初始化失败由控制层按已有顺序释放其他资源 */
int dppd_flow_probe_cache_init(struct dppd_flow_probe_cache **cache)
{
    if (cache == NULL)
        return -EINVAL;
    *cache = calloc(1, sizeof(**cache));
    return *cache == NULL ? -ENOMEM : 0;
}

/** 仅释放本地探测记录，不拥有网卡规则、规则账本或磁盘快照 */
void dppd_flow_probe_cache_destroy(struct dppd_flow_probe_cache *cache)
{
    free(cache);
}

/** 登记当前实例管理的端口，初始 epoch 为一，不继承另一个进程的观察 */
int dppd_flow_probe_cache_register(struct dppd_flow_probe_cache *cache, uint16_t port_id)
{
    if (cache == NULL)
        return -EINVAL;
    if (find_port(cache, port_id) != NULL)
        return 0;
    for (uint32_t i = 0; i < DPPD_MAX_PORTS; ++i) {
        struct probe_port *port = &cache->ports[i];

        if (!port->occupied) {
            port->occupied = true;
            port->port_id = port_id;
            port->statistics.epoch = 1;
            port->statistics.cache_capacity = DPPD_FLOW_PROBE_CACHE_CAPACITY;
            return 0;
        }
    }
    return -ENOSPC;
}

/** 清掉该端口的所有旧答复，统计仍按进程累计，版本回绕时也不会留下旧条目 */
int dppd_flow_probe_cache_clear(struct dppd_flow_probe_cache *cache, uint16_t port_id)
{
    struct probe_port *port;

    if (cache == NULL)
        return -EINVAL;
    port = (struct probe_port *)find_port(cache, port_id);
    if (port == NULL)
        return -ENOENT;
    for (uint32_t i = 0; i < DPPD_FLOW_PROBE_CACHE_CAPACITY; ++i) {
        if (cache->entries[i].occupied &&
            cache->entries[i].rule.install_port_id == port_id)
            cache->entries[i].occupied = false;
    }
    if (++port->statistics.epoch == 0)
        port->statistics.epoch = 1;
    port->statistics.invalidations++;
    return 0;
}

/** 同一交换域或设备可能共享资源，因此创建和删除不只使安装端口的结果失效 */
void dppd_flow_probe_cache_invalidate_all(struct dppd_flow_probe_cache *cache)
{
    if (cache == NULL)
        return;
    for (uint32_t i = 0; i < DPPD_MAX_PORTS; ++i) {
        if (cache->ports[i].occupied)
            (void)dppd_flow_probe_cache_clear(cache, cache->ports[i].port_id);
    }
}

/** 正式安装校验和显式探测都累计真实驱动答复，缓存命中不重复累计为驱动调用 */
void dppd_flow_probe_cache_observe(struct dppd_flow_probe_cache *cache, uint16_t port_id,
                                  const struct dppd_rule *rule, int validation_code)
{
    struct dppd_flow_probe_statistics *statistics;
    struct probe_port *port;

    if (cache == NULL || rule == NULL ||
        dppd_flow_probe_cache_register(cache, port_id) != 0)
        return;
    port = (struct probe_port *)find_port(cache, port_id);
    statistics = &port->statistics;
    statistics->validations++;
    if (validation_code == 0) {
        statistics->supported++;
        if (rule->domain >= DPPD_RULE_DOMAIN_INGRESS && rule->domain <= DPPD_RULE_DOMAIN_TRANSFER)
            statistics->observed_domains |= 1U << rule->domain;
        for (uint16_t i = 0; i < rule->nb_matches && i < DPPD_RULE_MAX_ITEMS; ++i) {
            if (rule->matches[i].type >= DPPD_MATCH_ETH &&
                rule->matches[i].type <= DPPD_MATCH_REPRESENTED_PORT)
                statistics->observed_items |= 1U << rule->matches[i].type;
        }
        for (uint16_t i = 0; i < rule->nb_actions && i < DPPD_RULE_MAX_ACTIONS; ++i) {
            if (rule->actions[i].type >= DPPD_ACTION_DROP &&
                rule->actions[i].type <= DPPD_ACTION_REPRESENTED_PORT)
                statistics->observed_actions |= 1U << rule->actions[i].type;
        }
    } else if (validation_code == -ENOTSUP || validation_code == -ENOSYS) {
        statistics->unsupported++;
    } else {
        statistics->failed++;
    }
}

/** 仅复制累计记录并数一下占用槽位，过期清理留给明确发起的探测操作 */
int dppd_flow_probe_cache_statistics(const struct dppd_flow_probe_cache *cache,
                                     uint16_t port_id,
                                     struct dppd_flow_probe_statistics *statistics)
{
    const struct probe_port *port;

    if (cache == NULL || statistics == NULL)
        return -EINVAL;
    memset(statistics, 0, sizeof(*statistics));
    port = find_port(cache, port_id);
    if (port == NULL)
        return -ENOENT;
    *statistics = port->statistics;
    for (uint32_t i = 0; i < DPPD_FLOW_PROBE_CACHE_CAPACITY; ++i) {
        if (cache->entries[i].occupied &&
            cache->entries[i].rule.install_port_id == port_id)
            statistics->cache_entries++;
    }
    return 0;
}

/**
 * 校验完整规则后查缓存，命中只返回诊断结果，未命中才询问驱动
 * 保留 ID 是因为 COUNT 编译会使用它，generation 和后端偏好不影响驱动规则语义
 * 比较使用已有的逐字段规则比较器，不把哈希碰撞或无效字节当作命中
 */
int dppd_flow_probe_cache_query(struct dppd_flow_probe_cache *cache, uint16_t port_id,
    const struct dppd_rule *rule, bool refresh,
    int (*validate)(uint16_t, const struct dppd_rule *, struct dppd_flow_error *),
    struct dppd_flow_probe_result *result)
{
    struct dppd_rule candidate;
    struct probe_port *port;
    struct probe_entry *slot = NULL;
    struct dppd_flow_error error = {0};
    char validation_error[128];
    uint64_t now_ns = 0;
    bool clock_available;
    int code, rc;

    if (cache == NULL || rule == NULL || validate == NULL || result == NULL)
        return -EINVAL;
    memset(result, 0, sizeof(*result));
    if (rule->id == 0 || dppd_rule_validate(rule, validation_error, sizeof(validation_error)) != 0)
        return -EINVAL;
    rc = dppd_flow_probe_cache_register(cache, port_id);
    if (rc != 0)
        return rc;
    port = (struct probe_port *)find_port(cache, port_id);
    candidate = *rule;
    candidate.install_port_id = port_id;
    candidate.generation = 0;
    candidate.fallback = DPPD_FALLBACK_REQUIRE_HARDWARE;
    clock_available = monotonic_now(&now_ns);
    for (uint32_t i = 0; i < DPPD_FLOW_PROBE_CACHE_CAPACITY; ++i) {
        struct probe_entry *entry = &cache->entries[i];

        if (entry->occupied && clock_available &&
            (now_ns < entry->recorded_ns ||
             now_ns - entry->recorded_ns >= DPPD_FLOW_PROBE_CACHE_TTL_NS))
            entry->occupied = false;
        if (!entry->occupied) {
            if (slot == NULL)
                slot = entry;
            continue;
        }
        if (dppd_rule_equal(&entry->rule, &candidate)) {
            slot = entry;
            if (!refresh && clock_available && entry->result.epoch == port->statistics.epoch) {
                *result = entry->result;
                result->cached = true;
                result->age_ns = now_ns - entry->recorded_ns;
                port->statistics.cache_hits++;
                return 0;
            }
            /** 刷新或时钟故障时先放弃同一键的旧答复，后续临时错误不能留下旧的成功结果 */
            entry->occupied = false;
        }
    }
    port->statistics.cache_misses++;
    code = validate(port_id, &candidate, &error);
    dppd_flow_probe_cache_observe(cache, port_id, &candidate, code);
    result->rule_id = candidate.id;
    result->install_port_id = port_id;
    result->status = classify(code);
    result->validation_code = code;
    result->epoch = port->statistics.epoch;
    memcpy(result->message, error.message, sizeof(result->message));
    result->message[sizeof(result->message) - 1U] = '\0';
    result->age_available = monotonic_now(&now_ns);
    /** 内存不足、设备忙、规则冲突等不能缓存成能力结论，下一次探测必须重新询问 */
    if (result->age_available && (code == 0 || code == -ENOTSUP || code == -ENOSYS)) {
        if (slot == NULL) {
            slot = &cache->entries[cache->next_victim];
            cache->next_victim = (cache->next_victim + 1U) % DPPD_FLOW_PROBE_CACHE_CAPACITY;
        }
        slot->rule = candidate;
        slot->recorded_ns = now_ns;
        slot->result = *result;
        slot->occupied = true;
    }
    return 0;
}
