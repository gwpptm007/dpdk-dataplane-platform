#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <rte_errno.h>
#include <rte_flow.h>
#include <rte_version.h>
#include "dppd/offload.h"

static const struct dppd_rule *expected;
static const uint16_t install_port = 7;
static unsigned int validate_calls, create_calls, destroy_calls, query_calls, flush_calls;
static int validate_result, create_error, destroy_result, query_result, flush_result;
static bool query_hits_set, query_bytes_set;
static uint64_t query_hits, query_bytes;
static unsigned int age_queries;
static int age_query_result;
static bool age_aged, age_seconds_valid;
static uint32_t age_seconds;
static struct rte_flow *const installed_flow = (struct rte_flow *)(uintptr_t)0x1234;

int __wrap_rte_flow_validate(uint16_t port, const struct rte_flow_attr *attr, const struct rte_flow_item *items,
    const struct rte_flow_action *actions, struct rte_flow_error *error);
struct rte_flow *__wrap_rte_flow_create(uint16_t port, const struct rte_flow_attr *attr,
    const struct rte_flow_item *items, const struct rte_flow_action *actions, struct rte_flow_error *error);
int __wrap_rte_flow_destroy(uint16_t port, struct rte_flow *flow, struct rte_flow_error *error);
int __wrap_rte_flow_query(uint16_t port, struct rte_flow *flow, const struct rte_flow_action *action, void *data,
    struct rte_flow_error *error);
int __wrap_rte_flow_flush(uint16_t port, struct rte_flow_error *error);

/** 在驱动入口直接读取编译结果，避免只测试另外一套没有参与正式安装的转换函数 */
static void check_compiled(uint16_t port, const struct rte_flow_attr *attr, const struct rte_flow_item *items,
    const struct rte_flow_action *actions)
{
    assert(expected != NULL && port == install_port);
    assert(attr->group == expected->group && attr->priority == expected->priority && attr->reserved == 0);
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
    /** 新版本公共入口就会拒绝混用方向，不能只靠模拟驱动接受来判断兼容 */
    assert(attr->ingress == (expected->domain == DPPD_RULE_DOMAIN_INGRESS));
    assert(attr->ingress + attr->egress + attr->transfer == 1);
#else
    assert(attr->ingress == (expected->domain != DPPD_RULE_DOMAIN_EGRESS));
#endif
    assert(attr->egress == (expected->domain == DPPD_RULE_DOMAIN_EGRESS));
    assert(attr->transfer == (expected->domain == DPPD_RULE_DOMAIN_TRANSFER));
    for (uint16_t i = 0; i < expected->nb_matches; ++i) {
        const struct dppd_match *match = &expected->matches[i];
        assert(items[i].last == NULL);
        switch (match->type) {
        case DPPD_MATCH_ETH:
            assert(items[i].type == RTE_FLOW_ITEM_TYPE_ETH && items[i].spec == NULL && items[i].mask == NULL);
            break;
        case DPPD_MATCH_IPV4: {
            const struct rte_flow_item_ipv4 *spec = items[i].spec, *mask = items[i].mask;
            assert(items[i].type == RTE_FLOW_ITEM_TYPE_IPV4 && spec != NULL && mask != NULL);
            assert(spec->hdr.src_addr == match->spec.ipv4.src_be && spec->hdr.dst_addr == match->spec.ipv4.dst_be);
            assert(mask->hdr.src_addr == match->spec.ipv4.src_mask_be &&
                mask->hdr.dst_addr == match->spec.ipv4.dst_mask_be);
            assert(mask->hdr.next_proto_id == 0 && mask->hdr.total_length == 0);
            break;
        }
        case DPPD_MATCH_UDP: {
            const struct rte_flow_item_udp *spec = items[i].spec, *mask = items[i].mask;
            assert(items[i].type == RTE_FLOW_ITEM_TYPE_UDP && spec != NULL && mask != NULL);
            assert(spec->hdr.src_port == match->spec.l4.src_be && spec->hdr.dst_port == match->spec.l4.dst_be);
            assert(
                mask->hdr.src_port == match->spec.l4.src_mask_be && mask->hdr.dst_port == match->spec.l4.dst_mask_be);
            break;
        }
        case DPPD_MATCH_TCP: {
            const struct rte_flow_item_tcp *spec = items[i].spec, *mask = items[i].mask;
            assert(items[i].type == RTE_FLOW_ITEM_TYPE_TCP && spec != NULL && mask != NULL);
            assert(spec->hdr.src_port == match->spec.l4.src_be && spec->hdr.dst_port == match->spec.l4.dst_be);
            assert(
                mask->hdr.src_port == match->spec.l4.src_mask_be && mask->hdr.dst_port == match->spec.l4.dst_mask_be);
            break;
        }
        case DPPD_MATCH_REPRESENTED_PORT: {
            const struct rte_flow_item_ethdev *spec = items[i].spec, *mask = items[i].mask;
            assert(items[i].type == RTE_FLOW_ITEM_TYPE_REPRESENTED_PORT && spec != NULL && mask != NULL);
            assert(spec->port_id == match->spec.ethdev_port_id && mask->port_id == UINT16_MAX);
            break;
        }
        default:
            assert(false);
        }
    }
    assert(items[expected->nb_matches].type == RTE_FLOW_ITEM_TYPE_END);
    for (uint16_t i = 0; i < expected->nb_actions; ++i) {
        const struct dppd_action *action = &expected->actions[i];
        switch (action->type) {
        case DPPD_ACTION_DROP:
            assert(actions[i].type == RTE_FLOW_ACTION_TYPE_DROP && actions[i].conf == NULL);
            break;
        case DPPD_ACTION_QUEUE:
            assert(actions[i].type == RTE_FLOW_ACTION_TYPE_QUEUE && actions[i].conf != NULL);
            assert(((const struct rte_flow_action_queue *)actions[i].conf)->index == action->conf.queue_id);
            break;
        case DPPD_ACTION_MARK:
            assert(actions[i].type == RTE_FLOW_ACTION_TYPE_MARK && actions[i].conf != NULL);
            assert(((const struct rte_flow_action_mark *)actions[i].conf)->id == action->conf.mark_id);
            break;
        case DPPD_ACTION_COUNT: {
            const struct rte_flow_action_count *count = actions[i].conf;
            assert(actions[i].type == RTE_FLOW_ACTION_TYPE_COUNT && count != NULL);
            assert(count->id == (uint32_t)expected->id);
            break;
        }
        case DPPD_ACTION_AGE: {
            const struct rte_flow_action_age *age = actions[i].conf;
            assert(actions[i].type == RTE_FLOW_ACTION_TYPE_AGE && age != NULL);
            assert(age->timeout == action->conf.age_timeout_seconds && age->reserved == 0 && age->context == NULL);
            break;
        }
        case DPPD_ACTION_REPRESENTED_PORT:
            assert(actions[i].type == RTE_FLOW_ACTION_TYPE_REPRESENTED_PORT && actions[i].conf != NULL);
            assert(((const struct rte_flow_action_ethdev *)actions[i].conf)->port_id == action->conf.ethdev_port_id);
            break;
        default:
            assert(false);
        }
    }
    assert(actions[expected->nb_actions].type == RTE_FLOW_ACTION_TYPE_END);
}

int __wrap_rte_flow_validate(uint16_t port, const struct rte_flow_attr *attr, const struct rte_flow_item *items,
    const struct rte_flow_action *actions, struct rte_flow_error *error)
{
    check_compiled(port, attr, items, actions);
    validate_calls++;
    error->message = "driver validate rejected";
    return validate_result;
}

struct rte_flow *__wrap_rte_flow_create(uint16_t port, const struct rte_flow_attr *attr,
    const struct rte_flow_item *items, const struct rte_flow_action *actions, struct rte_flow_error *error)
{
    check_compiled(port, attr, items, actions);
    create_calls++;
    error->message = "driver create exhausted";
    rte_errno = create_error;
    return create_error == 0 ? installed_flow : NULL;
}

int __wrap_rte_flow_destroy(uint16_t port, struct rte_flow *flow, struct rte_flow_error *error)
{
    assert(port == install_port && flow == installed_flow);
    destroy_calls++;
    error->message = "driver destroy busy";
    return destroy_result;
}

/** 驱动即使返回非零，也可能已经改写局部查询结构，适配层不能泄漏这些半成品 */
int __wrap_rte_flow_query(uint16_t port, struct rte_flow *flow, const struct rte_flow_action *action, void *data,
    struct rte_flow_error *error)
{
    assert(port == install_port && flow == installed_flow);
    if (action->type == RTE_FLOW_ACTION_TYPE_AGE) {
        struct rte_flow_query_age *query = data;
        assert(action->conf == NULL && query->reserved == 0 && !query->aged && !query->sec_since_last_hit_valid);
        assert(query->sec_since_last_hit == 0);
        age_queries++;
        query->aged = age_aged;
        query->sec_since_last_hit_valid = age_seconds_valid;
        query->sec_since_last_hit = age_seconds;
        error->message = "driver age query failed";
        return age_query_result;
    }
    struct rte_flow_query_count *query = data;
    assert(action->type == RTE_FLOW_ACTION_TYPE_COUNT && action->conf == NULL);
    assert(!query->reset && !query->hits_set && !query->bytes_set && query->reserved == 0);
    assert(query->hits == 0 && query->bytes == 0);
    query_calls++;
    query->hits_set = query_hits_set;
    query->bytes_set = query_bytes_set;
    query->hits = query_hits;
    query->bytes = query_bytes;
    error->message = "driver query failed";
    return query_result;
}

int __wrap_rte_flow_flush(uint16_t port, struct rte_flow_error *error)
{
    assert(port == install_port);
    flush_calls++;
    error->message = "driver flush failed";
    return flush_result;
}

static struct dppd_rule make_rule(void)
{
    return (struct dppd_rule){.id = UINT64_C(0x100000057),
        .generation = 19,
        .install_port_id = 7,
        .domain = DPPD_RULE_DOMAIN_TRANSFER,
        .fallback = DPPD_FALLBACK_REQUIRE_HARDWARE,
        .group = 3,
        .priority = 23,
        .nb_matches = 4,
        .nb_actions = 2,
        .matches = {{.type = DPPD_MATCH_REPRESENTED_PORT, .spec.ethdev_port_id = 31}, {.type = DPPD_MATCH_ETH},
            {.type = DPPD_MATCH_IPV4,
                .spec.ipv4 = {.src_be = htonl(0xc0000201),
                    .src_mask_be = htonl(0xffffffff),
                    .dst_be = htonl(0xc6336400),
                    .dst_mask_be = htonl(0xffffff00)}},
            {.type = DPPD_MATCH_UDP,
                .spec.l4 = {.src_be = htons(12345),
                    .src_mask_be = htons(65535),
                    .dst_be = htons(4789),
                    .dst_mask_be = htons(65535)}}},
        .actions = {{.type = DPPD_ACTION_COUNT}, {.type = DPPD_ACTION_REPRESENTED_PORT, .conf.ethdev_port_id = 42}}};
}

/** 同一份规则同时送入校验和创建，确认交换域方向、源与目的端口不会在两条路径中变样 */
static void compilation(void)
{
    struct dppd_rule rule = make_rule();
    struct dppd_flow_handle handle;
    struct dppd_flow_error error;
    expected = &rule;
    for (unsigned int variant = 0; variant < 3; ++variant) {
        if (variant != 0) {
            rule.domain = variant == 1 ? DPPD_RULE_DOMAIN_INGRESS : DPPD_RULE_DOMAIN_EGRESS;
            rule.nb_matches = 3;
            memmove(rule.matches, rule.matches + 1, rule.nb_matches * sizeof(rule.matches[0]));
            rule.matches[2].type = DPPD_MATCH_TCP;
            rule.actions[1].type = variant == 1 ? DPPD_ACTION_QUEUE : DPPD_ACTION_DROP;
            rule.actions[1].conf.queue_id = 5;
        }
        memset(&error, 0xff, sizeof(error));
        assert(dppd_flow_install(install_port, &rule, &handle, &error) == 0);
        assert(error.code == 0 && error.message[0] == '\0');
        assert(handle.rule_id == rule.id && handle.rule_generation == rule.generation);
        assert(handle.port_id == install_port && handle.flow == installed_flow && handle.has_count);
        assert(dppd_flow_remove(&handle, &error) == 0);
        assert(handle.flow == NULL && handle.rule_id == 0 && !handle.has_count);
        if (variant == 1)
            rule = make_rule();
    }
    rule = make_rule();
    rule.actions[0].type = DPPD_ACTION_MARK;
    rule.actions[0].conf.mark_id = 0xabcdef;
    assert(dppd_flow_create(install_port, &rule, &handle, &error) == 0 && !handle.has_count);
    assert(dppd_flow_remove(&handle, &error) == 0);
    assert(validate_calls == 3 && create_calls == 4 && destroy_calls == 4);
}

/** 校验拒绝不能继续创建，删除失败必须保留原对象，以便正式恢复路径再次处理 */
static void failures(void)
{
    struct dppd_rule rule = make_rule();
    struct dppd_flow_handle handle = {.rule_id = 99, .flow = installed_flow}, saved = handle;
    struct dppd_flow_error error;
    unsigned int created = create_calls, validated = validate_calls;
    expected = &rule;
    validate_result = -ENOTSUP;
    assert(dppd_flow_install(install_port, &rule, &handle, &error) == -ENOTSUP);
    assert(create_calls == created && memcmp(&handle, &saved, sizeof(handle)) == 0);
    assert(error.code == -ENOTSUP && strstr(error.message, "driver validate rejected") != NULL);
    validate_result = 0;
    create_error = ENOSPC;
    assert(dppd_flow_install(install_port, &rule, &handle, &error) == -ENOSPC);
    assert(handle.flow == NULL && handle.rule_id == 0 && !handle.has_count);
    assert(error.code == -ENOSPC && strstr(error.message, "driver create exhausted") != NULL);
    create_error = 0;
    assert(dppd_flow_create(install_port, &rule, &handle, &error) == 0);
    saved = handle;
    destroy_result = -EBUSY;
    assert(dppd_flow_remove(&handle, &error) == -EBUSY);
    assert(memcmp(&handle, &saved, sizeof(handle)) == 0);
    assert(error.code == -EBUSY && strstr(error.message, "driver destroy busy") != NULL);
    destroy_result = 0;
    assert(dppd_flow_remove(&handle, &error) == 0);
    assert(dppd_flow_remove(&handle, &error) == -EINVAL);
    rule.fallback = DPPD_FALLBACK_SOFTWARE_ONLY;
    assert(dppd_flow_install(install_port, &rule, &handle, &error) == -ENOTSUP);
    rule = make_rule();
    rule.nb_actions = 0;
    assert(dppd_flow_install(install_port, &rule, &handle, &error) == -EINVAL);
    rule = make_rule();
    rule.domain = DPPD_RULE_DOMAIN_INGRESS;
    rule.matches[0].type = DPPD_MATCH_ETH;
    rule.matches[1].type = DPPD_MATCH_IPV6;
    rule.nb_matches = 2;
    rule.actions[1].type = DPPD_ACTION_DROP;
    assert(dppd_flow_install(install_port, &rule, &handle, &error) == -ENOTSUP);
    assert(validate_calls == validated + 2 && create_calls == created + 2);
    flush_result = -EIO;
    assert(dppd_flow_flush(install_port, &error) == -EIO);
    assert(error.code == -EIO && strstr(error.message, "driver flush failed") != NULL);
    flush_result = 0;
    assert(dppd_flow_flush(install_port, &error) == 0 && error.code == 0 && flush_calls == 2);
}

/** 四种有效位组合都要覆盖，真实的零值有效，缺失值和驱动失败不能污染已有结果 */
static void counters(void)
{
    struct dppd_flow_handle handle = {.rule_id = 1, .port_id = 7, .flow = installed_flow, .has_count = true};
    struct dppd_flow_error error;
    uint64_t hits, bytes;
    query_hits = UINT64_C(0x100000003);
    query_bytes = UINT64_C(0x200000005);
    for (unsigned int fields = 0; fields < 4; ++fields) {
        query_hits_set = (fields & 1U) != 0;
        query_bytes_set = (fields & 2U) != 0;
        hits = 17;
        bytes = 29;
        int rc = dppd_flow_query_count(&handle, &hits, &bytes, &error);
        if (fields == 3) {
            assert(rc == 0 && hits == query_hits && bytes == query_bytes);
            assert(error.code == 0 && error.message[0] == '\0');
        } else {
            assert(rc == -ENODATA && hits == 17 && bytes == 29);
            assert(error.code == -ENODATA && strstr(error.message, "both packet and byte counters") != NULL);
        }
    }
    query_result = -EIO;
    hits = 17;
    bytes = 29;
    assert(dppd_flow_query_count(&handle, &hits, &bytes, &error) == -EIO);
    assert(hits == 17 && bytes == 29 && strstr(error.message, "driver query failed") != NULL);
    query_result = 0;
    query_hits = query_bytes = 0;
    assert(dppd_flow_query_count(&handle, &hits, &bytes, &error) == 0 && hits == 0 && bytes == 0);
    unsigned int queried = query_calls;
    handle.has_count = false;
    assert(dppd_flow_query_count(&handle, &hits, &bytes, &error) == -ENOENT);
    handle.has_count = true;
    assert(dppd_flow_query_count(&handle, NULL, &bytes, &error) == -EINVAL);
    assert(dppd_flow_query_count(&handle, &hits, NULL, &error) == -EINVAL);
    handle.flow = NULL;
    assert(dppd_flow_query_count(&handle, &hits, &bytes, &error) == -EINVAL);
    assert(dppd_flow_query_count(NULL, &hits, &bytes, &error) == -EINVAL);
    assert(query_calls == queried);
}

/** 捕获真正交给 PMD 的 AGE 配置和查询，老化状态不能由 COUNT 或无效秒数推算 */
static void aging(void)
{
    struct dppd_rule rule = make_rule();
    struct dppd_flow_handle handle;
    struct dppd_flow_error error;
    struct dppd_flow_age_result result;
    rule.actions[2] = rule.actions[1];
    rule.actions[1] = (struct dppd_action){.type = DPPD_ACTION_AGE, .conf.age_timeout_seconds = 1};
    rule.nb_actions = 3;
    expected = &rule;
    for (unsigned int boundary = 0; boundary < 2; ++boundary) {
        rule.actions[1].conf.age_timeout_seconds = boundary == 0 ? 1 : 0xffffffU;
        assert(dppd_flow_install(install_port, &rule, &handle, &error) == 0);
        assert(handle.has_age && handle.has_count);
        age_seconds = 0xffffffU;
        unsigned int count_queries = query_calls;
        for (unsigned int flags = 0; flags < 4; ++flags) {
            age_aged = (flags & 1U) != 0;
            age_seconds_valid = (flags & 2U) != 0;
            assert(dppd_flow_query_age(&handle, &result, &error) == 0);
            assert(result.aged == age_aged && result.sec_since_last_hit_valid == age_seconds_valid);
            assert(result.sec_since_last_hit == (age_seconds_valid ? age_seconds : 0));
            assert(error.code == 0 && error.message[0] == '\0');
        }
        assert(query_calls == count_queries);
        age_query_result = -ENOTSUP;
        result =
            (struct dppd_flow_age_result){.aged = true, .sec_since_last_hit_valid = true, .sec_since_last_hit = 17};
        assert(dppd_flow_query_age(&handle, &result, &error) == -ENOTSUP);
        assert(result.aged && result.sec_since_last_hit_valid && result.sec_since_last_hit == 17);
        assert(error.code == -ENOTSUP && strstr(error.message, "driver age query failed") != NULL);
        age_query_result = 0;
        handle.has_age = false;
        unsigned int queried = age_queries;
        assert(dppd_flow_query_age(&handle, &result, &error) == -ENODATA);
        assert(result.aged && result.sec_since_last_hit_valid && result.sec_since_last_hit == 17);
        handle.has_age = true;
        assert(dppd_flow_query_age(&handle, NULL, &error) == -EINVAL);
        assert(dppd_flow_query_age(NULL, &result, &error) == -EINVAL);
        assert(age_queries == queried);
        assert(dppd_flow_remove(&handle, &error) == 0 && !handle.has_age);
        assert(dppd_flow_query_age(&handle, &result, &error) == -EINVAL);
    }
    unsigned int validated = validate_calls, created = create_calls;
    rule.actions[1].conf.age_timeout_seconds = 0;
    assert(dppd_flow_install(install_port, &rule, &handle, &error) == -EINVAL);
    rule.actions[1].conf.age_timeout_seconds = 0x1000000U;
    assert(dppd_flow_install(install_port, &rule, &handle, &error) == -EINVAL);
    assert(validate_calls == validated && create_calls == created);
}

int main(void)
{
    compilation();
    failures();
    counters();
    aging();
    return 0;
}
