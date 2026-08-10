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
    struct dppd_control_count_result count_result;
    struct dppd_control_persistence_status persistence_status;
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

    fake.fail_validate = true;
    rule = make_rule(1001);
    assert(dppd_control_apply(&service, 5, &rule, 0, &result) == -ENOTSUP);
    assert(dppd_rule_repository_count(&service.rules) == 0);
    assert(dppd_rte_flow_backend_count(&service.rte_flow) == 0);
    fake.fail_validate = false;

    rule = make_rule(1002);
    rule.fallback = DPPD_FALLBACK_SOFTWARE_ONLY;
    assert(dppd_control_apply(&service, 5, &rule, 0, &result) == -ENOTSUP);
    assert(dppd_rule_repository_count(&service.rules) == 0);

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
     * 第二条 flow 创建失败时，transaction 必须删除第一条已创建对象，且 repository
     * 保持全空、persistence 保持未 attach；daemon 因而可以安全地拒绝启动。
     */
    memset(&fake, 0, sizeof(fake));
    fake.fail_create_at = 1;
    fake.fail_remove_at = -1;
    assert(dppd_control_init(&service, &topology, 4, &api) == 0);
    assert(dppd_control_persistence_restore(&service, state_path) == -EIO);
    assert(fake.create_calls == 2 && fake.remove_calls == 1);
    assert(dppd_rule_repository_count(&service.rules) == 0);
    assert(dppd_rule_repository_generation(&service.rules) == 0);
    assert(dppd_rte_flow_backend_count(&service.rte_flow) == 0);
    dppd_control_persistence_status(&service, &persistence_status);
    assert(!persistence_status.enabled && !persistence_status.dirty);
    assert(dppd_control_fini(&service) == 0);

    unlink(state_path);
    rmdir(state_directory);
    return 0;
}
