#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_flow.h>
#include "dppd/recovery_guard.h"
#include "dppd/rte_flow_backend.h"

static uint8_t expected[DPPD_TAP_COOKIE_SIZE];
static unsigned int entropy_calls, validate_calls, create_calls;
static int entropy_mode, driver_error;
static bool sync_fail, expect_owner = true, tap_driver = true;
static struct dppd_recovery_guard *active_guard;
ssize_t __wrap_getrandom(void *buffer, size_t size, unsigned int flags);
int __real_fsync(int fd);
int __wrap_fsync(int fd);
int __wrap_rte_eth_dev_info_get(uint16_t port, struct rte_eth_dev_info *info);
int __wrap_rte_flow_validate(uint16_t port, const struct rte_flow_attr *attr,
    const struct rte_flow_item *items, const struct rte_flow_action *actions, struct rte_flow_error *error);
struct rte_flow *__wrap_rte_flow_create(uint16_t port, const struct rte_flow_attr *attr,
    const struct rte_flow_item *items, const struct rte_flow_action *actions, struct rte_flow_error *error);
int __wrap_rte_flow_destroy(uint16_t port, struct rte_flow *flow, struct rte_flow_error *error);

/** 可控随机源覆盖系统错误、全零、碰撞以及短读，失败不能消耗尝试编号 */
ssize_t __wrap_getrandom(void *buffer, size_t size, unsigned int flags)
{
    assert(flags == 0);
    entropy_calls++;
    if (entropy_mode == 1 || (entropy_mode == 4 && entropy_calls == 1)) {
        errno = entropy_mode == 1 ? EIO : EINTR;
        return -1;
    }
    if (entropy_mode == 4 && size > 3)
        size = 3;
    if (entropy_mode == 3)
        memcpy(buffer, expected, size);
    else
        memset(buffer, entropy_mode == 2 ? 0 : (int)(entropy_calls & 255U), size);
    return (ssize_t)size;
}

/** 写入后同步失败时，即使文件已有字节，也不能把标识返回给驱动 */
int __wrap_fsync(int fd)
{
    if (sync_fail) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

int __wrap_rte_eth_dev_info_get(uint16_t port, struct rte_eth_dev_info *info)
{
    assert(port == 0);
    memset(info, 0, sizeof(*info));
    info->driver_name = tap_driver ? "net_tap" : "net_ring";
    return 0;
}

/** 检查真正交给驱动的动作数组，标识不能覆盖 DROP/QUEUE 或丢失末尾 END */
static void check_actions(const struct rte_flow_attr *attr, const struct rte_flow_item *items,
    const struct rte_flow_action *actions)
{
    assert(attr->ingress && !attr->egress && !attr->transfer);
    assert(items[0].type == RTE_FLOW_ITEM_TYPE_ETH && items[1].type == RTE_FLOW_ITEM_TYPE_END);
    assert(actions[0].type == RTE_FLOW_ACTION_TYPE_DROP || actions[0].type == RTE_FLOW_ACTION_TYPE_QUEUE);
    if (expect_owner) {
        assert((int)actions[1].type == DPPD_TAP_ACTION_OWNER_V1 && actions[1].conf != NULL);
        const struct dppd_tap_owner_action *owner = actions[1].conf;
        assert(owner->version == 1 && owner->size == sizeof(*owner));
        assert(memcmp(owner->cookie, expected, sizeof(expected)) == 0);
        assert(actions[2].type == RTE_FLOW_ACTION_TYPE_END);
    } else
        assert(actions[1].type == RTE_FLOW_ACTION_TYPE_END);
}

int __wrap_rte_flow_validate(uint16_t port, const struct rte_flow_attr *attr,
    const struct rte_flow_item *items, const struct rte_flow_action *actions, struct rte_flow_error *error)
{
    (void)error;
    assert(port == 0);
    check_actions(attr, items, actions);
    validate_calls++;
    return driver_error;
}

struct rte_flow *__wrap_rte_flow_create(uint16_t port, const struct rte_flow_attr *attr,
    const struct rte_flow_item *items, const struct rte_flow_action *actions, struct rte_flow_error *error)
{
    (void)error;
    assert(port == 0);
    check_actions(attr, items, actions);
    create_calls++;
    if (active_guard != NULL) {
        assert(active_guard->record.attempt_count == 1);
        assert(active_guard->record.attempts[0].phase == DPPD_RECOVERY_INTENT);
        assert(memcmp(active_guard->record.attempts[0].owner_cookie, expected, sizeof(expected)) == 0);
    }
    rte_errno = -driver_error;
    return driver_error == 0 ? (struct rte_flow *)(uintptr_t)1 : NULL;
}

int __wrap_rte_flow_destroy(uint16_t port, struct rte_flow *flow, struct rte_flow_error *error)
{
    (void)error;
    assert(port == 0 && flow != NULL);
    return 0;
}

/** 磁盘记录中的身份无需真实网卡，内核身份采集另由真实 TAP 集成测试验证 */
static void prepare(struct dppd_recovery_guard *guard, const char *path, const char *state)
{
    const struct dppd_recovery_identity identity = {.ifindex = 17, .ifname = "test-tap",
        .boot_id = "01234567-89ab-cdef-0123-456789abcdef", .netns_device = 4, .netns_inode = 100};
    assert(dppd_recovery_guard_open(guard, path, state) == 0);
    assert(dppd_recovery_guard_prepare_identity(guard, 0, "net_tap0", "net_tap", 1, 1, &identity) == 0);
}

/** 在独立文件中验证持久化、熵源失败、重复拒绝和同步失败后的故障锁存 */
static void persistence(const char *path, const char *state)
{
    struct dppd_recovery_guard guard;
    uint8_t cookie[DPPD_TAP_COOKIE_SIZE];
    uint64_t attempt = 0;
    prepare(&guard, path, state);
    uint64_t revision = guard.record.revision;
    for (entropy_mode = 1; entropy_mode <= 2; ++entropy_mode) {
        memset(cookie, 0xff, sizeof(cookie));
        assert(dppd_recovery_guard_begin_owned(&guard, 0, 1, 1, &attempt, cookie) ==
            (entropy_mode == 1 ? -EIO : -EAGAIN));
        assert(attempt == 0 && !dppd_tap_cookie_present(cookie));
        assert(guard.record.revision == revision && guard.record.last_attempt == 0);
    }
    entropy_mode = 4;
    entropy_calls = 0;
    assert(dppd_recovery_guard_begin_owned(&guard, 0, 1, 1, &attempt, expected) == 0);
    assert(attempt == 1 && entropy_calls > 2 && dppd_tap_cookie_present(expected));
    entropy_mode = 3;
    assert(dppd_recovery_guard_begin_owned(&guard, 0, 1, 1, &attempt, cookie) == -EAGAIN);
    assert(attempt == 0 && guard.record.last_attempt == 1);
    dppd_recovery_guard_close(&guard);
    assert(dppd_recovery_guard_open(&guard, path, NULL) == 0);
    assert(guard.record.format == 4 && guard.record.attempts[0].phase == DPPD_RECOVERY_INTENT);
    assert(memcmp(guard.record.attempts[0].owner_cookie, expected, sizeof(expected)) == 0);
    assert(dppd_recovery_guard_acknowledge(&guard, guard.record.revision) == 0);
    dppd_recovery_guard_close(&guard);
    prepare(&guard, path, state);
    entropy_mode = 0;
    sync_fail = true;
    assert(dppd_recovery_guard_begin_owned(&guard, 0, 1, 1, &attempt, cookie) == -EIO);
    assert(guard.faulted && attempt == 0 && !dppd_tap_cookie_present(cookie));
    assert(dppd_recovery_guard_clean(&guard) == -EUCLEAN);
    sync_fail = false;
    dppd_recovery_guard_close(&guard);
    assert(unlink(path) == 0);
}

/** 验证支持范围与旧驱动拒绝，没有成功调用后的无标识重试 */
static void compiler(struct dppd_rule *rule)
{
    struct dppd_flow_error error;
    struct dppd_flow_handle handle;
    memset(expected, 0x12, sizeof(expected));
    assert(dppd_flow_validate_owned(0, rule, expected, &error) == 0);
    assert(dppd_flow_create_owned(0, rule, expected, &handle, &error) == 0);
    assert(dppd_flow_remove(&handle, &error) == 0);
    rule->actions[0].type = DPPD_ACTION_QUEUE;
    assert(dppd_flow_create_owned(0, rule, expected, &handle, &error) == 0);
    assert(dppd_flow_remove(&handle, &error) == 0);
    driver_error = -ENOTSUP;
    unsigned int created = create_calls;
    assert(dppd_flow_validate_owned(0, rule, expected, &error) == -ENOTSUP);
    assert(create_calls == created);
    memset(&handle, 0xff, sizeof(handle));
    assert(dppd_flow_create_owned(0, rule, expected, &handle, &error) == -ENOTSUP);
    assert(handle.flow == NULL && create_calls == created + 1);
    driver_error = 0;
    unsigned int validated = validate_calls;
    tap_driver = false;
    assert(dppd_flow_validate_owned(0, rule, expected, &error) == -ENOTSUP);
    tap_driver = true;
    rule->actions[0].type = DPPD_ACTION_DROP;
    rule->domain = DPPD_RULE_DOMAIN_EGRESS;
    assert(dppd_flow_validate_owned(0, rule, expected, &error) == -ENOTSUP);
    rule->domain = DPPD_RULE_DOMAIN_INGRESS;
    rule->actions[0].type = DPPD_ACTION_COUNT;
    rule->actions[1].type = DPPD_ACTION_DROP;
    rule->nb_actions = 2;
    assert(dppd_flow_validate_owned(0, rule, expected, &error) == -ENOTSUP);
    rule->nb_actions = 1;
    rule->actions[0].type = DPPD_ACTION_DROP;
    memset(expected, 0, sizeof(expected));
    assert(dppd_flow_validate_owned(0, rule, expected, &error) == -EINVAL);
    assert(dppd_flow_validate_owned(0, rule, NULL, &error) == -EINVAL);
    assert(validate_calls == validated);
    expect_owner = false;
    assert(dppd_flow_create(0, rule, &handle, &error) == 0);
    assert(dppd_flow_remove(&handle, &error) == 0);
    expect_owner = true;
}

static int before(void *context, uint16_t port, const struct dppd_rule *rule,
    uint64_t *attempt, uint8_t cookie[DPPD_TAP_COOKIE_SIZE])
{
    int rc = dppd_recovery_guard_begin_owned(context, port, rule->id, rule->generation, attempt, cookie);
    if (rc == 0)
        memcpy(expected, cookie, sizeof(expected));
    return rc;
}

/** 缺少原生创建接口时不得走普通 create，事务撤销只释放尚未安装的本地槽位 */
static void backend(const char *path, const char *state, const struct dppd_rule *rule)
{
    for (unsigned int missing = 0; missing < 2; ++missing) {
        struct dppd_recovery_guard guard;
        struct dppd_rte_flow_backend backend;
        struct dppd_transaction transaction;
        struct dppd_transaction_item item = {.rule = *rule};
        struct dppd_transaction_backends backends = {0};
        prepare(&guard, path, state);
        active_guard = &guard;
        assert(dppd_rte_flow_backend_init(&backend, 2, NULL) == 0);
        if (missing)
            backend.api.create_owned = NULL;
        backend.before_create = before;
        backend.before_create_context = &guard;
        backends.rte_flow = dppd_rte_flow_transaction_backend(&backend);
        item.plan.rule_id = rule->id;
        item.plan.rule_generation = rule->generation;
        item.plan.backend = DPPD_PLAN_BACKEND_RTE_FLOW;
        assert(dppd_transaction_init(&transaction, 1, &item, 1) == 0);
        unsigned int created = create_calls;
        /** 事务的基础 validate 不含私有动作，before 中的正式标识另行持久化 */
        expect_owner = false;
        assert(backends.rte_flow.validate(backends.rte_flow.context, &item) == 0);
        expect_owner = true;
        uintptr_t token;
        assert(backends.rte_flow.prepare(backends.rte_flow.context, &item, &token) == 0);
        int rc = backends.rte_flow.commit(backends.rte_flow.context, &item, token);
        assert(rc == (missing ? -ENOTSUP : 0));
        assert(create_calls == created + (missing ? 0U : 1U));
        assert(dppd_rte_flow_backend_fini(&backend) == 0);
        dppd_recovery_guard_close(&guard);
        active_guard = NULL;
        assert(unlink(path) == 0);
    }
}

int main(void)
{
    char directory[] = "/tmp/dppd-owner-unit-XXXXXX", path[256], state[256];
    struct dppd_rule rule = {.id = 1, .generation = 1, .nb_matches = 1, .nb_actions = 1,
        .matches = {{.type = DPPD_MATCH_ETH}}, .actions = {{.type = DPPD_ACTION_DROP}}};
    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/recovery", directory);
    snprintf(state, sizeof(state), "%s/state", directory);
    persistence(path, state);
    compiler(&rule);
    backend(path, state, &rule);
    assert(rmdir(directory) == 0);
    return 0;
}
