#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dppd/recovery_guard.h"
#include "dppd/rte_flow_backend.h"

static unsigned int sync_count, fail_sync, creates, removes;
static bool fail_result, fail_delete_record, partial_failure;
static struct dppd_recovery_guard *current_guard;
int __real_fsync(int fd);
int __wrap_fsync(int fd);

/** 改坏字段后重新计算 CRC，确保拒绝来自语义和规范编码检查，而非仅依赖校验和 */
static void damaged_attempts(const char *path)
{
    unsigned char original[37952], damaged[37952];
    const unsigned int offsets[] = {26, 24, 28, 36, 56, 95, 112};
    struct dppd_recovery_guard guard;
    int fd = open(path, O_RDWR);
    assert(fd >= 0 && pread(fd, original, sizeof(original), 0) == sizeof(original));
    for (unsigned int case_id = 0; case_id < sizeof(offsets) / sizeof(offsets[0]); ++case_id) {
        memcpy(damaged, original, sizeof(damaged));
        damaged[9280 + offsets[case_id]] = offsets[case_id] == 56 ? 0 : 99;
        memset(damaged + 28, 0, 4);
        uint32_t crc = UINT32_MAX;
        for (size_t index = 0; index < sizeof(damaged); ++index) {
            crc ^= damaged[index];
            for (unsigned int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
        crc = ~crc;
        for (unsigned int index = 0; index < 4; ++index)
            damaged[28 + index] = (unsigned char)(crc >> (index * 8));
        assert(pwrite(fd, damaged, sizeof(damaged), 0) == sizeof(damaged));
        assert(dppd_recovery_guard_open(&guard, path, NULL) == -EBADMSG);
    }
    assert(pwrite(fd, original, sizeof(original), 0) == sizeof(original));
    assert(close(fd) == 0);
}

/** 在真实文件上精确模拟某次同步失败，确认写盘故障不会遗失已经创建的驱动对象 */
int __wrap_fsync(int fd)
{
    if (++sync_count == fail_sync) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

/** 先有端口保护再有逐次意图，正常返回时才把尝试编号交给 backend 槽位 */
static int begin(void *context, uint16_t port, const struct dppd_rule *rule, uint64_t *attempt,
    uint8_t cookie[DPPD_TAP_COOKIE_SIZE])
{
    memset(cookie, 0, DPPD_TAP_COOKIE_SIZE);
    int rc = dppd_recovery_guard_prepare(context, port, "test-device", "test-driver", rule->id, rule->generation);
    return rc == 0 ? dppd_recovery_guard_begin(context, port, rule->id, rule->generation, attempt) : rc;
}

/** 测试没有真实内核坐标，明确记录无法观察，不把不存在的候选伪造成证据 */
static int created(void *context, uint64_t attempt, int error)
{
    struct dppd_recovery_guard *guard = context;
    if (fail_result)
        fail_sync = sync_count + 1;
    int rc = dppd_recovery_guard_created(guard, attempt, error, DPPD_EVIDENCE_UNAVAILABLE, -ENOTSUP, NULL);
    if (rc != 0)
        guard->faulted = true;
    return rc;
}

/** 删除记录失败只能锁存恢复故障，不能阻止真实驱动释放已经持有的 handle */
static void removed(void *context, uint64_t attempt, int error)
{
    struct dppd_recovery_guard *guard = context;
    if (fail_delete_record)
        fail_sync = sync_count + 1;
    if (dppd_recovery_guard_removed(guard, attempt, error) != 0)
        guard->faulted = true;
}

static int validate(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_error *error)
{
    (void)port; (void)rule; (void)error;
    return 0;
}

/** 驱动执行之前磁盘意图已经成功同步，即使随后报告创建错误也可以留下待清理 handle */
static int create(uint16_t port, const struct dppd_rule *rule,
    struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    assert(current_guard->record.attempt_count == 1);
    assert(current_guard->record.attempts[0].phase == DPPD_RECOVERY_INTENT);
    assert(!current_guard->faulted);
    creates++;
    *handle = (struct dppd_flow_handle){.port_id = port, .rule_id = rule->id,
        .rule_generation = rule->generation, .flow = (struct rte_flow *)(uintptr_t)1};
    return partial_failure ? -EFAULT : 0;
}

static int remove_flow(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    assert(handle->flow != NULL);
    removes++;
    memset(handle, 0, sizeof(*handle));
    return 0;
}

/** 创建前、创建后和删除后的同步故障分别覆盖，所有实际 handle 最终都由驱动删除 */
static void backend_faults(const char *path, const char *state)
{
    const struct dppd_flow_api api = {.validate = validate, .create = create, .remove = remove_flow};
    for (unsigned int mode = 0; mode < 4; ++mode) {
        struct dppd_recovery_guard guard;
        struct dppd_rte_flow_backend backend;
        struct dppd_transaction transaction;
        struct dppd_transaction_item item = {0};
        struct dppd_transaction_backends backends = {0};
        creates = removes = 0;
        fail_result = mode == 1;
        fail_delete_record = mode == 2;
        partial_failure = mode == 3;
        assert(dppd_recovery_guard_open(&guard, path, state) == 0);
        current_guard = &guard;
        assert(dppd_recovery_guard_prepare(&guard, 0, "test-device", "test-driver", 1, 1) == 0);
        assert(dppd_rte_flow_backend_init(&backend, 2, &api) == 0);
        backend.before_create = begin;
        backend.before_create_context = &guard;
        backend.after_create = created;
        backend.after_remove = removed;
        backends.rte_flow = dppd_rte_flow_transaction_backend(&backend);
        item.rule.id = item.rule.generation = 1;
        item.plan.rule_id = item.plan.rule_generation = 1;
        item.plan.backend = DPPD_PLAN_BACKEND_RTE_FLOW;
        assert(dppd_transaction_init(&transaction, 1, &item, 1) == 0);
        if (mode == 0)
            fail_sync = sync_count + 1;
        int rc = dppd_transaction_run(&transaction, &backends);
        assert(rc == (mode < 2 ? -EIO : mode == 2 ? 0 : -EFAULT));
        if (mode == 2)
            assert(dppd_rte_flow_backend_remove_version(&backend, 1, 1, NULL) == 0);
        assert(creates == (mode == 0 ? 0U : 1U) && removes == creates);
        assert(backend.count == 0 && dppd_rte_flow_backend_fini(&backend) == 0);
        assert(dppd_recovery_guard_clean(&guard) == (mode == 3 ? 0 : -EUCLEAN));
        dppd_recovery_guard_close(&guard);
        assert(dppd_recovery_guard_open(&guard, path, NULL) == 0);
        if (mode != 3) {
            assert(guard.record.count == 1 && guard.record.attempt_count == 1);
            assert(dppd_recovery_guard_acknowledge(&guard, guard.record.revision) == 0);
        }
        dppd_recovery_guard_close(&guard);
    }
}

/** 同版本重新创建获得新编号，失败与意图记录保留，只有删除成功的槽位允许复用 */
static void lifecycle(const char *path, const char *state)
{
    struct dppd_recovery_guard guard;
    struct dppd_recovery_identity identity = {.ifindex = 10, .ifname = "test-tap",
        .boot_id = "01234567-89ab-cdef-0123-456789abcdef", .netns_device = 4, .netns_inode = 100};
    struct dppd_recovery_filter candidate = {.parent = 0x10000, .handle = 0xabc,
        .priority = 10, .protocol = 3, .kind = "flower"};
    uint64_t first, second;
    assert(dppd_recovery_guard_open(&guard, path, state) == 0);
    assert(dppd_recovery_guard_begin(&guard, 0, 9, 7, &first) == -ENOENT);
    assert(dppd_recovery_guard_prepare_identity(&guard, 0, "net_tap0", "net_tap", 9, 7, &identity) == 0);
    assert(dppd_recovery_guard_begin(&guard, 0, 9, 7, &first) == 0);
    assert(dppd_recovery_guard_clean(&guard) == -EUCLEAN);
    assert(dppd_recovery_guard_created(&guard, first, 0, DPPD_EVIDENCE_SINGLE_ADDITION, 0, &candidate) == 0);
    assert(dppd_recovery_guard_created(&guard, first, 0, DPPD_EVIDENCE_NONE, 0, NULL) == -EALREADY);
    assert(dppd_recovery_guard_removed(&guard, first, -EIO) == 0);
    assert(guard.record.attempts[0].phase == DPPD_RECOVERY_CREATED && guard.record.attempts[0].remove_error == -EIO);
    dppd_recovery_guard_close(&guard);
    damaged_attempts(path);
    assert(dppd_recovery_guard_open(&guard, path, NULL) == 0);
    assert(guard.record.attempts[0].candidate.handle == 0xabc && guard.record.attempts[0].rule_id == 9);
    assert(dppd_recovery_guard_begin(&guard, 0, 9, 7, &second) == -EUCLEAN);
    assert(dppd_recovery_guard_acknowledge(&guard, guard.record.revision) == 0);
    dppd_recovery_guard_close(&guard);
    assert(dppd_recovery_guard_open(&guard, path, state) == 0);
    assert(dppd_recovery_guard_prepare(&guard, 0, "test-device", "test-driver", 9, 7) == 0);
    assert(dppd_recovery_guard_begin(&guard, 0, 9, 7, &second) == 0 && second == first + 1);
    assert(dppd_recovery_guard_created(&guard, second, -EIO, DPPD_EVIDENCE_AMBIGUOUS, 0, NULL) == 0);
    assert(dppd_recovery_guard_clean(&guard) == -EUCLEAN);
    assert(dppd_recovery_guard_removed(&guard, second, 0) == 0);
    assert(dppd_recovery_guard_begin(&guard, 0, 9, 7, &first) == 0 && first == second + 1);
    assert(guard.record.attempt_count == 1);
    assert(dppd_recovery_guard_removed(&guard, second, 0) == -ENOENT);
    assert(dppd_recovery_guard_created(&guard, first, 0, DPPD_EVIDENCE_NONE, 0, NULL) == 0);
    for (unsigned int index = 1; index < DPPD_RECOVERY_ATTEMPT_LIMIT; ++index)
        assert(dppd_recovery_guard_begin(&guard, 0, index + 10, 1, &second) == 0);
    assert(dppd_recovery_guard_begin(&guard, 0, 999, 1, &second) == -ENOSPC);
    assert(dppd_recovery_guard_removed(&guard, first, 0) == 0);
    assert(dppd_recovery_guard_begin(&guard, 0, 999, 1, &second) == 0);
    uint64_t saved_id = guard.record.last_attempt;
    guard.record.last_attempt = UINT64_MAX;
    assert(dppd_recovery_guard_begin(&guard, 0, 999, 1, &second) == -EOVERFLOW);
    guard.record.last_attempt = saved_id;
    dppd_recovery_guard_close(&guard);
    assert(dppd_recovery_guard_open(&guard, path, NULL) == 0 && guard.record.attempt_count == 256);
    assert(dppd_recovery_guard_acknowledge(&guard, guard.record.revision) == 0);
    assert(guard.record.attempt_count == 0 && guard.record.last_attempt == saved_id);
    dppd_recovery_guard_close(&guard);
}

int main(void)
{
    char directory[] = "/tmp/dppd-attempt-unit-XXXXXX";
    char path[256], state[256];
    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/recovery", directory);
    snprintf(state, sizeof(state), "%s/state", directory);
    lifecycle(path, state);
    backend_faults(path, state);
    assert(unlink(path) == 0 && rmdir(directory) == 0);
    return 0;
}
