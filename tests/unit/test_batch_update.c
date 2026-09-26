#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "dppd/control.h"
#include "dppd/persistence.h"

/**
 * 测试替换的只有 PMD 调用，规则仓库、事务引擎和软件 QSBR 回收均使用项目真实实现
 * 计数器记录调用顺序，fail_* 指定“第几次调用失败”，零表示不注入对应故障
 * observed 和 expected_revision 用来检查实际安装期间规则账本没有提前发布新版本
 */
static struct {
    unsigned int validates, creates, removes;
    unsigned int fail_create, fail_remove, fail_remove_again;
    bool unsupported;
    uint16_t unsupported_port;
    struct dppd_control_service *observed;
    uint64_t expected_revision;
} fake;

/**
 * 每次进入模拟驱动都核对账本版本，确保全批安装和补偿结束前仍保持原来的版本
 * 若控制层提前发布了某条新规则，这里会直接失败，而不只检查最终是否回滚干净
 */
static void observe(void)
{
    if (fake.observed != NULL)
        assert(dppd_rule_repository_generation(&fake.observed->rules) ==
               fake.expected_revision);
}

/** unsupported 模拟 net_ring 一类不提供 flow 能力的驱动，供 PREFER 软件回退用例使用 */
static int validate(uint16_t port, const struct dppd_rule *rule,
                    struct dppd_flow_error *error)
{
    (void)port;
    (void)rule;
    (void)error;
    observe();
    fake.validates++;
    return fake.unsupported || fake.unsupported_port == port ? -ENOSYS : 0;
}

/**
 * 在指定的创建调用返回 EIO，其他调用只填写模拟 handle，不接触物理网卡
 * 用规则 ID 构造的指针仅充当非空标记，测试不会解引用或释放这段虚拟地址
 */
static int create(uint16_t port, const struct dppd_rule *rule,
                  struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    observe();
    if (++fake.creates == fake.fail_create)
        return -EIO;
    memset(handle, 0, sizeof(*handle));
    handle->rule_id = rule->id;
    handle->rule_generation = rule->generation;
    handle->port_id = port;
    handle->flow = (struct rte_flow *)(uintptr_t)(rule->id + 1U);
    return 0;
}

/**
 * 两个失败位置可以分别模拟原操作失败和补偿失败
 * 删除失败时保留 handle，删除成功才清空它，符合恢复流程需要继续持有对象的约定
 */
static int remove_flow(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    observe();
    if (++fake.removes == fake.fail_remove || fake.removes == fake.fail_remove_again)
        return -EFAULT;
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static const struct dppd_flow_api api = {
    .validate = validate, .create = create, .remove = remove_flow,
};
static struct dppd_topology topology;

/**
 * 为每个用例创建独立服务并安装基线旧规则，避免前一用例的错误计数影响下一用例
 * 构造更新请求时改变优先级并交换安装端口，用来验证替换的是完整规则而不仅是版本号
 * 基线创建完成之后才开启 observe，因为创建基线本身就需要正常推进仓库版本
 */
static void setup(struct dppd_control_service *service,
                  struct dppd_control_batch_update_request requests[4],
                  uint32_t capacity, uint32_t count, bool software)
{
    uint32_t i;

    memset(&fake, 0, sizeof(fake));
    memset(requests, 0, sizeof(*requests) * 4);
    assert(dppd_control_init(service, &topology, capacity, &api) == 0);
    for (i = 0; i < count; ++i) {
        struct dppd_control_apply_result result;
        struct dppd_rule rule = {0};

        rule.id = 100 + i;
        rule.fallback = software ? DPPD_FALLBACK_SOFTWARE_ONLY :
                                   DPPD_FALLBACK_PREFER_HARDWARE;
        rule.nb_matches = 1;
        rule.matches[0].type = DPPD_MATCH_ETH;
        rule.nb_actions = 1;
        rule.actions[0].type = DPPD_ACTION_DROP;
        assert(dppd_control_apply(service, (uint16_t)(5 + i % 2), &rule,
                                  0, &result) == 0);
        requests[i].rule = rule;
        requests[i].rule.priority = 10 + i;
        requests[i].install_port_id = (uint16_t)(6 - i % 2);
        requests[i].expected_generation = result.generation;
    }
    fake.observed = service;
    fake.expected_revision = count;
}

/**
 * 关闭事务过程观察并撤销删除故障，让普通用例能够完整释放测试服务
 * 持续磁盘故障用例不使用本函数，因为它的退出保存本来就应返回错误
 */
static void finish(struct dppd_control_service *service)
{
    fake.observed = NULL;
    fake.fail_remove = fake.fail_remove_again = 0;
    assert(dppd_control_fini(service) == 0);
}

/**
 * 验证失败补偿后的状态与两条基线规则一致，包括规则数、旧版本、优先级和端口
 * 同时检查对象确实存在于原后端，防止仅恢复账本却没有恢复实际安装对象
 */
static void assert_old(struct dppd_control_service *service, bool software)
{
    uint32_t i;

    assert(service->rules.generation == 2 && service->rules.count == 2);
    assert(dppd_rte_flow_backend_count(&service->rte_flow) == (software ? 0U : 2U));
    assert(dppd_software_backend_count(&service->software) == (software ? 2U : 0U));
    for (i = 0; i < 2; ++i) {
        struct dppd_rule stored;

        assert(dppd_rule_repository_get(&service->rules, 100 + i, &stored) == 0);
        assert(stored.generation == i + 1 && stored.priority == 0);
        assert(stored.install_port_id == 5 + i);
        if (software)
            assert(dppd_software_backend_contains_version(&service->software,
                                                           100 + i, i + 1));
        else
            assert(dppd_rte_flow_backend_find_version(&service->rte_flow,
                                                       100 + i, i + 1) != NULL);
    }
}

/**
 * 覆盖两条或四条更新、强制软件执行，以及硬件校验失败后的软件回退
 * 成功后旧版本在两个后端都应消失，所有新版本按请求顺序连续分配并共享事务编号
 */
static void test_success(uint32_t count, bool software, bool fallback)
{
    struct dppd_control_service service;
    struct dppd_control_batch_update_request requests[4];
    struct dppd_control_apply_result results[4];
    uint32_t i;

    setup(&service, requests, count * 2, count, software);
    fake.unsupported = fallback;
    assert(dppd_control_update_batch(&service, requests, count, results) == 0);
    assert(service.rules.generation == count * 2 && service.rules.count == count);
    for (i = 0; i < count; ++i) {
        struct dppd_rule stored;

        assert(results[i].status == DPPD_RULE_UPDATED);
        assert(results[i].generation == count + i + 1);
        assert(results[i].transaction_id != 0);
        assert(results[i].transaction_id == results[0].transaction_id);
        assert(results[i].plan.fallback_used == fallback);
        assert(dppd_rule_repository_get(&service.rules, 100 + i, &stored) == 0);
        assert(stored.priority == 10 + i);
        assert(stored.install_port_id == requests[i].install_port_id);
        assert(dppd_rte_flow_backend_find_version(&service.rte_flow,
                                                   100 + i, i + 1) == NULL);
        assert(!dppd_software_backend_contains_version(&service.software,
                                                        100 + i, i + 1));
        if (software || fallback) {
            assert(results[i].plan.backend == DPPD_PLAN_BACKEND_SOFTWARE);
            assert(dppd_software_backend_contains_version(&service.software,
                                                           100 + i, stored.generation));
        } else {
            const struct dppd_flow_handle *handle = dppd_rte_flow_backend_find_version(
                &service.rte_flow, 100 + i, stored.generation);
            assert(handle != NULL && handle->port_id == requests[i].install_port_id);
        }
    }
    /**
     * 重发旧请求必须报告版本过期，重新读取新版本后再提交相同内容仍是一次完整替换
     * 这与单规则 apply 的内容幂等行为不同，批量接口承诺为整批分配连续新版本
     */
    assert(dppd_control_update_batch(&service, requests, count, results) == -ESTALE);
    for (i = 0; i < count; ++i)
        requests[i].expected_generation = count + i + 1;
    fake.expected_revision = count * 2;
    assert(dppd_control_update_batch(&service, requests, count, results) == 0);
    assert(service.rules.generation == count * 3);
    finish(&service);
}

/**
 * 检查容量不足、条数越界、旧版本错误、重复或不存在的 ID、非法规则和版本溢出
 * 用 validate/remove 调用次数证明这些错误在触碰驱动之前就被拒绝
 */
static void test_preflight(bool software)
{
    struct dppd_control_service service;
    struct dppd_control_batch_update_request requests[4];
    struct dppd_control_apply_result results[4];
    unsigned int validates;

    setup(&service, requests, 2, 2, software);
    validates = fake.validates;
    if (software) {
        assert(dppd_control_update_batch(&service, requests, 2, results) == 0);
        assert(service.rules.generation == 4);
        assert(dppd_software_backend_count(&service.software) == 2);
    } else {
        assert(dppd_control_update_batch(&service, requests, 2, results) == -ENOSPC);
        assert_old(&service, software);
    }
    assert(fake.validates == validates && fake.removes == 0);
    finish(&service);

    setup(&service, requests, 4, 2, software);
    validates = fake.validates;
    assert(dppd_control_update_batch(&service, requests, 1, results) == -EINVAL);
    assert(dppd_control_update_batch(&service, requests, 5, results) == -EINVAL);
    requests[1].expected_generation = 0;
    assert(dppd_control_update_batch(&service, requests, 2, results) == -ESTALE);
    requests[1].expected_generation = DPPD_RULE_GENERATION_ANY;
    assert(dppd_control_update_batch(&service, requests, 2, results) == -ESTALE);
    requests[1].expected_generation = 1;
    assert(dppd_control_update_batch(&service, requests, 2, results) == -ESTALE);
    requests[1].expected_generation = 2;
    requests[1].rule.id = 100;
    assert(dppd_control_update_batch(&service, requests, 2, results) == -EEXIST);
    requests[1].rule.id = 999;
    assert(dppd_control_update_batch(&service, requests, 2, results) == -ENOENT);
    requests[1].rule.id = 101;
    requests[1].rule.nb_actions = 0;
    assert(dppd_control_update_batch(&service, requests, 2, results) == -EINVAL);
    requests[1].rule.nb_actions = 1;
    requests[1].rule.nb_matches = DPPD_RULE_MAX_ITEMS + 1;
    assert(dppd_control_update_batch(&service, requests, 2, results) == -EINVAL);
    requests[1].rule.nb_matches = 1;
    requests[1].install_port_id = 99;
    assert(dppd_control_update_batch(&service, requests, 2, results) == -ENOENT);
    requests[1].install_port_id = 5;
    service.rules.generation = UINT64_MAX - 2;
    assert(dppd_control_update_batch(&service, requests, 2, results) == -EOVERFLOW);
    service.rules.generation = 2;
    assert(fake.validates == validates && fake.removes == 0);
    assert_old(&service, software);
    finish(&service);
}

/**
 * 按实际执行顺序在创建、删除和补偿阶段注入错误
 * 可完整补偿的故障必须保留全部旧规则，补偿也失败的故障必须进入隔离并拒绝继续写入
 * 后半部分还检查 PREFER 的回退限制，以及硬件与软件混合时能否恢复原后端
 */
static void test_failures(void)
{
    struct dppd_control_service service;
    struct dppd_control_batch_update_request requests[4];
    struct dppd_control_apply_result results[4];

    setup(&service, requests, 4, 2, false);
    fake.fail_create = fake.creates + 2;
    assert(dppd_control_update_batch(&service, requests, 2, results) == -EIO);
    assert(fake.removes == 1); /** 仅撤销第一条新版本，没有删除旧版本 */
    assert_old(&service, false);
    finish(&service);

    setup(&service, requests, 4, 2, false);
    fake.fail_remove = 2;
    assert(dppd_control_update_batch(&service, requests, 2, results) == -EFAULT);
    assert(fake.creates == 5 && fake.removes == 4);
    assert_old(&service, false);
    assert(service.recovery_state == DPPD_CONTROL_RECOVERY_READY);
    finish(&service);

    /** 三种组合分别覆盖创建后撤新失败、删除后恢复失败、删除后撤新失败，均应封锁写入 */
    for (unsigned int mode = 0; mode < 3; ++mode) {
        setup(&service, requests, 4, 2, false);
        if (mode == 0) {
            fake.fail_create = 4;
            fake.fail_remove = 1;
        } else {
            fake.fail_remove = 2;
            if (mode == 1)
                fake.fail_create = 5;
            else
                fake.fail_remove_again = 3;
        }
        assert(dppd_control_update_batch(&service, requests, 2, results) == -EUCLEAN);
        assert(service.rules.generation == 2);
        assert(service.recovery_state == DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED);
        assert(dppd_control_update_batch(&service, requests, 2, results) == -EUCLEAN);
        fake.fail_remove = fake.fail_remove_again = 0;
        assert(dppd_control_reconciliation_retry(&service) == 0);
        assert(service.recovery_state == DPPD_CONTROL_RECOVERY_RESTART_REQUIRED);
        finish(&service);
    }

    /** PREFER 只能在校验阶段回退；REQUIRE 和无法表达相同语义的 QUEUE 必须继续报告失败 */
    setup(&service, requests, 4, 2, false);
    fake.unsupported = true;
    requests[1].rule.fallback = DPPD_FALLBACK_REQUIRE_HARDWARE;
    assert(dppd_control_update_batch(&service, requests, 2, results) == -ENOSYS);
    requests[1].rule.fallback = DPPD_FALLBACK_PREFER_HARDWARE;
    requests[1].rule.actions[0].type = DPPD_ACTION_QUEUE;
    assert(dppd_control_update_batch(&service, requests, 2, results) == -ENOSYS);
    assert_old(&service, false);
    finish(&service);

    /** 新规则已经装入软件后端，但旧硬件删除失败，必须撤销新软件规则并恢复原硬件规则 */
    setup(&service, requests, 4, 2, false);
    requests[0].rule.fallback = requests[1].rule.fallback = DPPD_FALLBACK_SOFTWARE_ONLY;
    fake.fail_remove = 2;
    assert(dppd_control_update_batch(&service, requests, 2, results) == -EFAULT);
    assert_old(&service, false);
    finish(&service);

    /** 第一条旧规则在软件、第二条在硬件，补偿时必须按真实归属恢复，不能按策略字段猜测 */
    setup(&service, requests, 4, 2, true);
    fake.observed = NULL;
    requests[1].rule.fallback = DPPD_FALLBACK_REQUIRE_HARDWARE;
    assert(dppd_control_apply(&service, 5, &requests[1].rule, 2, &results[0]) == 0);
    requests[1].expected_generation = results[0].generation;
    requests[1].rule.priority++;
    fake.observed = &service;
    fake.expected_revision = 3;
    fake.fail_remove = fake.removes + 1;
    assert(dppd_control_update_batch(&service, requests, 2, results) == -EFAULT);
    assert(service.rules.generation == 3);
    assert(dppd_software_backend_contains_version(&service.software, 100, 1));
    assert(dppd_software_backend_count(&service.software) == 1);
    assert(dppd_rte_flow_backend_find_version(&service.rte_flow, 101, 3) != NULL);
    assert(dppd_rte_flow_backend_count(&service.rte_flow) == 1);
    finish(&service);
}

/**
 * 先验证整批成功后写入磁盘的是完整新版本，再验证持续保存失败时的 dirty 行为
 * 磁盘失败不同于安装失败，内存中的整批新规则可能已经生效，不能要求回到旧版本
 */
static void test_persistence(bool software)
{
    struct dppd_control_service service;
    struct dppd_control_batch_update_request requests[4];
    struct dppd_control_apply_result results[4];
    struct dppd_persisted_snapshot snapshot;
    char path[128];

    snprintf(path, sizeof(path), "/tmp/dppd-batch-update-%ld.bin", (long)getpid());
    setup(&service, requests, 4, 2, software);
    assert(dppd_control_persistence_attach(&service, path) == 0);
    assert(dppd_control_update_batch(&service, requests, 2, results) == 0);
    assert(dppd_persistence_load(path, &snapshot) == 0);
    assert(snapshot.count == 2 && snapshot.repository_generation == 4);
    assert(snapshot.rules[0].generation == 3 && snapshot.rules[1].generation == 4);
    dppd_persisted_snapshot_destroy(&snapshot);
    finish(&service);
    assert(unlink(path) == 0);

    /** /dev/null 是文件而非目录，用它作父路径可稳定触发 ENOTDIR，不受运行者权限影响 */
    setup(&service, requests, 4, 2, software);
    assert(dppd_control_persistence_attach(&service, "/dev/null/rules.bin") == 0);
    service.persistence_dirty = false; /** 从 clean 状态开始，模拟下一次更新完成后才发生保存故障 */
    assert(dppd_control_update_batch(&service, requests, 2, results) == -EUCLEAN);
    assert(service.persistence_dirty && service.rules.generation == 4);
    assert(service.recovery_state == DPPD_CONTROL_RECOVERY_READY);
    assert(dppd_control_update_batch(&service, requests, 2, results) == -EUCLEAN);
    /**
     * 退出会重试保存 dirty 快照，磁盘故障仍在时应返回底层 ENOTDIR
     * 即使报告保存失败，也必须释放仓库、路径和后端对象，不能把错误处理变成内存泄漏
     */
    fake.observed = NULL;
    assert(dppd_control_fini(&service) == -ENOTDIR);
    assert(service.rules.records == NULL && service.persistence_path == NULL);
    assert(service.rte_flow.objects == NULL && !service.software.lock_initialized);
}

/** 两个模拟端口足以检查跨端口更新，依次运行成功、预检拒绝、故障补偿和持久化用例 */
static void test_mixed_capacity(void)
{
    struct dppd_control_service service;
    struct dppd_control_batch_update_request requests[4];
    struct dppd_control_apply_result results[4];
    uint32_t i;

    setup(&service, requests, 4, 4, true);
    for (i = 0; i < 4; ++i)
        requests[i].rule.fallback = DPPD_FALLBACK_PREFER_HARDWARE;
    fake.unsupported_port = 6;
    assert(dppd_control_update_batch(&service, requests, 4, results) == -ENOSPC);
    assert(fake.validates == 4 && fake.creates == 0 && fake.removes == 0);
    assert(service.rules.generation == 4);
    for (i = 0; i < 4; ++i)
        assert(dppd_software_backend_contains_version(&service.software, 100 + i, i + 1));
    finish(&service);
}

int main(void)
{
    topology.nb_endpoints = 2;
    topology.endpoints[0].ethdev_port_id = 5;
    topology.endpoints[1].ethdev_port_id = 6;
    test_success(2, false, false);
    test_success(4, false, false);
    test_success(2, true, false);
    test_success(4, true, false);
    test_success(2, false, true);
    test_preflight(false);
    test_preflight(true);
    test_failures();
    test_persistence(false);
    test_persistence(true);
    test_mixed_capacity();
    return 0;
}
