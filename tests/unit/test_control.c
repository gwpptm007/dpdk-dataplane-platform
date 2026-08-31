#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dppd/control.h"
#include "dppd/persistence.h"

struct fake_state {
    int validate_calls;
    int create_calls;
    int remove_calls;
    bool fail_validate;
    int fail_create_at;
    int fail_remove_at;
    int query_calls;
    uint16_t create_ports[16];
};

static struct fake_state fake;

static int fake_validate(uint16_t port_id, const struct dppd_rule *rule,
                         struct dppd_flow_error *error)
{
    (void)port_id;
    (void)rule;
    (void)error;
    fake.validate_calls++;
    return fake.fail_validate ? -ENOTSUP : 0;
}

static int fake_create(uint16_t port_id, const struct dppd_rule *rule,
                       struct dppd_flow_handle *handle,
                       struct dppd_flow_error *error)
{
    int call = fake.create_calls++;

    (void)error;
    memset(handle, 0, sizeof(*handle));
    if (call >= 0 && call < (int)(sizeof(fake.create_ports) /
                                  sizeof(fake.create_ports[0])))
        fake.create_ports[call] = port_id;
    if (call == fake.fail_create_at)
        return -EIO;
    handle->rule_id = rule->id;
    handle->rule_generation = rule->generation;
    handle->port_id = port_id;
    handle->flow = (struct rte_flow *)(uintptr_t)(rule->id + 1U);
    for (uint16_t i = 0; i < rule->nb_actions; ++i) {
        if (rule->actions[i].type == DPPD_ACTION_COUNT)
            handle->has_count = true;
    }
    return 0;
}

static int fake_remove(struct dppd_flow_handle *handle,
                       struct dppd_flow_error *error)
{
    int call = fake.remove_calls++;

    (void)error;
    if (call == fake.fail_remove_at)
        return -EFAULT;
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static int fake_query_count(const struct dppd_flow_handle *handle,
                            uint64_t *hits,
                            uint64_t *bytes,
                            struct dppd_flow_error *error)
{
    (void)handle;
    (void)error;
    fake.query_calls++;
    *hits = 123;
    *bytes = 4567;
    return 0;
}

static struct dppd_rule make_rule(uint64_t id)
{
    struct dppd_rule rule;

    memset(&rule, 0, sizeof(rule));
    rule.id = id;
    rule.domain = DPPD_RULE_DOMAIN_INGRESS;
    rule.fallback = DPPD_FALLBACK_PREFER_HARDWARE;
    rule.nb_matches = 1;
    rule.matches[0].type = DPPD_MATCH_ETH;
    rule.nb_actions = 1;
    rule.actions[0].type = DPPD_ACTION_DROP;
    return rule;
}

int main(void)
{
    const struct dppd_flow_api api = {
        .validate = fake_validate,
        .create = fake_create,
        .remove = fake_remove,
        .query_count = fake_query_count,
    };
    struct dppd_topology topology;
    struct dppd_control_service service;
    struct dppd_control_apply_result result;
    struct dppd_control_apply_result batch_results[2];
    struct dppd_control_batch_create_request batch_requests[2];
    struct dppd_control_batch_remove_request batch_remove_requests[2];
    struct dppd_control_batch_remove_result batch_remove_results[2];
    struct dppd_control_count_result count_result;
    struct dppd_control_persistence_status persistence_status;
    struct dppd_control_recovery_status recovery_status;
    struct dppd_persisted_snapshot snapshot;
    struct dppd_rule_repository source_repository;
    struct dppd_rule_apply_result repository_result;
    struct dppd_rule restored;
    struct dppd_rule rule;
    char state_directory[128];
    char state_path[160];
    char moved_directory[128];
    char moved_path[160];
    uint64_t generation;
    bool removed;

    memset(&fake, 0, sizeof(fake));
    fake.fail_create_at = -1;
    fake.fail_remove_at = -1;
    memset(&topology, 0, sizeof(topology));
    topology.nb_endpoints = 2;
    topology.endpoints[0].ethdev_port_id = 5;
    topology.endpoints[1].ethdev_port_id = 6;
    assert(dppd_control_init(&service, &topology, 4, &api) == 0);

    rule = make_rule(1000);
    assert(dppd_control_apply(&service, 5, &rule, 0, &result) == 0);
    assert(result.status == DPPD_RULE_CREATED && result.generation == 1);
    assert(result.transaction_id == 1);
    assert(fake.validate_calls == 1 && fake.create_calls == 1);
    assert(dppd_rule_repository_count(&service.rules) == 1);
    assert(dppd_rte_flow_backend_count(&service.rte_flow) == 1);
    assert(dppd_control_query_count(&service, 1000, 1,
                                    &count_result) == -ENODATA);

    assert(dppd_control_apply(&service, 5, &rule, 0, &result) == 0);
    assert(result.status == DPPD_RULE_UNCHANGED && result.generation == 1);
    assert(result.transaction_id == 0);
    assert(fake.validate_calls == 1 && fake.create_calls == 1);

    rule.priority = 10;
    assert(dppd_control_apply(&service, 5, &rule, 0, &result) == -ESTALE);
    assert(dppd_control_apply(&service, 5, &rule, 1, &result) == 0);
    assert(result.status == DPPD_RULE_UPDATED && result.generation == 2);
    assert(dppd_rule_repository_generation(&service.rules) == 2);
    assert(dppd_rte_flow_backend_count(&service.rte_flow) == 1);
    assert(dppd_rte_flow_backend_find_version(&service.rte_flow, 1000, 1) == NULL);
    assert(dppd_rte_flow_backend_find_version(&service.rte_flow, 1000, 2) != NULL);

    rule.priority = 20;
    fake.fail_create_at = fake.create_calls;
    assert(dppd_control_apply(&service, 5, &rule, 2, &result) == -EIO);
    assert(dppd_rule_repository_generation(&service.rules) == 2);
    assert(dppd_rte_flow_backend_count(&service.rte_flow) == 1);
    fake.fail_create_at = -1;

    fake.fail_remove_at = fake.remove_calls;
    assert(dppd_control_apply(&service, 5, &rule, 2, &result) == -EFAULT);
    assert(dppd_rule_repository_generation(&service.rules) == 2);
    assert(dppd_rte_flow_backend_count(&service.rte_flow) == 1);
    assert(dppd_rte_flow_backend_find_version(&service.rte_flow, 1000, 2) != NULL);
    fake.fail_remove_at = -1;

    assert(dppd_control_apply(&service, 5, &rule, 2, &result) == 0);
    assert(result.status == DPPD_RULE_UPDATED && result.generation == 3);
    assert(dppd_rte_flow_backend_count(&service.rte_flow) == 1);

    assert(dppd_control_remove(&service, 1000, 2, &removed,
                               &generation) == -ESTALE);
    assert(dppd_control_remove(&service, 1000, 3, &removed,
                               &generation) == 0);
    assert(removed && generation == 4);
    assert(dppd_control_remove(&service, 1000, 0, &removed,
                               &generation) == 0);
    assert(!removed && generation == 4);

    /*
     * 同批规则共享一个 backend transaction，成功后才连续发布两个 desired generation。
     * 两条规则刻意安装到不同端口，证明批量原子性不依赖“同端口”这个 dppctl 演示层限制，
     * 而由 control 层的统一事务和 repository 发布顺序保证。
     */
    memset(batch_requests, 0, sizeof(batch_requests));
    batch_requests[0].install_port_id = 5;
    batch_requests[0].rule = make_rule(1100);
    batch_requests[1].install_port_id = 6;
    batch_requests[1].rule = make_rule(1101);
    assert(dppd_control_create_batch(&service, batch_requests, 2,
                                     batch_results) == 0);
    assert(batch_results[0].status == DPPD_RULE_CREATED &&
           batch_results[0].generation == 5);
    assert(batch_results[1].status == DPPD_RULE_CREATED &&
           batch_results[1].generation == 6);
    assert(batch_results[0].transaction_id == batch_results[1].transaction_id);
    assert(dppd_rule_repository_count(&service.rules) == 2);
    assert(dppd_rte_flow_backend_count(&service.rte_flow) == 2);

    /*
     * 删除也要求整批精确 generation：成功后两个 actual object 都消失，repository
     * generation 按输入顺序推进。结果不复用单条 delete 的“缺失即成功”语义。
     */
    memset(batch_remove_requests, 0, sizeof(batch_remove_requests));
    batch_remove_requests[0].rule_id = 1100;
    batch_remove_requests[0].expected_generation = 5;
    batch_remove_requests[1].rule_id = 1101;
    batch_remove_requests[1].expected_generation = 6;
    assert(dppd_control_remove_batch(&service, batch_remove_requests, 2,
                                     batch_remove_results) == 0);
    assert(batch_remove_results[0].generation == 7 &&
           batch_remove_results[1].generation == 8);
    assert(dppd_rule_repository_count(&service.rules) == 0);
    assert(dppd_rte_flow_backend_count(&service.rte_flow) == 0);

    /*
     * 第二条 create 失败时，第一条已创建的 actual object 必须被 transaction 逆序删除。
     * 同时断言 desired repository 仍为空，避免出现“查询能看到规则、数据面实际没有规则”
     * 的双写不一致；这正是批量接口不允许逐条 apply 伪装成原子操作的原因。
     */
    batch_requests[0].rule = make_rule(1102);
    batch_requests[1].rule = make_rule(1103);
    fake.fail_create_at = fake.create_calls + 1;
    assert(dppd_control_create_batch(&service, batch_requests, 2,
                                     batch_results) == -EIO);
    assert(dppd_rule_repository_count(&service.rules) == 0);
    assert(dppd_rte_flow_backend_count(&service.rte_flow) == 0);
    fake.fail_create_at = -1;

    /*
     * 第二条 actual remove 失败时，第一条必须由补偿创建恢复；desired 未被触碰，
     * 因而清除故障后可用同一组 generation 安全重试整批删除。
     */
    batch_requests[0].rule = make_rule(1200);
    batch_requests[1].rule = make_rule(1201);
    assert(dppd_control_create_batch(&service, batch_requests, 2,
                                     batch_results) == 0);
    batch_remove_requests[0].rule_id = 1200;
    batch_remove_requests[0].expected_generation = batch_results[0].generation;
    batch_remove_requests[1].rule_id = 1201;
    batch_remove_requests[1].expected_generation = batch_results[1].generation;
    fake.fail_remove_at = fake.remove_calls + 1;
    assert(dppd_control_remove_batch(&service, batch_remove_requests, 2,
                                     batch_remove_results) == -EFAULT);
    assert(dppd_rule_repository_count(&service.rules) == 2);
    assert(dppd_rte_flow_backend_count(&service.rte_flow) == 2);
    fake.fail_remove_at = -1;
    assert(dppd_control_remove_batch(&service, batch_remove_requests, 2,
                                     batch_remove_results) == 0);
    assert(dppd_rule_repository_count(&service.rules) == 0);

    fake.fail_validate = true;
    rule = make_rule(1001);
    assert(dppd_control_apply(&service, 5, &rule, 0, &result) == 0);
    assert(result.plan.backend == DPPD_PLAN_BACKEND_SOFTWARE);
    assert(result.plan.fallback_used);
    assert(dppd_software_backend_contains_version(&service.software, 1001,
                                                   result.generation));
    assert(dppd_control_remove(&service, 1001, result.generation, &removed,
                               &generation) == 0);
    assert(removed);
    assert(dppd_rte_flow_backend_count(&service.rte_flow) == 0);
    fake.fail_validate = false;

    rule = make_rule(1002);
    rule.fallback = DPPD_FALLBACK_SOFTWARE_ONLY;
    rule.nb_actions = 2;
    rule.actions[0].type = DPPD_ACTION_COUNT;
    rule.actions[1].type = DPPD_ACTION_DROP;
    assert(dppd_control_apply(&service, 5, &rule, 0, &result) == 0);
    assert(result.status == DPPD_RULE_CREATED);
    assert(result.plan.backend == DPPD_PLAN_BACKEND_SOFTWARE);
    assert(dppd_software_backend_contains_version(&service.software, 1002,
                                                   result.generation));
    assert(dppd_rte_flow_backend_count(&service.rte_flow) == 0);
    assert(dppd_control_query_count(&service, 1002, result.generation,
                                    &count_result) == 0);
    assert(count_result.hits == 0 && count_result.bytes == 0);
    assert(dppd_control_remove(&service, 1002, result.generation, &removed,
                               &generation) == 0);
    assert(removed);

    rule = make_rule(1003);
    /* COUNT 必须位于 fate action 之前，和真实 rte_flow action 顺序保持一致。 */
    rule.nb_actions = 2;
    rule.actions[0].type = DPPD_ACTION_COUNT;
    rule.actions[1].type = DPPD_ACTION_DROP;
    assert(dppd_control_apply(&service, 5, &rule, 0, &result) == 0);
    assert(dppd_control_query_count(&service, 1003, 0,
                                    &count_result) == -ESTALE);
    assert(dppd_control_query_count(&service, 1003, result.generation,
                                    &count_result) == 0);
    assert(count_result.rule_id == 1003);
    assert(count_result.generation == result.generation);
    assert(count_result.hits == 123 && count_result.bytes == 4567);
    assert(fake.query_calls == 1);
    fake.fail_remove_at = fake.remove_calls;
    assert(dppd_control_remove(&service, 1003, result.generation, &removed,
                               &generation) == -EFAULT);
    assert(dppd_rule_repository_count(&service.rules) == 1);
    assert(dppd_rte_flow_backend_count(&service.rte_flow) == 1);
    fake.fail_remove_at = -1;
    assert(dppd_control_remove(&service, 1003, result.generation, &removed,
                               &generation) == 0);
    assert(dppd_control_fini(&service) == 0);

    /*
     * 使用不存在的父目录注入落盘失败：第一次 mutation 已经生效但返回 EUCLEAN，
     * 后续 mutation 在 preflight 被阻止；创建目录后，幂等重试先修复 snapshot，
     * 再允许新 mutation 继续。
     */
    snprintf(state_directory, sizeof(state_directory),
             "/tmp/dppd-control-state-%ld", (long)getpid());
    snprintf(state_path, sizeof(state_path), "%s/rules.bin", state_directory);
    snprintf(moved_directory, sizeof(moved_directory),
             "/tmp/dppd-control-state-moved-%ld", (long)getpid());
    snprintf(moved_path, sizeof(moved_path), "%s/rules.bin", moved_directory);
    unlink(state_path);
    rmdir(state_directory);
    unlink(moved_path);
    rmdir(moved_directory);
    memset(&fake, 0, sizeof(fake));
    fake.fail_create_at = -1;
    fake.fail_remove_at = -1;
    assert(dppd_control_init(&service, &topology, 4, &api) == 0);
    assert(dppd_control_persistence_attach(&service, state_path) == 0);

    rule = make_rule(2000);
    assert(dppd_control_apply(&service, 5, &rule, 0, &result) == -EUCLEAN);
    assert(result.status == DPPD_RULE_CREATED && result.generation == 1);
    assert(dppd_rule_repository_count(&service.rules) == 1);
    dppd_control_persistence_status(&service, &persistence_status);
    assert(persistence_status.enabled && persistence_status.dirty);
    assert(persistence_status.persisted_generation == 0);
    assert(persistence_status.current_generation == 1);
    assert(persistence_status.last_error == -ENOENT);

    rule = make_rule(2001);
    assert(dppd_control_apply(&service, 5, &rule, 0, &result) == -EUCLEAN);
    assert(dppd_rule_repository_count(&service.rules) == 1);
    assert(fake.create_calls == 1);

    assert(mkdir(state_directory, 0700) == 0);
    rule = make_rule(2000);
    assert(dppd_control_apply(&service, 5, &rule, 0, &result) == 0);
    assert(result.status == DPPD_RULE_UNCHANGED && result.generation == 1);
    dppd_control_persistence_status(&service, &persistence_status);
    assert(!persistence_status.dirty);
    assert(persistence_status.persisted_generation == 1);

    rule = make_rule(2001);
    assert(dppd_control_apply(&service, 5, &rule, 0, &result) == 0);
    assert(result.status == DPPD_RULE_CREATED && result.generation == 2);
    assert(dppd_control_remove(&service, 2001, 2, &removed, &generation) == 0);
    assert(removed && generation == 3);

    /*
     * 再覆盖 delete 已生效而 snapshot 保存失败的路径。把父目录整体移走可稳定注入
     * ENOENT，不依赖测试进程是否拥有 root 权限；显式 flush 修复后再核对最终快照。
     */
    rule = make_rule(2002);
    assert(dppd_control_apply(&service, 5, &rule, 0, &result) == 0);
    assert(result.status == DPPD_RULE_CREATED && result.generation == 4);
    assert(rename(state_directory, moved_directory) == 0);
    assert(dppd_control_remove(&service, 2002, 4, &removed,
                               &generation) == -EUCLEAN);
    assert(removed && generation == 5);
    dppd_control_persistence_status(&service, &persistence_status);
    assert(persistence_status.dirty);
    assert(persistence_status.persisted_generation == 4);
    assert(persistence_status.current_generation == 5);
    assert(persistence_status.last_error == -ENOENT);
    assert(mkdir(state_directory, 0700) == 0);
    assert(dppd_control_persistence_flush(&service) == 0);

    assert(dppd_persistence_load(state_path, &snapshot) == 0);
    assert(snapshot.repository_generation == 5);
    assert(snapshot.count == 1 && snapshot.rules[0].id == 2000);
    dppd_persisted_snapshot_destroy(&snapshot);

    assert(dppd_control_fini(&service) == 0);
    unlink(state_path);
    rmdir(state_directory);
    unlink(moved_path);
    rmdir(moved_directory);

    /* 配置一个不存在的 state file 时必须创建可校验的空 v2 snapshot，而非仅 attach。 */
    assert(mkdir(state_directory, 0700) == 0);
    memset(&fake, 0, sizeof(fake));
    fake.fail_create_at = -1;
    fake.fail_remove_at = -1;
    assert(dppd_control_init(&service, &topology, 4, &api) == 0);
    assert(dppd_control_persistence_restore(&service, state_path) == 0);
    assert(dppd_persistence_load(state_path, &snapshot) == 0);
    assert(snapshot.count == 0 && snapshot.repository_generation == 0);
    dppd_persisted_snapshot_destroy(&snapshot);
    dppd_control_persistence_status(&service, &persistence_status);
    assert(persistence_status.enabled && !persistence_status.dirty);
    assert(dppd_control_fini(&service) == 0);
    assert(unlink(state_path) == 0);
    assert(rmdir(state_directory) == 0);

    /*
     * 构造包含两个安装端口、且 rule/global generation 不完全相同的 v2 snapshot。
     * 这能证明恢复保留历史 generation，而不是把规则重新 apply 成 1..N。
     */
    assert(mkdir(state_directory, 0700) == 0);
    assert(dppd_rule_repository_init(&source_repository, 4) == 0);
    rule = make_rule(3000);
    rule.install_port_id = 5;
    assert(dppd_rule_repository_apply(&source_repository, &rule, 0,
                                      &repository_result) == 0);
    rule = make_rule(3001);
    rule.install_port_id = 6;
    assert(dppd_rule_repository_apply(&source_repository, &rule, 0,
                                      &repository_result) == 0);
    rule = make_rule(3000);
    rule.install_port_id = 5;
    rule.priority = 30;
    assert(dppd_rule_repository_apply(&source_repository, &rule, 1,
                                      &repository_result) == 0);
    assert(repository_result.generation == 3);
    assert(dppd_persistence_save(state_path, &source_repository) == 0);
    dppd_rule_repository_destroy(&source_repository);

    memset(&fake, 0, sizeof(fake));
    fake.fail_create_at = -1;
    fake.fail_remove_at = -1;
    assert(dppd_control_init(&service, &topology, 4, &api) == 0);
    assert(dppd_control_persistence_restore(&service, state_path) == 0);
    assert(dppd_rule_repository_count(&service.rules) == 2);
    assert(dppd_rule_repository_generation(&service.rules) == 3);
    assert(fake.create_calls == 2);
    assert(fake.create_ports[0] == 5 && fake.create_ports[1] == 6);
    assert(dppd_rule_repository_get(&service.rules, 3000, &restored) == 0);
    assert(restored.generation == 3 && restored.install_port_id == 5);
    assert(restored.priority == 30);
    assert(dppd_rule_repository_get(&service.rules, 3001, &restored) == 0);
    assert(restored.generation == 2 && restored.install_port_id == 6);
    dppd_control_persistence_status(&service, &persistence_status);
    assert(persistence_status.enabled && !persistence_status.dirty);
    assert(persistence_status.persisted_generation == 3);
    assert(persistence_status.current_generation == 3);
    assert(dppd_control_fini(&service) == 0);

    /*
     * 第二条 flow 创建失败且第一条回滚删除也失败时，不能丢掉本进程 handle 后直接
     * 退出。service 必须进入隔离状态，阻断普通写入，并支持对同一对象显式重试。
     */
    memset(&fake, 0, sizeof(fake));
    fake.fail_create_at = 1;
    fake.fail_remove_at = 0;
    assert(dppd_control_init(&service, &topology, 4, &api) == 0);
    assert(dppd_control_persistence_restore(&service, state_path) == -EUCLEAN);
    assert(fake.create_calls == 2 && fake.remove_calls == 1);
    assert(dppd_rule_repository_count(&service.rules) == 0);
    assert(dppd_rule_repository_generation(&service.rules) == 0);
    assert(dppd_rte_flow_backend_count(&service.rte_flow) == 1);
    dppd_control_persistence_status(&service, &persistence_status);
    assert(!persistence_status.enabled && !persistence_status.dirty);
    dppd_control_recovery_status(&service, &recovery_status);
    assert(recovery_status.state ==
           DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED);
    assert(recovery_status.residual_objects == 1);
    assert(recovery_status.last_error == -EFAULT);
    rule = make_rule(3002);
    assert(dppd_control_apply(&service, 5, &rule, 0, &result) == -EUCLEAN);
    assert(fake.create_calls == 2);

    /* fake_remove 只在第 0 次调用失败；retry 复用残留 handle 的下一次 remove 成功。 */
    assert(dppd_control_reconciliation_retry(&service) == 0);
    assert(fake.remove_calls == 2);
    dppd_control_recovery_status(&service, &recovery_status);
    assert(recovery_status.state == DPPD_CONTROL_RECOVERY_RESTART_REQUIRED);
    assert(recovery_status.residual_objects == 0);
    assert(recovery_status.last_error == 0);
    assert(dppd_control_reconciliation_retry(&service) == -EALREADY);
    assert(dppd_control_fini(&service) == 0);

    unlink(state_path);
    rmdir(state_directory);
    return 0;
}
