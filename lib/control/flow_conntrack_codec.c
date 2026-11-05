#include "flow_conntrack_io.h"

#include <errno.h>
#include <string.h>

struct policy_codec {
    uint8_t *output;
    const uint8_t *input;
    size_t capacity, offset;
    int error;
};

static uint64_t number(struct policy_codec *c, uint64_t value)
{
    if (c->offset > c->capacity || c->capacity - c->offset < 8) {
        c->error = -EBADMSG;
        return 0;
    }
    uint64_t result = 0;
    for (unsigned int i = 0; i < 8; ++i) {
        if (c->output != NULL)
            c->output[c->offset] = (uint8_t)(value >> (i * 8U));
        else
            result |= (uint64_t)c->input[c->offset] << (i * 8U);
        ++c->offset;
    }
    return c->output != NULL ? value : result;
}

/** IP 地址直接保存网络字节，避免同一地址在不同 CPU 上变成不同的请求内容 */
static void address(struct policy_codec *c, void *value, size_t length)
{
    if (c->offset > c->capacity || c->capacity - c->offset < length) {
        c->error = -EBADMSG;
        return;
    }
    if (c->output != NULL)
        memcpy(c->output + c->offset, value, length);
    else
        memcpy(value, c->input + c->offset, length);
    c->offset += length;
}

static void snat_wire(struct policy_codec *c, struct dppd_snat_config *s)
{
    s->enabled = number(c, s->enabled);
    if (!s->enabled)
        return;
    s->inside_port = number(c, s->inside_port);
    s->outside_port = number(c, s->outside_port);
    s->first_port = number(c, s->first_port);
    s->last_port = number(c, s->last_port);
    s->ipv6 = number(c, s->ipv6);
    if (s->ipv6)
        address(c, s->address6, 16);
    else
        address(c, &s->address_be, 4);
}

static int policy_wire(struct policy_codec *c, struct dppd_conntrack_policy *p)
{
    p->capacity = number(c, p->capacity);
    p->timeout_s = number(c, p->timeout_s);
    snat_wire(c, &p->snat);
    snat_wire(c, &p->snat6);
    uint64_t count = number(c, p->dnat_count);
    if (count > DPPD_MAX_DNAT_RULES)
        return -EBADMSG;
    p->dnat_count = count;
    for (uint32_t i = 0; i < p->dnat_count; ++i) {
        struct dppd_dnat_rule *r = &p->dnat[i];
        r->outside_port = number(c, r->outside_port);
        r->inside_port = number(c, r->inside_port);
        r->public_first = number(c, r->public_first);
        r->public_last = number(c, r->public_last);
        r->private_first = number(c, r->private_first);
        r->protocol = number(c, r->protocol);
        r->ipv6 = number(c, r->ipv6);
        if (r->ipv6) {
            address(c, r->public_address6, 16);
            address(c, r->private_address6, 16);
        } else {
            address(c, &r->public_address_be, 4);
            address(c, &r->private_address_be, 4);
        }
    }
    return c->error;
}

int dppd_flow_conntrack_encode(const struct dppd_conntrack_policy *policy, uint8_t *data,
    size_t capacity, size_t *length)
{
    if (policy == NULL || data == NULL || length == NULL)
        return -EINVAL;
    int rc = dppd_conntrack_policy_validate(policy);
    if (rc != 0)
        return rc;
    struct dppd_conntrack_policy copy = *policy;
    struct policy_codec c = {.output = data, .capacity = capacity};
    rc = policy_wire(&c, &copy);
    if (rc == 0)
        *length = c.offset;
    return rc;
}

int dppd_flow_conntrack_decode(const uint8_t *data, size_t length, struct dppd_conntrack_policy *policy)
{
    if (data == NULL || policy == NULL || length > DPPD_FLOW_CONNTRACK_BYTES)
        return -EINVAL;
    struct dppd_conntrack_policy candidate = {0};
    struct policy_codec c = {.input = data, .capacity = length};
    int rc = policy_wire(&c, &candidate);
    if (rc != 0 || c.offset != length)
        return -EBADMSG;
    uint8_t canonical[DPPD_FLOW_CONNTRACK_BYTES];
    size_t canonical_length;
    /** 再编码比较可发现整数截断、非法布尔值和尾部多余数据，损坏文件不能被悄悄修正 */
    rc = dppd_flow_conntrack_encode(&candidate, canonical, sizeof(canonical), &canonical_length);
    if (rc != 0 || length != canonical_length || memcmp(data, canonical, length) != 0)
        return -EBADMSG;
    *policy = candidate;
    return 0;
}

static bool bound(const struct dppd_flow_routing_state *routing, uint16_t port)
{
    for (uint16_t i = 0; i < routing->binding_count; ++i)
        if (routing->bindings[i].port_id == port)
            return true;
    return false;
}

int dppd_flow_conntrack_validate(const struct dppd_flow_conntrack_state *state,
    const struct dppd_flow_routing_state *routing)
{
    if (state == NULL)
        return 0;
    if (routing == NULL || state->generation == UINT64_MAX ||
        (state->generation == 0 && state->policy.capacity != 0))
        return -EINVAL;
    const struct dppd_conntrack_policy *p = &state->policy;
    int rc = dppd_conntrack_policy_validate(p);
    if (rc != 0)
        return rc;
    const struct dppd_snat_config *snats[2] = {&p->snat, &p->snat6};
    for (unsigned int i = 0; i < 2; ++i)
        if (snats[i]->enabled && (!bound(routing, snats[i]->inside_port) || !bound(routing, snats[i]->outside_port)))
            return -ENODEV;
    for (uint32_t i = 0; i < p->dnat_count; ++i)
        if (!bound(routing, p->dnat[i].inside_port) || !bound(routing, p->dnat[i].outside_port))
            return -ENODEV;
    return 0;
}
