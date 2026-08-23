#include "dppd/control.h"
#include "dppd/persistence.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * 保存失败时规则可能已经在硬件和内存中生效，不能假装事务完全回滚。
 * service 进入 dirty/fail-stop 状态并向写请求返回 -EUCLEAN；下一次写入前必须
 * 先成功保存当前完整 repository，防止磁盘持续故障时偏差继续扩大。
 */
static int persist_current_repository(struct dppd_control_service *service)
{
    int rc;

    if (service->persistence_path == NULL)
        return 0;
    rc = dppd_persistence_save(service->persistence_path, &service->rules);
    if (rc != 0) {
        service->persistence_dirty = true;
        service->persistence_last_error = rc;
        return -EUCLEAN;
    }
    service->persistence_dirty = false;
    service->persistence_last_error = 0;
    service->persisted_generation =
        dppd_rule_repository_generation(&service->rules);
    return 0;
}

static int persistence_write_preflight(struct dppd_control_service *service)
{
    /*
     * clean 状态不做额外磁盘写；dirty 状态先保存“已经生效的当前状态”。
     * 成功后本次 mutation 才能继续，失败则保持 fail-stop。
     */
    return service->persistence_dirty ?
        persist_current_repository(service) : 0;
}

static int recovery_write_preflight(const struct dppd_control_service *service)
{
    /*
     * 进入恢复隔离模式意味着上一轮启动事务的 actual state 已不可信。此时绝不能
     * 接受 apply/delete 或重新覆盖 snapshot；只能通过专用 retry 清除遗留 handle。
     */
    return service->recovery_state == DPPD_CONTROL_RECOVERY_READY ? 0 :
        -EUCLEAN;
}

int dppd_control_init(struct dppd_control_service *service,
                      const struct dppd_topology *topology,
                      uint32_t rule_capacity,
                      const struct dppd_flow_api *flow_api)
{
    int rc;

    if (service == NULL || topology == NULL ||
        rule_capacity == 0 || rule_capacity == UINT32_MAX)
        return -EINVAL;
    memset(service, 0, sizeof(*service));
    rc = dppd_rule_repository_init(&service->rules, rule_capacity);
    if (rc != 0)
        return rc;
    /* 额外槽位用于规则更新时让新旧 generation 短暂共存。 */
    rc = dppd_rte_flow_backend_init(&service->rte_flow,
                                    rule_capacity + 1U, flow_api);
    if (rc != 0) {
        dppd_rule_repository_destroy(&service->rules);
        return rc;
    }
    service->topology = topology;
    service->next_transaction_id = 1;
    return 0;
}

int dppd_control_fini(struct dppd_control_service *service)
{
    int persistence_rc = 0;
    int rc;

    if (service == NULL)
        return -EINVAL;
    if (service->recovery_state == DPPD_CONTROL_RECOVERY_READY &&
        service->persistence_dirty)
        persistence_rc = dppd_control_persistence_flush(service);
    rc = dppd_rte_flow_backend_fini(&service->rte_flow);
    if (rc != 0)
        return rc;
    dppd_rule_repository_destroy(&service->rules);
    free(service->persistence_path);
    memset(service, 0, sizeof(*service));
    return persistence_rc;
}

int dppd_control_persistence_attach(struct dppd_control_service *service,
                                    const char *path)
{
    char *copy;
    uint64_t current_generation;

    if (service == NULL || path == NULL || path[0] == '\0' ||
        service->rules.records == NULL)
        return -EINVAL;
    if (service->persistence_path != NULL)
        return -EALREADY;
    if (recovery_write_preflight(service) != 0)
        return -EUCLEAN;
    copy = strdup(path);
    if (copy == NULL)
        return -ENOMEM;
    service->persistence_path = copy;
    current_generation = dppd_rule_repository_generation(&service->rules);
    /*
     * attach 本身不读取也不写入文件，因此不能凭空声称非零 generation 已落盘。
     * 如果调用方刚完成 snapshot 恢复，保守地标记 dirty 仍然安全：显式 flush 或下次
     * mutation 的 preflight 会重新保存一次当前完整状态，再允许新的变更继续。
     */
    service->persisted_generation = 0;
    service->persistence_dirty = current_generation != 0;
    service->persistence_last_error = 0;
    return 0;
}

int dppd_control_persistence_restore(struct dppd_control_service *service,
                                     const char *path)
{
    struct dppd_persisted_snapshot snapshot;
    struct dppd_transaction_item *items = NULL;
    struct dppd_transaction transaction;
    struct dppd_transaction_backends backends;
    char *path_copy = NULL;
    uint32_t i;
    int rollback_rc;
    int rc;

    if (service == NULL || service->topology == NULL || path == NULL ||
        path[0] == '\0' || service->rules.records == NULL)
        return -EINVAL;
    if (recovery_write_preflight(service) != 0)
        return -EUCLEAN;
    if (service->persistence_path != NULL ||
        dppd_rule_repository_count(&service->rules) != 0 ||
        dppd_rule_repository_generation(&service->rules) != 0 ||
        dppd_rte_flow_backend_count(&service->rte_flow) != 0)
        return -EBUSY;

    rc = dppd_persistence_load(path, &snapshot);
    if (rc == -ENOENT) {
        /*
         * 显式配置了 state path 就要求启动时建立可持久化的空基线。若目录或权限
         * 不可用，daemon 直接启动失败，不能悄悄退化成仅内存模式。
         */
        rc = dppd_control_persistence_attach(service, path);
        if (rc != 0)
            return rc;
        return dppd_control_persistence_flush(service);
    }
    if (rc != 0)
        return rc;
    if (snapshot.count > service->rules.capacity) {
        rc = -ENOSPC;
        goto cleanup;
    }

    /* 在接触硬件前完成所有本地内存分配，避免提交后因 ENOMEM 进入模糊状态。 */
    path_copy = strdup(path);
    if (path_copy == NULL) {
        rc = -ENOMEM;
        goto cleanup;
    }
    if (snapshot.count != 0) {
        items = calloc(snapshot.count, sizeof(*items));
        if (items == NULL) {
            rc = -ENOMEM;
            goto cleanup;
        }
    }

    for (i = 0; i < snapshot.count; ++i) {
        struct dppd_planner_context planner_context;

        memset(&planner_context, 0, sizeof(planner_context));
        planner_context.topology = service->topology;
        planner_context.install_port_id = snapshot.rules[i].install_port_id;
        planner_context.hardware_available = true;
        planner_context.software_equivalent = false;
        items[i].rule = snapshot.rules[i];
        rc = dppd_plan_rule(&planner_context, &items[i].rule,
                            &items[i].plan);
        if (rc != 0)
            goto cleanup;
        /* 当前恢复路径只支持已经实现事务语义的 rte_flow backend。 */
        if (items[i].plan.backend != DPPD_PLAN_BACKEND_RTE_FLOW) {
            rc = -ENOTSUP;
            goto cleanup;
        }
    }

    memset(&transaction, 0, sizeof(transaction));
    memset(&backends, 0, sizeof(backends));
    backends.rte_flow = dppd_rte_flow_transaction_backend(&service->rte_flow);
    if (snapshot.count != 0) {
        rc = dppd_transaction_init(&transaction,
                                   service->next_transaction_id++, items,
                                   snapshot.count);
        if (rc != 0)
            goto cleanup;
        /* 全量 validate/prepare/commit；任一失败由 transaction 自动逆序回滚。 */
        rc = dppd_transaction_run(&transaction, &backends);
        if (rc != 0) {
            if (transaction.rollback_code != 0) {
                /*
                 * transaction 已尽力回滚，但 backend 仍持有可定位对象。保留 service
                 * 和 handle，交给恢复隔离模式中的显式 retry；不能继续向下发布规则。
                 */
                service->recovery_state =
                    DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED;
                service->recovery_last_error = transaction.rollback_code;
                rc = -EUCLEAN;
            }
            goto cleanup;
        }
    }

    rc = dppd_rule_repository_restore(&service->rules, snapshot.rules,
                                      snapshot.count,
                                      snapshot.repository_generation);
    if (rc != 0) {
        /*
         * restore 已全量预检且不分配，正常不应失败；仍保留防御性硬件回滚，避免
         * 未来 repository 实现变化时留下“硬件已装、desired 未发布”的对象。
         */
        if (snapshot.count != 0) {
            rollback_rc = dppd_transaction_rollback_committed(&transaction,
                                                               &backends);
            if (rollback_rc != 0)
                rc = -EUCLEAN;
        }
        goto cleanup;
    }

    /* 文件正是本次恢复来源，因此可以确认磁盘与内存 generation 完全一致。 */
    service->persistence_path = path_copy;
    path_copy = NULL;
    service->persisted_generation = snapshot.repository_generation;
    service->persistence_dirty = false;
    service->persistence_last_error = 0;
    rc = 0;

cleanup:
    free(path_copy);
    free(items);
    dppd_persisted_snapshot_destroy(&snapshot);
    return rc;
}

int dppd_control_persistence_flush(struct dppd_control_service *service)
{
    int rc;

    if (service == NULL || service->persistence_path == NULL)
        return -EINVAL;
    if (recovery_write_preflight(service) != 0)
        return -EUCLEAN;
    rc = dppd_persistence_save(service->persistence_path, &service->rules);
    if (rc != 0) {
        service->persistence_dirty = true;
        service->persistence_last_error = rc;
        return rc;
    }
    service->persistence_dirty = false;
    service->persistence_last_error = 0;
    service->persisted_generation =
        dppd_rule_repository_generation(&service->rules);
    return 0;
}

void dppd_control_persistence_status(
    const struct dppd_control_service *service,
    struct dppd_control_persistence_status *status)
{
    if (status == NULL)
        return;
    memset(status, 0, sizeof(*status));
    if (service == NULL)
        return;
    status->enabled = service->persistence_path != NULL;
    status->dirty = service->persistence_dirty;
    status->persisted_generation = service->persisted_generation;
    status->current_generation =
        dppd_rule_repository_generation(&service->rules);
    status->last_error = service->persistence_last_error;
}

void dppd_control_recovery_status(
    const struct dppd_control_service *service,
    struct dppd_control_recovery_status *status)
{
    if (status == NULL)
        return;
    memset(status, 0, sizeof(*status));
    if (service == NULL)
        return;
    status->state = service->recovery_state;
    status->residual_objects =
        dppd_rte_flow_backend_count(&service->rte_flow);
    status->last_error = service->recovery_last_error;
}

int dppd_control_reconciliation_retry(struct dppd_control_service *service)
{
    uint32_t residual_objects;
    int rc;

    if (service == NULL)
        return -EINVAL;
    if (service->recovery_state == DPPD_CONTROL_RECOVERY_READY ||
        service->recovery_state == DPPD_CONTROL_RECOVERY_RESTART_REQUIRED)
        return -EALREADY;

    rc = dppd_rte_flow_backend_reconcile(&service->rte_flow,
                                         &residual_objects);
    if (rc != 0 || residual_objects != 0) {
        /*
         * remove 失败时仍保留对象和其 handle，下一次 retry 可以继续调用同一个 PMD
         * destroy。API 返回 EUCLEAN，具体底层 errno 通过 status.last_error 提供。
         */
        service->recovery_last_error = rc != 0 ? rc : -EUCLEAN;
        return -EUCLEAN;
    }
    service->recovery_last_error = 0;
    service->recovery_state = DPPD_CONTROL_RECOVERY_RESTART_REQUIRED;
    return 0;
}

int dppd_control_apply(struct dppd_control_service *service,
                       uint16_t install_port_id,
                       const struct dppd_rule *rule,
                       uint64_t expected_generation,
                       struct dppd_control_apply_result *result)
{
    struct dppd_rule existing;
    struct dppd_rule candidate;
    struct dppd_planner_context planner_context;
    struct dppd_transaction_item item;
    struct dppd_transaction transaction;
    struct dppd_transaction_backends backends;
    struct dppd_rule_apply_result repository_result;
    struct dppd_flow_error flow_error;
    bool updating = false;
    int rc;

    if (service == NULL || service->topology == NULL ||
        rule == NULL || rule->id == 0 || result == NULL)
        return -EINVAL;
    memset(result, 0, sizeof(*result));
    rc = recovery_write_preflight(service);
    if (rc != 0)
        return rc;
    rc = persistence_write_preflight(service);
    if (rc != 0)
        return rc;

    /* install_port_id 是持久化 desired state 的一部分，由 API 参数统一覆盖客户端值。 */
    candidate = *rule;
    candidate.install_port_id = install_port_id;
    rc = dppd_rule_repository_get(&service->rules, rule->id, &existing);
    if (rc == 0) {
        if (dppd_rule_equal(&existing, &candidate)) {
            result->status = DPPD_RULE_UNCHANGED;
            result->generation = existing.generation;
            return 0;
        }
        if (expected_generation != DPPD_RULE_GENERATION_ANY &&
            expected_generation != existing.generation)
            return -ESTALE;
        updating = true;
    } else if (rc != -ENOENT) {
        return rc;
    } else {
        if (expected_generation != DPPD_RULE_GENERATION_ANY &&
            expected_generation != 0)
            return -ESTALE;
    }

    candidate.generation = dppd_rule_repository_generation(&service->rules) + 1U;
    planner_context.topology = service->topology;
    planner_context.install_port_id = install_port_id;
    planner_context.hardware_available = true;
    planner_context.software_equivalent = false;
    rc = dppd_plan_rule(&planner_context, &candidate, &result->plan);
    if (rc != 0)
        return rc;

    memset(&item, 0, sizeof(item));
    item.rule = candidate;
    item.plan = result->plan;
    memset(&backends, 0, sizeof(backends));
    backends.rte_flow = dppd_rte_flow_transaction_backend(&service->rte_flow);
    result->transaction_id = service->next_transaction_id++;
    rc = dppd_transaction_init(&transaction, result->transaction_id, &item, 1);
    if (rc != 0)
        return rc;
    rc = dppd_transaction_run(&transaction, &backends);
    if (rc != 0)
        return rc;

    if (updating) {
        int rollback_rc;

        /*
         * 新 generation 已创建后才删除旧对象；旧对象删除失败时撤销新对象，
         * repository 仍指向旧 generation，因此控制面不会发布半更新状态。
         */
        rc = dppd_rte_flow_backend_remove_version(&service->rte_flow,
                                                   existing.id,
                                                   existing.generation,
                                                   &flow_error);
        if (rc != 0) {
            rollback_rc = dppd_rte_flow_backend_remove_version(
                &service->rte_flow, candidate.id, candidate.generation,
                &flow_error);
            return rollback_rc == 0 ? rc : -EUCLEAN;
        }
    }

    rc = dppd_rule_repository_apply(&service->rules, &candidate,
                                    updating ? existing.generation : expected_generation,
                                    &repository_result);
    if (rc != 0) {
        /* 单控制线程下不应到达这里；到达即表示 actual/desired 已失配。 */
        return -EUCLEAN;
    }
    result->status = repository_result.status;
    result->generation = repository_result.generation;
    return persist_current_repository(service);
}

int dppd_control_remove(struct dppd_control_service *service,
                        uint64_t rule_id,
                        uint64_t expected_generation,
                        bool *removed,
                        uint64_t *generation)
{
    struct dppd_rule existing;
    struct dppd_flow_error flow_error;
    int rc;

    if (service == NULL || rule_id == 0 || removed == NULL || generation == NULL)
        return -EINVAL;
    rc = recovery_write_preflight(service);
    if (rc != 0)
        return rc;
    rc = persistence_write_preflight(service);
    if (rc != 0)
        return rc;
    rc = dppd_rule_repository_get(&service->rules, rule_id, &existing);
    if (rc == -ENOENT)
        return dppd_rule_repository_remove(&service->rules, rule_id,
                                           expected_generation, removed,
                                           generation);
    if (rc != 0)
        return rc;
    if (expected_generation != DPPD_RULE_GENERATION_ANY &&
        expected_generation != existing.generation)
        return -ESTALE;
    if (dppd_rte_flow_backend_find_version(&service->rte_flow, rule_id,
                                            existing.generation) == NULL)
        return -EUCLEAN;
    rc = dppd_rte_flow_backend_remove_version(&service->rte_flow, rule_id,
                                               existing.generation, &flow_error);
    if (rc != 0)
        return rc;
    rc = dppd_rule_repository_remove(&service->rules, rule_id,
                                      existing.generation, removed, generation);
    if (rc != 0 || !*removed)
        return rc;
    return persist_current_repository(service);
}

int dppd_control_query_count(struct dppd_control_service *service,
                             uint64_t rule_id,
                             uint64_t expected_generation,
                             struct dppd_control_count_result *result)
{
    const struct dppd_flow_handle *handle;
    struct dppd_flow_error flow_error;
    struct dppd_rule rule;
    int rc;

    if (service == NULL || rule_id == 0 || result == NULL)
        return -EINVAL;
    memset(result, 0, sizeof(*result));
    if (recovery_write_preflight(service) != 0)
        return -EUCLEAN;

    /*
     * repository 是 desired state 的发布点。先读 repository 再定位同 generation
     * 的硬件 handle，确保查询结果不会来自尚未发布或已经退休的 flow。
     */
    rc = dppd_rule_repository_get(&service->rules, rule_id, &rule);
    if (rc != 0)
        return rc;
    if (expected_generation != DPPD_RULE_GENERATION_ANY &&
        expected_generation != rule.generation)
        return -ESTALE;
    handle = dppd_rte_flow_backend_find_version(&service->rte_flow, rule.id,
                                                 rule.generation);
    if (handle == NULL)
        return -EUCLEAN;
    if (!handle->has_count)
        return -ENODATA;

    rc = dppd_rte_flow_backend_query_count(&service->rte_flow, rule.id,
                                            rule.generation, &result->hits,
                                            &result->bytes, &flow_error);
    if (rc != 0)
        return rc;
    result->rule_id = rule.id;
    result->generation = rule.generation;
    return 0;
}
