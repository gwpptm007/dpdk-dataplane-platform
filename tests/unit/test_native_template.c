#define ALLOW_EXPERIMENTAL_API
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <rte_errno.h>
#include <rte_flow.h>
#include <rte_version.h>
#include "dppd/flow_template.h"

#if RTE_VERSION >= RTE_VERSION_NUM(24, 11, 0, 0)
static unsigned int phase, fail_at, destroy_fail, configured, destroyed;
static int pattern_object, actions_object, table_object;

int __wrap_rte_flow_info_get(uint16_t port, struct rte_flow_port_info *info, struct rte_flow_queue_info *queue,
    struct rte_flow_error *error);
int __wrap_rte_flow_configure(uint16_t port, const struct rte_flow_port_attr *attr, uint16_t queues,
    const struct rte_flow_queue_attr *attributes[], struct rte_flow_error *error);
struct rte_flow_pattern_template *__wrap_rte_flow_pattern_template_create(uint16_t port,
    const struct rte_flow_pattern_template_attr *attr, const struct rte_flow_item items[],
    struct rte_flow_error *error);
struct rte_flow_actions_template *__wrap_rte_flow_actions_template_create(uint16_t port,
    const struct rte_flow_actions_template_attr *attr, const struct rte_flow_action actions[],
    const struct rte_flow_action masks[], struct rte_flow_error *error);
struct rte_flow_template_table *__wrap_rte_flow_template_table_create(uint16_t port,
    const struct rte_flow_template_table_attr *attr, struct rte_flow_pattern_template *patterns[], uint8_t np,
    struct rte_flow_actions_template *actions[], uint8_t na, struct rte_flow_error *error);
int __wrap_rte_flow_template_table_destroy(uint16_t port, struct rte_flow_template_table *table,
    struct rte_flow_error *error);
int __wrap_rte_flow_actions_template_destroy(uint16_t port, struct rte_flow_actions_template *actions,
    struct rte_flow_error *error);
int __wrap_rte_flow_pattern_template_destroy(uint16_t port, struct rte_flow_pattern_template *pattern,
    struct rte_flow_error *error);

int __wrap_rte_flow_info_get(uint16_t port, struct rte_flow_port_info *info, struct rte_flow_queue_info *queue,
    struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7);
    if (fail_at == 9)
        return -ENOTSUP;
    *info = (struct rte_flow_port_info){.max_nb_queues = 8,
        .max_nb_counters = 16384,
        .max_nb_aging_objects = 4096,
        .supported_flags = RTE_FLOW_PORT_FLAG_STRICT_QUEUE};
    if (fail_at == 10)
        info->supported_flags = 0;
    queue->max_size = 256;
    return 0;
}

int __wrap_rte_flow_configure(uint16_t port, const struct rte_flow_port_attr *attr, uint16_t queues,
    const struct rte_flow_queue_attr *attributes[], struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && queues == 8 && attr->nb_counters == 16384 && attr->nb_aging_objects == 4096);
    assert(attr->flags == RTE_FLOW_PORT_FLAG_STRICT_QUEUE);
    for (unsigned int i = 0; i < queues; ++i)
        assert(attributes[i]->size == 256);
    ++configured;
    return 0;
}

static bool fail(void)
{
    if (++phase != fail_at)
        return false;
    rte_errno = ENOSPC;
    return true;
}

struct rte_flow_pattern_template *__wrap_rte_flow_pattern_template_create(uint16_t port,
    const struct rte_flow_pattern_template_attr *attr, const struct rte_flow_item items[], struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && attr->transfer && !attr->ingress && !attr->egress && !attr->relaxed_matching);
    assert(items[0].type == RTE_FLOW_ITEM_TYPE_ETH && items[1].type == RTE_FLOW_ITEM_TYPE_IPV4);
    const struct rte_flow_item_ipv4 *mask = items[1].mask;
    assert(mask->hdr.src_addr == UINT32_MAX && mask->hdr.dst_addr == 0);
    assert(items[2].type == RTE_FLOW_ITEM_TYPE_END);
    return fail() ? NULL : (struct rte_flow_pattern_template *)&pattern_object;
}

struct rte_flow_actions_template *__wrap_rte_flow_actions_template_create(uint16_t port,
    const struct rte_flow_actions_template_attr *attr, const struct rte_flow_action actions[],
    const struct rte_flow_action masks[], struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && attr->transfer && !attr->ingress && !attr->egress);
    assert(actions[0].type == RTE_FLOW_ACTION_TYPE_MARK && masks[0].type == actions[0].type);
    assert(((const struct rte_flow_action_mark *)actions[0].conf)->id == 42);
    assert(((const struct rte_flow_action_mark *)masks[0].conf)->id == 0);
    assert(actions[1].type == RTE_FLOW_ACTION_TYPE_REPRESENTED_PORT);
    assert(((const struct rte_flow_action_ethdev *)masks[1].conf)->port_id == UINT16_MAX);
    assert(actions[2].type == RTE_FLOW_ACTION_TYPE_END && masks[2].type == RTE_FLOW_ACTION_TYPE_END);
    return fail() ? NULL : (struct rte_flow_actions_template *)&actions_object;
}

struct rte_flow_template_table *__wrap_rte_flow_template_table_create(uint16_t port,
    const struct rte_flow_template_table_attr *attr, struct rte_flow_pattern_template *patterns[], uint8_t np,
    struct rte_flow_actions_template *actions[], uint8_t na, struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && np == 1 && na == 1 && attr->nb_flows == 16384);
    assert(attr->flow_attr.transfer && !attr->flow_attr.ingress && attr->flow_attr.group == 1);
    assert(attr->flow_attr.priority == 3);
    assert(patterns[0] == (struct rte_flow_pattern_template *)&pattern_object);
    assert(actions[0] == (struct rte_flow_actions_template *)&actions_object);
    return fail() ? NULL : (struct rte_flow_template_table *)&table_object;
}

int __wrap_rte_flow_template_table_destroy(uint16_t port, struct rte_flow_template_table *table,
    struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && table == (struct rte_flow_template_table *)&table_object && destroyed == 0);
    if (destroy_fail == 1)
        return -EIO;
    ++destroyed;
    return 0;
}

int __wrap_rte_flow_actions_template_destroy(uint16_t port, struct rte_flow_actions_template *actions,
    struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && actions == (struct rte_flow_actions_template *)&actions_object);
    if (destroy_fail == 2)
        return -EIO;
    ++destroyed;
    return 0;
}

int __wrap_rte_flow_pattern_template_destroy(uint16_t port, struct rte_flow_pattern_template *pattern,
    struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && pattern == (struct rte_flow_pattern_template *)&pattern_object);
    if (destroy_fail == 3)
        return -EIO;
    ++destroyed;
    return 0;
}

/** 直接检查传给真实公共 API 的结构、掩码、方向和清理顺序，不靠模拟内部结果推断 */
static void test_native(void)
{
    struct dppd_flow_template_driver driver = dppd_native_flow_template_driver();
    struct dppd_flow_template_spec shape = {.capacity = 16384, .dynamic_actions = 1};
    struct dppd_flow_template_parts parts;
    shape.example = (struct dppd_rule){.id = 1,
        .install_port_id = 7,
        .domain = DPPD_RULE_DOMAIN_TRANSFER,
        .group = 1,
        .priority = 3,
        .nb_matches = 2,
        .nb_actions = 2};
    shape.example.matches[0].type = DPPD_MATCH_ETH;
    shape.example.matches[1].type = DPPD_MATCH_IPV4;
    shape.example.matches[1].spec.ipv4.src_mask_be = UINT32_MAX;
    shape.example.actions[0] = (struct dppd_action){.type = DPPD_ACTION_MARK, .conf.mark_id = 42};
    shape.example.actions[1] = (struct dppd_action){.type = DPPD_ACTION_REPRESENTED_PORT, .conf.ethdev_port_id = 8};
    assert(dppd_native_flow_configure(7, 8, 256, 16384, 4096) == 0 && configured == 1);
    assert(dppd_native_flow_configure(7, 8, 256, 16385, 4096) == -ENOSPC && configured == 1);
    assert(dppd_native_flow_configure(7, 8, 256, 16384, 4097) == -ENOSPC && configured == 1);
    assert(dppd_native_flow_configure(7, 0, 256, 0, 0) == -EINVAL);
    fail_at = 9;
    assert(dppd_native_flow_configure(7, 8, 256, 16384, 4096) == -ENOTSUP && configured == 1);
    fail_at = 10;
    assert(dppd_native_flow_configure(7, 8, 256, 16384, 4096) == -ENOTSUP && configured == 1);
    for (unsigned int failure = 0; failure <= 3; ++failure) {
        phase = destroyed = 0;
        fail_at = failure;
        parts = (struct dppd_flow_template_parts){0};
        int rc = driver.create(driver.context, &shape, &parts);
        assert(rc == (failure == 0 ? 0 : -ENOSPC));
        assert(driver.destroy(driver.context, 7, &parts) == 0);
        assert(parts.table == NULL && parts.actions == NULL && parts.pattern == NULL);
    }
    fail_at = 0;
    for (destroy_fail = 1; destroy_fail <= 3; ++destroy_fail) {
        phase = destroyed = 0;
        parts = (struct dppd_flow_template_parts){0};
        assert(driver.create(driver.context, &shape, &parts) == 0);
        assert(driver.destroy(driver.context, 7, &parts) == -EIO);
        assert(parts.pattern != NULL);
        unsigned int saved = destroy_fail;
        destroy_fail = 0;
        assert(driver.destroy(driver.context, 7, &parts) == 0);
        assert(destroyed == 3 && parts.pattern == NULL && parts.actions == NULL && parts.table == NULL);
        destroy_fail = saved;
    }
}
#endif

int main(void)
{
#if RTE_VERSION >= RTE_VERSION_NUM(24, 11, 0, 0)
    test_native();
#else
    struct dppd_flow_template_driver driver = dppd_native_flow_template_driver();
    struct dppd_flow_template_spec shape = {0};
    struct dppd_flow_template_parts parts = {0};
    assert(dppd_native_flow_configure(7, 8, 256, 16384, 4096) == -ENOTSUP);
    assert(driver.create(driver.context, &shape, &parts) == -ENOTSUP && parts.table == NULL);
#endif
    return 0;
}
