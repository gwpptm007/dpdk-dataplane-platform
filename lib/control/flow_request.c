#include "flow_request_io.h"
#include "persistence_codec.h"

#include <errno.h>
#include <string.h>

_Static_assert(24U + 8U * (64U + DPPD_PERSISTENCE_RULE_BYTES) <= DPPD_REQUEST_BYTES,
    "canonical request buffer must fit every supported operation");

static void number(uint8_t *data, uint32_t *offset, uint64_t value)
{
    for (unsigned int byte = 0; byte < 8; ++byte)
        data[(*offset)++] = (uint8_t)(value >> (8U * byte));
}

/** 复用规范规则编码，临时零版本独立记录，未使用的联合体和填充字节不参与请求比较 */
static int rule_bytes(uint8_t *data, uint32_t *offset, const struct dppd_rule *input)
{
    struct dppd_rule rule = *input;
    number(data, offset, rule.id);
    number(data, offset, rule.generation);
    uint64_t unresolved = 0;
    if (rule.nb_actions > DPPD_RULE_MAX_ACTIONS)
        return -EINVAL;
    for (uint16_t i = 0; i < rule.nb_actions; ++i)
        if (rule.actions[i].type == DPPD_ACTION_SHARED_COUNT && rule.actions[i].conf.shared_count.generation == 0) {
            unresolved |= UINT64_C(1) << i;
            rule.actions[i].conf.shared_count.generation = 1;
        }
    number(data, offset, unresolved);
    rule.id = rule.generation = 1;
    memset(data + *offset, 0, DPPD_PERSISTENCE_RULE_BYTES);
    int rc = dppd_persistence_rule_encode(&rule, data + *offset);
    *offset += DPPD_PERSISTENCE_RULE_BYTES;
    return rc;
}

int dppd_flow_request_encode(const struct dppd_flow_request *r, uint8_t *data, uint32_t *length)
{
    if (r == NULL || data == NULL || length == NULL || r->key.client == 0 || r->key.sequence == 0 ||
        r->operation < DPPD_FLOW_REQUEST_CREATE || r->operation > DPPD_FLOW_REQUEST_CONFIGURE || r->count == 0 ||
        r->count > (r->operation == DPPD_FLOW_REQUEST_CONFIGURE ? 8U : 4U))
        return -EINVAL;
    uint32_t offset = 0;
    number(data, &offset, r->operation);
    number(data, &offset, r->count);
    number(data, &offset, r->operation == DPPD_FLOW_REQUEST_CONFIGURE ? r->expected_revision : r->queue);
    for (uint32_t i = 0; i < r->count; ++i) {
        int rc = 0;
        if (r->operation == DPPD_FLOW_REQUEST_CONFIGURE) {
            const struct dppd_flow_config_change *change = &r->payload.configuration[i];
            if (change->operation < DPPD_FLOW_CONFIG_TEMPLATE_ADD || change->operation > DPPD_FLOW_CONFIG_COUNT_DELETE)
                return -EINVAL;
            number(data, &offset, change->operation);
            number(data, &offset, change->key);
            number(data, &offset, change->expected_generation);
            if (change->operation == DPPD_FLOW_CONFIG_TEMPLATE_ADD) {
                number(data, &offset, change->spec.table.capacity);
                number(data, &offset, change->spec.table.dynamic_actions);
                rc = rule_bytes(data, &offset, &change->spec.table.example);
            } else if (change->operation == DPPD_FLOW_CONFIG_COUNT_ADD) {
                number(data, &offset, change->spec.count.port_id);
                number(data, &offset, change->spec.count.domain);
            }
        } else if (r->operation == DPPD_FLOW_REQUEST_REMOVE) {
            number(data, &offset, r->payload.remove[i].id);
            number(data, &offset, r->payload.remove[i].generation);
        } else {
            const struct dppd_flow_state_rule *entry = &r->payload.change.rules[i];
            number(data, &offset, entry->table_key);
            number(data, &offset, entry->queue);
            number(data, &offset, r->payload.change.expected[i]);
            rc = rule_bytes(data, &offset, &entry->rule);
        }
        if (rc != 0)
            return rc;
    }
    *length = offset;
    return offset <= DPPD_REQUEST_BYTES ? 0 : -E2BIG;
}

int dppd_flow_request_ledger_validate(const struct dppd_flow_request_ledger *ledger, uint64_t revision)
{
    if (ledger == NULL)
        return 0;
    if (ledger->cursor >= DPPD_REQUEST_RECEIPTS)
        return -EINVAL;
    for (uint32_t c = 0; c < DPPD_REQUEST_CLIENTS; ++c) {
        if ((ledger->clients[c].client == 0) != (ledger->clients[c].sequence == 0))
            return -EINVAL;
        for (uint32_t j = 0; j < c; ++j)
            if (ledger->clients[c].client != 0 && ledger->clients[c].client == ledger->clients[j].client)
                return -EINVAL;
    }
    for (uint32_t i = 0; i < DPPD_REQUEST_RECEIPTS; ++i) {
        const struct dppd_flow_request_record *record = &ledger->receipts[i];
        const struct dppd_flow_request_result *r = &record->result;
        if (r->key.client == 0)
            continue;
        if (r->key.sequence == 0 || r->operation < DPPD_FLOW_REQUEST_CREATE ||
            r->operation > DPPD_FLOW_REQUEST_CONFIGURE || r->phase < DPPD_FLOW_REQUEST_PENDING ||
            r->phase > DPPD_FLOW_REQUEST_DONE || r->count == 0 ||
            r->count > (r->operation == DPPD_FLOW_REQUEST_CONFIGURE ? 8U : 4U) || r->revision > revision ||
            r->status > 0 || r->status < -4095 || record->length == 0 || record->length > DPPD_REQUEST_BYTES ||
            (r->phase == DPPD_FLOW_REQUEST_PENDING && (r->applied || r->status != 0)))
            return -EINVAL;
        bool client = false;
        for (uint32_t c = 0; c < DPPD_REQUEST_CLIENTS; ++c)
            client |= ledger->clients[c].client == r->key.client && ledger->clients[c].sequence >= r->key.sequence;
        if (!client)
            return -EINVAL;
        for (uint32_t j = 0; j < i; ++j)
            if (ledger->receipts[j].result.key.client == r->key.client &&
                ledger->receipts[j].result.key.sequence == r->key.sequence)
                return -EINVAL;
    }
    return 0;
}
