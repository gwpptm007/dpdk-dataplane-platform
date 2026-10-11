#include "dppd/tap_content.h"
#include <errno.h>
#include <string.h>

/** 只有文件中的普通整数使用小端，网络字段用 memcpy 保持原始网络字节序 */
static void put(uint8_t *bytes, uint32_t value, unsigned int width)
{
    for (unsigned int i = 0; i < width; ++i)
        bytes[i] = (uint8_t)(value >> (8U * i));
}

/** 逐字节读取避免未对齐访问，也不依赖运行机器的字节序 */
static uint32_t get(const uint8_t *bytes, unsigned int width)
{
    uint32_t value = 0;
    for (unsigned int i = 0; i < width; ++i)
        value |= (uint32_t)bytes[i] << (8U * i);
    return value;
}

/** 匹配条件占前 36 字节，后面的挂载位置单独比较，便于区分内容变化和范围变化 */
void dppd_tap_content_encode(const struct dppd_tap_content *c, uint8_t bytes[DPPD_TAP_CONTENT_SIZE])
{
    memset(bytes, 0, DPPD_TAP_CONTENT_SIZE);
    put(bytes, c->version, 4);
    put(bytes + 4, c->eth_type, 2);
    put(bytes + 6, c->queue, 2);
    bytes[8] = c->ip_proto;
    bytes[9] = c->action;
    memcpy(bytes + 12, &c->ipv4.src_be, 4);
    memcpy(bytes + 16, &c->ipv4.src_mask_be, 4);
    memcpy(bytes + 20, &c->ipv4.dst_be, 4);
    memcpy(bytes + 24, &c->ipv4.dst_mask_be, 4);
    memcpy(bytes + 28, &c->l4.src_be, 2);
    memcpy(bytes + 30, &c->l4.src_mask_be, 2);
    memcpy(bytes + 32, &c->l4.dst_be, 2);
    memcpy(bytes + 34, &c->l4.dst_mask_be, 2);
    put(bytes + 36, c->parent, 4);
    put(bytes + 40, c->chain, 4);
    put(bytes + 44, c->priority, 2);
    put(bytes + 46, c->protocol, 2);
}

/** 被掩码忽略的位必须清零，防止同一规则出现多种磁盘表示 */
bool dppd_tap_content_valid(const struct dppd_tap_content *c)
{
    uint8_t bytes[DPPD_TAP_CONTENT_SIZE], zero[DPPD_TAP_CONTENT_SIZE] = {0};
    if (c == NULL)
        return false;
    dppd_tap_content_encode(c, bytes);
    if (c->version == 0)
        return memcmp(bytes, zero, sizeof(bytes)) == 0;
    if (c->version != 1 || (c->eth_type != 0 && c->eth_type != 0x0800) ||
        (c->ip_proto != 0 && c->ip_proto != 6 && c->ip_proto != 17) || (c->action != 1 && c->action != 2) ||
        (c->action == 1 && c->queue != 0) || c->parent != 0x10000 || c->chain != 0 || c->priority == 0 ||
        c->protocol != 3 || (c->ipv4.src_be & c->ipv4.src_mask_be) != c->ipv4.src_be ||
        (c->ipv4.dst_be & c->ipv4.dst_mask_be) != c->ipv4.dst_be ||
        (c->l4.src_be & c->l4.src_mask_be) != c->l4.src_be || (c->l4.dst_be & c->l4.dst_mask_be) != c->l4.dst_be ||
        (c->l4.src_mask_be != 0 && c->l4.src_mask_be != UINT16_MAX) ||
        (c->l4.dst_mask_be != 0 && c->l4.dst_mask_be != UINT16_MAX))
        return false;
    if (c->eth_type == 0 && (c->ip_proto != 0 || c->ipv4.src_mask_be != 0 || c->ipv4.dst_mask_be != 0))
        return false;
    return c->ip_proto != 0 || (c->l4.src_mask_be == 0 && c->l4.dst_mask_be == 0);
}

/** 解码后重新编码对比，连同未知保留位一起检查，不把损坏数据当作未保存内容 */
int dppd_tap_content_decode(const uint8_t bytes[DPPD_TAP_CONTENT_SIZE], struct dppd_tap_content *c)
{
    uint8_t canonical[DPPD_TAP_CONTENT_SIZE];
    memset(c, 0, sizeof(*c));
    c->version = get(bytes, 4);
    c->eth_type = (uint16_t)get(bytes + 4, 2);
    c->queue = (uint16_t)get(bytes + 6, 2);
    c->ip_proto = bytes[8];
    c->action = bytes[9];
    memcpy(&c->ipv4.src_be, bytes + 12, 4);
    memcpy(&c->ipv4.src_mask_be, bytes + 16, 4);
    memcpy(&c->ipv4.dst_be, bytes + 20, 4);
    memcpy(&c->ipv4.dst_mask_be, bytes + 24, 4);
    memcpy(&c->l4.src_be, bytes + 28, 2);
    memcpy(&c->l4.src_mask_be, bytes + 30, 2);
    memcpy(&c->l4.dst_be, bytes + 32, 2);
    memcpy(&c->l4.dst_mask_be, bytes + 34, 2);
    c->parent = get(bytes + 36, 4);
    c->chain = get(bytes + 40, 4);
    c->priority = (uint16_t)get(bytes + 44, 2);
    c->protocol = (uint16_t)get(bytes + 46, 2);
    dppd_tap_content_encode(c, canonical);
    return dppd_tap_content_valid(c) && memcmp(bytes, canonical, sizeof(canonical)) == 0 ? 0 : -EBADMSG;
}

/** 在驱动调用前生成预期值，不依赖之后读回的对象，崩溃窗口也能核对原始意图 */
int dppd_tap_content_from_rule(const struct dppd_rule *rule, uint16_t max_queues, struct dppd_tap_content *c)
{
    if (rule == NULL || c == NULL || dppd_rule_validate(rule, NULL, 0) != 0)
        return -EINVAL;
    memset(c, 0, sizeof(*c));
    if (rule->domain != DPPD_RULE_DOMAIN_INGRESS || rule->nb_actions != 1 || rule->group > 15 || max_queues == 0 ||
        max_queues > 4094 || rule->priority > 4094U - max_queues)
        return -ENOTSUP;
    c->version = 1;
    c->parent = 0x10000;
    c->protocol = 3;
    /** 高 4 位保留组号，低 12 位放优先级，先让出驱动内部规则使用的编号 */
    c->priority = (uint16_t)((rule->group << 12) | (rule->priority + max_queues + 1U));
    for (uint16_t i = 0; i < rule->nb_matches; ++i) {
        const struct dppd_match *match = &rule->matches[i];
        switch (match->type) {
        case DPPD_MATCH_ETH:
            break;
        case DPPD_MATCH_IPV4:
            c->eth_type = 0x0800;
            c->ipv4 = match->spec.ipv4;
            c->ipv4.src_be &= c->ipv4.src_mask_be;
            c->ipv4.dst_be &= c->ipv4.dst_mask_be;
            break;
        case DPPD_MATCH_TCP:
        case DPPD_MATCH_UDP:
            c->ip_proto = match->type == DPPD_MATCH_TCP ? 6 : 17;
            c->l4 = match->spec.l4;
            c->l4.src_be &= c->l4.src_mask_be;
            c->l4.dst_be &= c->l4.dst_mask_be;
            break;
        default:
            return -ENOTSUP;
        }
    }
    if (rule->actions[0].type == DPPD_ACTION_DROP)
        c->action = 1;
    else if (rule->actions[0].type == DPPD_ACTION_QUEUE) {
        c->action = 2;
        c->queue = rule->actions[0].conf.queue_id;
    } else
        return -ENOTSUP;
    return dppd_tap_content_valid(c) ? 0 : -ENOTSUP;
}
