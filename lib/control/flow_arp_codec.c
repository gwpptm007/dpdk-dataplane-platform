#include "flow_arp_io.h"

#include <errno.h>
#include <string.h>

struct arp_codec {
    const uint8_t *input;
    uint8_t *output;
    size_t capacity, offset;
    int error;
};

static uint64_t number(struct arp_codec *c, uint64_t value)
{
    if (c->offset > c->capacity || c->capacity - c->offset < 8) {
        c->error = -EBADMSG;
        return 0;
    }
    uint64_t result = 0;
    for (unsigned int i = 0; i < 8; ++i) {
        if (c->output != NULL)
            c->output[c->offset] = (uint8_t)(value >> (8U * i));
        else
            result |= (uint64_t)c->input[c->offset] << (8U * i);
        ++c->offset;
    }
    return c->output != NULL ? value : result;
}

static int policy_wire(struct arp_codec *c, struct dppd_arp_policy *p)
{
    p->reachable_ms = number(c, p->reachable_ms);
    uint64_t count = number(c, p->local_count);
    if (count > DPPD_ARP_LOCAL_LIMIT)
        return -EBADMSG;
    p->local_count = count;
    for (uint32_t i = 0; i < p->local_count; ++i) {
        struct dppd_arp_local *local = &p->locals[i];
        local->port_id = number(c, local->port_id);
        uint64_t depth = number(c, local->vlan_depth);
        if (depth > 2 || c->error != 0 || c->offset > c->capacity || c->capacity - c->offset < 4)
            return -EBADMSG;
        local->vlan_depth = depth;
        /** IPv4 地址原样保存网络字节，避免把四个字节当主机整数翻转 */
        if (c->output != NULL)
            memcpy(c->output + c->offset, local->address, 4);
        else
            memcpy(local->address, c->input + c->offset, 4);
        c->offset += 4;
        for (uint8_t v = 0; v < local->vlan_depth; ++v)
            local->vlan[v] = number(c, local->vlan[v]);
    }
    return c->error;
}

int dppd_flow_arp_encode(const struct dppd_arp_policy *policy, uint8_t *data, size_t capacity, size_t *length)
{
    if (policy == NULL || data == NULL || length == NULL)
        return -EINVAL;
    int rc = dppd_arp_policy_validate(policy);
    if (rc != 0)
        return rc;
    struct dppd_arp_policy copy = *policy;
    struct arp_codec c = {.output = data, .capacity = capacity};
    rc = policy_wire(&c, &copy);
    if (rc == 0)
        *length = c.offset;
    return rc;
}

int dppd_flow_arp_decode(const uint8_t *data, size_t length, struct dppd_arp_policy *policy)
{
    if (data == NULL || policy == NULL || length > DPPD_FLOW_ARP_BYTES)
        return -EINVAL;
    struct dppd_arp_policy candidate = {0};
    struct arp_codec c = {.input = data, .capacity = length};
    if (policy_wire(&c, &candidate) != 0 || c.offset != length)
        return -EBADMSG;
    uint8_t canonical[DPPD_FLOW_ARP_BYTES];
    size_t size;
    /** 重新编码比较同时拒绝整数截断、尾随数据和非规范值，不把损坏文件自动修成合法配置 */
    if (dppd_flow_arp_encode(&candidate, canonical, sizeof(canonical), &size) != 0 || size != length ||
        memcmp(data, canonical, length) != 0)
        return -EBADMSG;
    *policy = candidate;
    return 0;
}

int dppd_flow_arp_validate(const struct dppd_flow_arp_state *arp, const struct dppd_flow_routing_state *routing)
{
    if (arp == NULL)
        return 0;
    if (routing == NULL || arp->generation == UINT64_MAX ||
        (arp->generation == 0 && arp->policy.local_count != 0))
        return -EINVAL;
    int rc = dppd_arp_policy_validate(&arp->policy);
    if (rc != 0)
        return rc;
    for (uint32_t i = 0; i < arp->policy.local_count; ++i) {
        bool found = false;
        for (uint16_t j = 0; j < routing->binding_count; ++j)
            found |= arp->policy.locals[i].port_id == routing->bindings[j].port_id;
        if (!found)
            return -ENODEV;
    }
    return 0;
}
