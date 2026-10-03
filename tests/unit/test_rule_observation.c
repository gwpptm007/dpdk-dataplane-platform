#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dppd/management.h"

/** 错误只由本测试的驱动接口注入，不需要网卡，也不改变生产进程的调用方式 */
static struct {
    unsigned int validates, creates, removes;
    int validate_error, fail_create, fail_remove;
    bool partial_create;
} driver;
static unsigned int fail_allocation;

void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size);

/** 让指定一次本地分配失败，用来验证失败的整批软件发布仍保留旧账本 */
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_allocation != 0 && --fail_allocation == 0)
        return NULL;
    return __real_calloc(count, size);
}

/** 校验错误可独立于创建错误配置，便于区分支持能力失败和安装副作用失败 */
static int validate(uint16_t port, const struct dppd_rule *rule, struct dppd_flow_error *error)
{
    (void)port; (void)rule; (void)error;
    driver.validates++;
    return driver.validate_error;
}

/** 部分创建失败仍可以留下 handle，上层必须清理，不能把错误当成没有安装过对象 */
static int create(uint16_t port, const struct dppd_rule *rule,
    struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    bool fail = (int)driver.creates++ == driver.fail_create;

    (void)error;
    memset(handle, 0, sizeof(*handle));
    if (!fail || driver.partial_create) {
        handle->rule_id = rule->id;
        handle->rule_generation = rule->generation;
        handle->port_id = port;
        handle->flow = (struct rte_flow *)(uintptr_t)(rule->id + 1U);
    }
    return fail ? -EIO : 0;
}

/** 删除失败保留原 handle，之后的恢复重试才能再次定位同一个实际对象 */
static int remove_flow(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    (void)error;
    if ((int)driver.removes++ == driver.fail_remove)
        return -EFAULT;
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static const struct dppd_flow_api api = {
    .validate = validate, .create = create, .remove = remove_flow,
};
static const struct dppd_topology topology = {
    .nb_endpoints = 2, .endpoints = {{.ethdev_port_id = 5}, {.ethdev_port_id = 6}},
};

/** 每个场景用独立 service，避免前一次故障或累计值掩盖本次断言 */
static void setup(struct dppd_control_service *control, uint32_t capacity)
{
    memset(&driver, 0, sizeof(driver));
    driver.fail_create = driver.fail_remove = -1;
    assert(dppd_control_init(control, &topology, capacity, &api) == 0);
}

/** 构造最小合法规则，只通过参数改变后端策略 */
static struct dppd_rule rule(uint64_t id, enum dppd_fallback_policy policy)
{
    struct dppd_rule value = {.id = id, .fallback = policy, .nb_matches = 1, .nb_actions = 1};

    value.matches[0].type = DPPD_MATCH_ETH;
    value.actions[0].type = DPPD_ACTION_DROP;
    return value;
}

/** 错误分类依赖原始 errno，保存与隔离阶段优先表达业务原因 */
static void classify_errors(void)
{
    const struct {int code; enum dppd_rule_failure_kind kind;} cases[] = {
        {0, DPPD_RULE_FAILURE_NONE}, {-EINVAL, DPPD_RULE_FAILURE_INPUT},
        {-ESTALE, DPPD_RULE_FAILURE_CONFLICT}, {-ENOENT, DPPD_RULE_FAILURE_NOT_FOUND},
        {-ENOTSUP, DPPD_RULE_FAILURE_UNSUPPORTED}, {-ENOMEM, DPPD_RULE_FAILURE_RESOURCE},
        {-EFAULT, DPPD_RULE_FAILURE_DEVICE}, {-EAGAIN, DPPD_RULE_FAILURE_TEMPORARY},
        {-EACCES, DPPD_RULE_FAILURE_PERMISSION}, {-EUCLEAN, DPPD_RULE_FAILURE_INCONSISTENT},
        {-EPROTO, DPPD_RULE_FAILURE_INTERNAL},
    };
    struct dppd_rule_observation observation = {0};

    for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        assert(dppd_rule_failure_classify(DPPD_RULE_STAGE_COMMIT, cases[i].code) == cases[i].kind);
    assert(dppd_rule_failure_classify(DPPD_RULE_STAGE_PERSIST, -EIO) == DPPD_RULE_FAILURE_PERSISTENCE);
    assert(dppd_rule_failure_classify(DPPD_RULE_STAGE_ISOLATION, -EUCLEAN) == DPPD_RULE_FAILURE_ISOLATED);
    dppd_rule_observation_begin(&observation, DPPD_RULE_OPERATION_APPLY);
    dppd_rule_observation_compensation(&observation, -EFAULT, 7);
    dppd_rule_observation_fault(&observation, -EIO);
    dppd_rule_observation_finish(&observation, -EUCLEAN);
    dppd_rule_observation_finish(&observation, -EIO);
    assert(observation.metrics.operations == 1 && observation.metrics.compensation_failures == 1);
    assert(observation.metrics.last.cause_code == -EIO && observation.metrics.last.compensation_code == -EFAULT);
}

/** 成功回退计入安装数量，幂等重放和读取保留最后一次失败，过期版本不接触驱动 */
static void success_and_preflight(void)
{
    struct dppd_control_service control;
    struct dppd_control_apply_result result;
    struct dppd_rule value = rule(10, DPPD_FALLBACK_REQUIRE_HARDWARE);
    struct dppd_rule_metrics metrics, copied;
    unsigned int validates;

    setup(&control, 1);
    driver.validate_error = -ENOTSUP;
    assert(dppd_control_apply(&control, 5, &value, 0, &result) == -ENOTSUP);
    metrics = control.observation.metrics;
    assert(metrics.last.stage == DPPD_RULE_STAGE_VALIDATE && metrics.last.rule_id == 10);
    assert(metrics.last.backend_known && metrics.last.backend == DPPD_PLAN_BACKEND_RTE_FLOW);
    assert(metrics.last.generation == 1 && metrics.last.transaction_id != 0);
    value.fallback = DPPD_FALLBACK_PREFER_HARDWARE;
    assert(dppd_control_apply(&control, 5, &value, 0, &result) == 0);
    assert(dppd_control_apply(&control, 5, &value, 0, &result) == 0);
    metrics = control.observation.metrics;
    assert(metrics.operations == 3 && metrics.succeeded == 2 && metrics.failed == 1);
    assert(metrics.applied == 1 && metrics.unchanged == 1 && metrics.fallback_rules == 1);
    assert(metrics.last.sequence == 1 && metrics.last.kind == DPPD_RULE_FAILURE_UNSUPPORTED);
    validates = driver.validates;
    value.priority = 9;
    assert(dppd_control_apply(&control, 5, &value, 7, &result) == -ESTALE);
    assert(control.observation.metrics.last.stage == DPPD_RULE_STAGE_PREFLIGHT);
    assert(control.observation.metrics.last.generation == 7 && driver.validates == validates);
    value.id = 11;
    assert(dppd_control_apply(&control, 5, &value, 0, &result) == -ENOSPC);
    assert(control.observation.metrics.last.kind == DPPD_RULE_FAILURE_RESOURCE);
    assert(driver.validates == validates);
    dppd_control_rule_metrics(&control, &metrics);
    fail_allocation = 1;
    for (unsigned int i = 0; i < 10; ++i) {
        dppd_control_rule_metrics(&control, &copied);
        assert(memcmp(&copied, &metrics, sizeof(metrics)) == 0);
    }
    assert(fail_allocation == 1);
    fail_allocation = 0;
    assert(dppd_control_fini(&control) == 0);
}

/** 单规则部分创建失败且撤销失败必须隔离，管理查询允许读取原始错误，普通写入被拦截 */
static void partial_create_isolation(void)
{
    struct dppd_control_service control;
    struct dppd_control_apply_result result;
    struct dppd_rule value = rule(20, DPPD_FALLBACK_REQUIRE_HARDWARE);
    struct dppd_management_request request = {.version = DPPD_MANAGEMENT_VERSION,
        .size = sizeof(request), .operation = DPPD_MANAGEMENT_RULE_METRICS};
    struct dppd_management_response response;
    struct dppd_rule_metrics metrics;

    setup(&control, 2);
    driver.fail_create = driver.fail_remove = 0;
    driver.partial_create = true;
    assert(dppd_control_apply(&control, 6, &value, 0, &result) == -EUCLEAN);
    metrics = control.observation.metrics;
    assert(metrics.failed == 1 && metrics.compensation_failures == 1 && metrics.applied == 0);
    assert(metrics.last.stage == DPPD_RULE_STAGE_COMMIT && metrics.last.cause_code == -EIO);
    assert(metrics.last.response_code == -EUCLEAN && metrics.last.compensation_code == -EFAULT);
    assert(metrics.last.rule_id == 20 && metrics.last.compensation_rule_id == 20);
    assert(control.recovery_state == DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED);
    assert(dppd_rte_flow_backend_count(&control.rte_flow) == 1 && control.rules.count == 0);
    assert(dppd_management_handle(&control, NULL, NULL, &request, &response) == 0);
    assert(response.status == 0 && memcmp(&response.payload.rule_metrics, &metrics, sizeof(metrics)) == 0);
    request.version = 12;
    assert(dppd_management_handle(&control, NULL, NULL, &request, &response) == 0);
    assert(response.status == -EPROTO && control.observation.metrics.operations == 1);
    request.version = DPPD_MANAGEMENT_VERSION;
    request.operation = DPPD_MANAGEMENT_RULE_APPLY;
    assert(dppd_management_handle(&control, NULL, NULL, &request, &response) == 0);
    assert(response.status == -EUCLEAN && control.observation.metrics.operations == 1);
    assert(dppd_control_apply(&control, 6, &value, 0, &result) == -EUCLEAN);
    assert(control.observation.metrics.last.kind == DPPD_RULE_FAILURE_ISOLATED);
    driver.fail_remove = (int)driver.removes;
    assert(dppd_control_reconciliation_retry(&control) == -EUCLEAN);
    assert(control.observation.metrics.last.stage == DPPD_RULE_STAGE_RECONCILE);
    assert(control.observation.metrics.last.cause_code == -EFAULT);
    metrics = control.observation.metrics;
    assert(dppd_control_reconciliation_retry(&control) == 0);
    assert(control.recovery_state == DPPD_CONTROL_RECOVERY_RESTART_REQUIRED);
    assert(memcmp(&metrics.last, &control.observation.metrics.last, sizeof(metrics.last)) == 0);
    assert(dppd_control_fini(&control) == 0);
}

/** 整批第二条创建失败定位第二条，第一条撤销失败单独记录，不能多算内部回滚请求 */
static void batch_compensation(void)
{
    struct dppd_control_service control;
    struct dppd_control_batch_create_request requests[2] = {0};
    struct dppd_control_apply_result results[2];
    struct dppd_rule_failure_event last;

    setup(&control, 4);
    for (unsigned int i = 0; i < 2; ++i) {
        requests[i].rule = rule(30 + i, DPPD_FALLBACK_REQUIRE_HARDWARE);
        requests[i].install_port_id = 5;
    }
    driver.fail_create = 1;
    driver.fail_remove = 0;
    assert(dppd_control_create_batch(&control, requests, 2, results) == -EUCLEAN);
    last = control.observation.metrics.last;
    assert(last.rule_id == 31 && last.generation == 2 && last.rule_count == 2);
    assert(last.stage == DPPD_RULE_STAGE_COMMIT && last.cause_code == -EIO);
    assert(last.compensation_rule_id == 30 && last.compensation_code == -EFAULT);
    assert(control.observation.metrics.operations == 1 && control.observation.metrics.failed == 1);
    assert(dppd_control_reconciliation_retry(&control) == 0);
    assert(dppd_control_fini(&control) == 0);

    /** 删除第二条失败后重建第一条也失败，仍须保留删除错误和重建错误各自的身份 */
    setup(&control, 4);
    assert(dppd_control_create_batch(&control, requests, 2, results) == 0);
    struct dppd_control_batch_remove_request deletes[2] = {
        {.rule_id = 30, .expected_generation = 1}, {.rule_id = 31, .expected_generation = 2}};
    struct dppd_control_batch_remove_result removed[2];
    driver.fail_remove = 1;
    driver.fail_create = 2;
    assert(dppd_control_remove_batch(&control, deletes, 2, removed) == -EUCLEAN);
    last = control.observation.metrics.last;
    assert(last.stage == DPPD_RULE_STAGE_REMOVE && last.rule_id == 31 && last.cause_code == -EFAULT);
    assert(last.port_known && last.install_port_id == 5 && last.backend_known);
    assert(last.compensation_rule_id == 30 && last.compensation_code == -EIO);
    assert(control.observation.metrics.operations == 2 && control.observation.metrics.failed == 1);
    assert(dppd_control_reconciliation_retry(&control) == 0);
    assert(dppd_control_fini(&control) == 0);
}

/** 保存失败发生在规则完整发布以后，API 的 EUCLEAN 与文件系统的原始错误都必须可见 */
static void persistence_failure(void)
{
    struct dppd_control_service control;
    struct dppd_control_apply_result result;
    struct dppd_rule value = rule(40, DPPD_FALLBACK_SOFTWARE_ONLY);
    struct dppd_rule_failure_event last;
    char path[128];
    FILE *file;

    setup(&control, 4);
    snprintf(path, sizeof(path), "/tmp/dppd-observation-%ld", (long)getpid());
    file = fopen(path, "w");
    assert(file != NULL && fclose(file) == 0);
    /** 空文件不是合法快照，加载错误归于存储阶段，不能被误报成驱动创建失败 */
    assert(dppd_control_persistence_restore(&control, path) != 0);
    assert(control.observation.metrics.last.stage == DPPD_RULE_STAGE_LOAD);
    assert(control.observation.metrics.last.kind == DPPD_RULE_FAILURE_PERSISTENCE);
    char state[160];
    snprintf(state, sizeof(state), "%s/state.bin", path);
    assert(dppd_control_persistence_attach(&control, state) == 0);
    assert(dppd_control_apply(&control, 5, &value, 0, &result) == -EUCLEAN);
    last = control.observation.metrics.last;
    assert(last.stage == DPPD_RULE_STAGE_PERSIST && last.kind == DPPD_RULE_FAILURE_PERSISTENCE);
    assert(last.cause_code == -ENOTDIR && last.response_code == -EUCLEAN && last.applied);
    assert(control.observation.metrics.failed_after_apply == 1 && control.observation.metrics.applied == 1);
    value.id = 41;
    assert(dppd_control_apply(&control, 5, &value, 0, &result) == -EUCLEAN);
    assert(!control.observation.metrics.last.applied && control.rules.count == 1);
    assert(dppd_control_persistence_flush(&control) == -ENOTDIR);
    last = control.observation.metrics.last;
    assert(last.operation == DPPD_RULE_OPERATION_FLUSH && last.response_code == -ENOTDIR);
    assert(unlink(path) == 0 && mkdir(path, 0700) == 0);
    assert(dppd_control_persistence_flush(&control) == 0);
    assert(memcmp(&last, &control.observation.metrics.last, sizeof(last)) == 0);
    assert(!control.persistence_dirty && control.persisted_generation == 1);
    assert(dppd_control_fini(&control) == 0);
    assert(unlink(state) == 0 && rmdir(path) == 0);
}

/** 软件整批发布的内存失败报告整个批次，不错误归因到最后一条规划完成的成员 */
static void software_publication_failure(void)
{
    struct dppd_control_service control;
    struct dppd_control_apply_result results[2];
    struct dppd_control_batch_update_request requests[2] = {0};

    setup(&control, 2);
    for (unsigned int i = 0; i < 2; ++i) {
        requests[i].rule = rule(50 + i, DPPD_FALLBACK_SOFTWARE_ONLY);
        requests[i].install_port_id = 5;
        assert(dppd_control_apply(&control, 5, &requests[i].rule, 0, &results[i]) == 0);
        requests[i].expected_generation = results[i].generation;
        requests[i].rule.priority = 9;
    }
    fail_allocation = 1;
    assert(dppd_control_update_batch(&control, requests, 2, results) == -ENOMEM);
    assert(fail_allocation == 0 && control.rules.generation == 2);
    assert(control.observation.metrics.last.rule_id == 0 && control.observation.metrics.last.rule_count == 2);
    assert(control.observation.metrics.last.stage == DPPD_RULE_STAGE_COMMIT);
    assert(control.observation.metrics.last.backend == DPPD_PLAN_BACKEND_SOFTWARE);
    assert(dppd_control_update_batch(&control, requests, 2, results) == 0);
    assert(control.rules.generation == 4 && control.observation.metrics.operations == 4);
    assert(dppd_control_fini(&control) == 0);
}

/** 覆盖真实控制路径的诊断契约，不依赖为实现凑数的字段赋值测试 */
int main(void)
{
    classify_errors();
    success_and_preflight();
    partial_create_isolation();
    batch_compensation();
    persistence_failure();
    software_publication_failure();
    return 0;
}
