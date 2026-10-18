#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "dppd/flow_template.h"

static unsigned int fail_allocation, fail_create, fail_destroy, calls;
static int pattern_object, actions_object, table_object;

void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_allocation != 0 && --fail_allocation == 0)
        return NULL;
    return __real_calloc(count, size);
}

static struct dppd_flow_template_spec spec(void)
{
    struct dppd_flow_template_spec value = {.capacity = 2, .dynamic_actions = 1};
    value.example = (struct dppd_rule){.id = 1,
        .install_port_id = 7,
        .fallback = DPPD_FALLBACK_SOFTWARE_ONLY,
        .nb_matches = 2,
        .nb_actions = 2};
    value.example.matches[0].type = DPPD_MATCH_ETH;
    value.example.matches[1].type = DPPD_MATCH_IPV4;
    value.example.matches[1].spec.ipv4.src_mask_be = UINT32_MAX;
    value.example.actions[0] = (struct dppd_action){.type = DPPD_ACTION_MARK, .conf.mark_id = 23};
    value.example.actions[1].type = DPPD_ACTION_DROP;
    return value;
}

static struct dppd_admission *admission(void)
{
    struct dppd_admission_config config = {.ticket_capacity = 128, .port_count = 1};
    struct dppd_admission *result = NULL;
    config.ports[0].port_id = 7;
    config.limits[DPPD_RESOURCE_TEMPLATE_TABLE] = 64;
    config.ports[0].limits[DPPD_RESOURCE_TEMPLATE_TABLE] = 64;
    assert(dppd_admission_create(&config, &result) == 0);
    return result;
}

static void test_shape(void)
{
    struct dppd_flow_template_spec shape = spec();
    struct dppd_rule rule = shape.example;
    rule.id = 99;
    rule.generation = 12;
    rule.matches[1].spec.ipv4.src_be = 123;
    rule.actions[0].conf.mark_id = 777;
    assert(dppd_flow_template_matches(&shape, &rule));
    shape.dynamic_actions = 0;
    assert(!dppd_flow_template_matches(&shape, &rule));
    shape.dynamic_actions = 1;
    rule.matches[1].spec.ipv4.src_mask_be = 0;
    assert(!dppd_flow_template_matches(&shape, &rule));
    rule = shape.example;
    rule.group = 3;
    assert(!dppd_flow_template_matches(&shape, &rule));
    rule = shape.example;
    rule.install_port_id = 8;
    assert(!dppd_flow_template_matches(&shape, &rule));
    shape.dynamic_actions = 4;
    assert(!dppd_flow_template_matches(&shape, &shape.example));
    assert(!dppd_flow_template_matches(NULL, &rule));
    shape = spec();
    shape.example.matches[1].type = DPPD_MATCH_IPV6;
    shape.example.matches[1].spec = (typeof(shape.example.matches[1].spec)){0};
    memset(shape.example.matches[1].spec.ipv6.dst_mask, 0xff, 16);
    rule = shape.example;
    rule.matches[1].spec.ipv6.dst[15] = 55;
    assert(dppd_flow_template_matches(&shape, &rule));
    rule.matches[1].spec.ipv6.dst_mask[15] = 0;
    assert(!dppd_flow_template_matches(&shape, &rule));
}

static void test_lease_and_capacity(void)
{
    struct dppd_admission *quota = admission();
    struct dppd_flow_templates *tables = NULL, *other = NULL;
    struct dppd_flow_template_spec shape = spec();
    struct dppd_flow_template_id id, old, ids[64], untouched = {.owner = 123};
    struct dppd_flow_template_lease leases[2], held;
    struct dppd_flow_template_info info;
    struct dppd_admission_snapshot usage;
    unsigned int i;

    for (i = 1; i <= 2; ++i) {
        fail_allocation = i;
        assert(dppd_flow_templates_create(quota, 2, NULL, &tables) == -ENOMEM && tables == NULL);
    }
    assert(dppd_flow_templates_create(quota, 2, NULL, &tables) == 0);
    assert(dppd_flow_templates_create(quota, 2, NULL, &other) == 0);
    assert(dppd_flow_template_create(tables, &shape, &id) == 0);
    old = id;
    assert(dppd_flow_templates_close(&tables) == -EBUSY);
    assert(dppd_flow_template_acquire(tables, &id, &shape.example, &leases[0]) == 0);
    assert(dppd_flow_template_acquire(tables, &id, &shape.example, &leases[1]) == 0);
    held = leases[0];
    assert(dppd_flow_template_acquire(tables, &id, &shape.example, &held) == -ENOSPC);
    assert(held.serial == leases[0].serial);
    assert(dppd_flow_template_destroy(tables, &id) == -EBUSY);
    assert(dppd_flow_template_get(tables, &id, &info) == 0 && info.references == 2);
    assert(dppd_flow_template_release(other, &leases[0]) == -ESTALE);
    assert(dppd_flow_template_release(tables, &leases[0]) == 0);
    assert(dppd_flow_template_release(tables, &leases[0]) == -ESTALE);
    assert(dppd_flow_template_acquire(tables, &id, &shape.example, &held) == 0);
    assert(dppd_flow_template_release(tables, &leases[0]) == -ESTALE);
    assert(dppd_flow_template_release(tables, &held) == 0);
    assert(dppd_flow_template_release(tables, &leases[1]) == 0);
    assert(dppd_flow_template_destroy(tables, &id) == 0);
    assert(dppd_flow_template_create(tables, &shape, &id) == 0);
    assert(dppd_flow_template_get(tables, &old, &info) == -ESTALE);
    assert(dppd_flow_template_destroy(tables, &old) == -ESTALE);
    assert(dppd_flow_template_destroy(tables, &id) == 0);
    for (i = 0; i < 64; ++i)
        assert(dppd_flow_template_create(tables, &shape, &ids[i]) == 0);
    assert(dppd_flow_template_create(tables, &shape, &untouched) == -ENOSPC && untouched.owner == 123);
    assert(dppd_flow_template_create(other, &shape, &untouched) == -ENOSPC && untouched.owner == 123);
    assert(dppd_admission_snapshot(quota, &usage) == 0);
    assert(usage.global.committed[DPPD_RESOURCE_TEMPLATE_TABLE] == 64);
    for (i = 0; i < 64; ++i)
        assert(dppd_flow_template_destroy(tables, &ids[i]) == 0);
    assert(dppd_flow_templates_close(&tables) == 0);
    assert(dppd_flow_templates_close(&other) == 0);
    assert(dppd_admission_close(&quota) == 0);
}

static int create_parts(void *context, const struct dppd_flow_template_spec *shape,
    struct dppd_flow_template_parts *parts)
{
    assert(context == &calls && shape->example.install_port_id == 7);
    ++calls;
    if (fail_create == 1)
        return -ENOMEM;
    parts->pattern = &pattern_object;
    if (fail_create == 2)
        return -EIO;
    parts->actions = &actions_object;
    if (fail_create == 3)
        return -EIO;
    parts->table = &table_object;
    return 0;
}

static int destroy_parts(void *context, uint16_t port, struct dppd_flow_template_parts *parts)
{
    assert(context == &calls && port == 7);
    ++calls;
    if (fail_destroy == 1)
        return -EIO;
    parts->table = NULL;
    if (fail_destroy == 2)
        return -EIO;
    parts->actions = NULL;
    if (fail_destroy == 3)
        return -EIO;
    parts->pattern = NULL;
    return 0;
}

/** 每一个创建边界与清理边界相交，残留必须持有额度且只能重试删除 */
static void test_partial_failures(void)
{
    struct dppd_admission *quota = admission();
    struct dppd_flow_templates *tables = NULL;
    struct dppd_flow_template_driver driver = {.context = &calls, .create = create_parts, .destroy = destroy_parts};
    struct dppd_flow_template_spec shape = spec();
    struct dppd_flow_template_id id;
    struct dppd_flow_template_info info;
    struct dppd_flow_template_lease lease;
    struct dppd_admission_snapshot usage;

    assert(dppd_flow_templates_create(quota, 8, &driver, &tables) == 0);
    for (fail_create = 0; fail_create <= 3; ++fail_create) {
        for (fail_destroy = 0; fail_destroy <= 3; ++fail_destroy) {
            unsigned int saved_create = fail_create, saved_destroy = fail_destroy;
            id = (struct dppd_flow_template_id){.owner = 987};
            int rc = dppd_flow_template_create(tables, &shape, &id);
            if (fail_create == 1 || (fail_create != 0 && fail_destroy == 0)) {
                assert(rc < 0 && rc != -EUCLEAN && id.owner == 987);
            } else {
                assert(rc == (fail_create == 0 ? 0 : -EUCLEAN));
                assert(dppd_flow_template_get(tables, &id, &info) == 0);
                if (rc != 0) {
                    assert(info.residual);
                    assert(dppd_flow_template_acquire(tables, &id, &shape.example, &lease) == -EUCLEAN);
                    assert(dppd_admission_snapshot(quota, &usage) == 0 && usage.residual_tickets == 1);
                }
                rc = dppd_flow_template_destroy(tables, &id);
                if (fail_destroy != 0) {
                    assert(rc == -EUCLEAN);
                    fail_destroy = 0;
                    assert(dppd_flow_template_destroy(tables, &id) == 0);
                } else {
                    assert(rc == 0);
                }
            }
            assert(dppd_admission_snapshot(quota, &usage) == 0 && usage.held_tickets == 0);
            fail_create = saved_create;
            fail_destroy = saved_destroy;
        }
    }
    fail_create = fail_destroy = 0;
    assert(dppd_flow_templates_close(&tables) == 0);
    assert(dppd_admission_close(&quota) == 0);
}

int main(void)
{
    test_shape();
    test_lease_and_capacity();
    test_partial_failures();
    /** 逻辑保护不挤占实际流容量，实际流删除后仍不能销毁协调器正在使用的模板 */
    struct dppd_admission *quota = admission();
    struct dppd_flow_templates *tables = NULL;
    struct dppd_flow_template_spec shape = spec();
    struct dppd_flow_template_id id;
    struct dppd_flow_template_lease pin, flow;
    struct dppd_flow_template_info info;
    shape.capacity = 1;
    assert(dppd_flow_templates_create(quota, 3, NULL, &tables) == 0);
    assert(dppd_flow_template_create(tables, &shape, &id) == 0);
    assert(dppd_flow_template_pin(tables, &id, &shape.example, &pin) == 0);
    assert(dppd_flow_template_acquire(tables, &id, &shape.example, &flow) == 0);
    assert(dppd_flow_template_get(tables, &id, &info) == 0 && info.references == 2 && info.flows == 1);
    assert(dppd_flow_template_release(tables, &flow) == 0);
    assert(dppd_flow_template_destroy(tables, &id) == -EBUSY);
    assert(dppd_flow_template_get(tables, &id, &info) == 0 && info.references == 1 && info.flows == 0);
    assert(dppd_flow_template_acquire(tables, &id, &shape.example, &flow) == 0);
    assert(dppd_flow_template_release(tables, &pin) == 0);
    assert(dppd_flow_template_release(tables, &pin) == -ESTALE);
    assert(dppd_flow_template_release(tables, &flow) == 0);
    assert(dppd_flow_template_destroy(tables, &id) == 0);
    assert(dppd_flow_templates_close(&tables) == 0);
    assert(dppd_admission_close(&quota) == 0);
    return 0;
}
