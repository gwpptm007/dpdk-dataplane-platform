#define ALLOW_EXPERIMENTAL_API
#include <assert.h>
#include <errno.h>
#include <rte_errno.h>
#include <rte_flow.h>
#include <rte_version.h>
#include "dppd/async_flow.h"

#if RTE_VERSION >= RTE_VERSION_NUM(24, 11, 0, 0)
static const struct rte_flow_item *saved_pattern;
static const struct rte_flow_action *saved_actions;
static uintptr_t pending_tag;
static bool create_error, destroy_error, completion_error;
static bool aged_error;
static unsigned int creates, destroys, pushes, pulls;
static int flow_object, table_object;

struct rte_flow *__wrap_rte_flow_async_create(uint16_t port, uint32_t queue, const struct rte_flow_op_attr *attr,
    struct rte_flow_template_table *table, const struct rte_flow_item items[], uint8_t pattern_index,
    const struct rte_flow_action actions[], uint8_t actions_index, void *user_data, struct rte_flow_error *error);
int __wrap_rte_flow_async_destroy(uint16_t port, uint32_t queue, const struct rte_flow_op_attr *attr,
    struct rte_flow *flow, void *user_data, struct rte_flow_error *error);
int __wrap_rte_flow_push(uint16_t port, uint32_t queue, struct rte_flow_error *error);
int __wrap_rte_flow_pull(uint16_t port, uint32_t queue, struct rte_flow_op_result results[], uint16_t count,
    struct rte_flow_error *error);
int __wrap_rte_flow_get_q_aged_flows(uint16_t port, uint32_t queue, void **contexts, uint32_t count,
    struct rte_flow_error *error);

struct rte_flow *__wrap_rte_flow_async_create(uint16_t port, uint32_t queue, const struct rte_flow_op_attr *attr,
    struct rte_flow_template_table *table, const struct rte_flow_item items[], uint8_t pattern_index,
    const struct rte_flow_action actions[], uint8_t actions_index, void *user_data, struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && queue == 3 && attr->postpone && pattern_index == 0 && actions_index == 0);
    assert(table == (struct rte_flow_template_table *)&table_object && user_data != NULL);
    ++creates;
    if (create_error) {
        rte_errno = ENOSPC;
        return NULL;
    }
    pending_tag = (uintptr_t)user_data;
    saved_pattern = items;
    saved_actions = actions;
    return (struct rte_flow *)&flow_object;
}

int __wrap_rte_flow_async_destroy(uint16_t port, uint32_t queue, const struct rte_flow_op_attr *attr,
    struct rte_flow *flow, void *user_data, struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && queue == 3 && attr->postpone && flow == (struct rte_flow *)&flow_object && user_data != NULL);
    ++destroys;
    if (destroy_error)
        return -ENOSPC;
    pending_tag = (uintptr_t)user_data;
    return 0;
}

/** 在 enqueue 返回后再次读取所有自引用参数，检查适配器没有留下失效栈地址 */
static void check_storage(void)
{
    assert(saved_pattern[0].type == RTE_FLOW_ITEM_TYPE_ETH);
    assert(saved_pattern[1].type == RTE_FLOW_ITEM_TYPE_IPV4);
    assert(((const struct rte_flow_item_ipv4 *)saved_pattern[1].spec)->hdr.src_addr == 123);
    assert(((const struct rte_flow_item_ipv4 *)saved_pattern[1].mask)->hdr.src_addr == UINT32_MAX);
    assert(saved_actions[0].type == RTE_FLOW_ACTION_TYPE_MARK);
    assert(((const struct rte_flow_action_mark *)saved_actions[0].conf)->id == 42);
    assert(saved_actions[1].type == RTE_FLOW_ACTION_TYPE_AGE);
    const struct rte_flow_action_age *age = saved_actions[1].conf;
    assert(age->timeout == 7 && age->context == (void *)(uintptr_t)9);
    assert(saved_actions[2].type == RTE_FLOW_ACTION_TYPE_DROP && saved_actions[3].type == RTE_FLOW_ACTION_TYPE_END);
}

int __wrap_rte_flow_push(uint16_t port, uint32_t queue, struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && queue == 3);
    check_storage();
    ++pushes;
    return 0;
}

int __wrap_rte_flow_pull(uint16_t port, uint32_t queue, struct rte_flow_op_result results[], uint16_t count,
    struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && queue == 3 && count > 0);
    check_storage();
    ++pulls;
    results[0] = (struct rte_flow_op_result){.user_data = (void *)pending_tag,
        .status = completion_error ? RTE_FLOW_OP_ERROR : RTE_FLOW_OP_SUCCESS};
    return 1;
}

int __wrap_rte_flow_get_q_aged_flows(uint16_t port, uint32_t queue, void **contexts, uint32_t count,
    struct rte_flow_error *error)
{
    (void)error;
    assert(port == 7 && queue == 3 && count == 2);
    if (aged_error)
        return -ENOMEM;
    contexts[0] = (void *)(uintptr_t)9;
    contexts[1] = (void *)UINTPTR_MAX;
    return 2;
}

static void test_native(void)
{
    struct dppd_async_flow_driver driver = dppd_native_async_flow_driver();
    struct dppd_rule rule = {.id = 1, .generation = 1, .install_port_id = 7, .nb_matches = 2, .nb_actions = 3};
    struct dppd_async_driver_result result;
    void *object = NULL;
    rule.matches[0].type = DPPD_MATCH_ETH;
    rule.matches[1].type = DPPD_MATCH_IPV4;
    rule.matches[1].spec.ipv4.src_be = 123;
    rule.matches[1].spec.ipv4.src_mask_be = UINT32_MAX;
    rule.actions[0] = (struct dppd_action){.type = DPPD_ACTION_MARK, .conf.mark_id = 42};
    rule.actions[1] = (struct dppd_action){.type = DPPD_ACTION_AGE, .conf.age_timeout_seconds = 7};
    rule.actions[2].type = DPPD_ACTION_DROP;
    assert(driver.create(NULL, 7, 3, NULL, &rule, 9, &object) == -EINVAL && object == NULL);
    create_error = true;
    assert(driver.create(NULL, 7, 3, &table_object, &rule, 9, &object) == -ENOSPC && object == NULL);
    create_error = false;
    assert(driver.create(NULL, 7, 3, &table_object, &rule, 9, &object) == 0 && object != NULL);
    rule.matches[1].spec.ipv4.src_be = 456;
    rule.actions[0].conf.mark_id = 777;
    assert(driver.push(NULL, 7, 3) == 0);
    completion_error = true;
    assert(driver.pull(NULL, 7, 3, &result, 1) == 1 && result.tag == 9 && result.status == -EIO);
    uintptr_t tags[2] = {77, 88};
    aged_error = true;
    assert(driver.aged(NULL, 7, 3, tags, 2) == -ENOMEM && tags[0] == 77 && tags[1] == 88);
    aged_error = false;
    assert(driver.aged(NULL, 7, 3, tags, 2) == 2 && tags[0] == 9 && tags[1] == UINTPTR_MAX);
    /** 创建失败完成也继续使用原始句柄，直到删除成功完成才释放包装存储 */
    destroy_error = true;
    assert(driver.destroy(NULL, 7, 3, object, 10) == -ENOSPC);
    check_storage();
    destroy_error = false;
    assert(driver.destroy(NULL, 7, 3, object, 11) == 0);
    assert(driver.push(NULL, 7, 3) == 0);
    completion_error = false;
    assert(driver.pull(NULL, 7, 3, &result, 1) == 1 && result.tag == 11 && result.status == 0);
    driver.release(NULL, object);
    assert(creates == 2 && destroys == 2 && pushes == 2 && pulls == 2);
}
#endif

int main(void)
{
#if RTE_VERSION >= RTE_VERSION_NUM(24, 11, 0, 0)
    test_native();
#else
    struct dppd_async_flow_driver driver = dppd_native_async_flow_driver();
    struct dppd_rule rule = {0};
    struct dppd_async_driver_result result;
    void *object = NULL;
    assert(driver.create(NULL, 7, 0, NULL, &rule, 1, &object) == -ENOTSUP && object == NULL);
    assert(driver.destroy(NULL, 7, 0, NULL, 1) == -ENOTSUP);
    assert(driver.push(NULL, 7, 0) == -ENOTSUP);
    assert(driver.pull(NULL, 7, 0, &result, 1) == -ENOTSUP);
    uintptr_t tag = 77;
    assert(driver.aged(NULL, 7, 0, &tag, 1) == -ENOTSUP && tag == 77);
#endif
    return 0;
}
