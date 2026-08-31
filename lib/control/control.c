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

/*
 * desired repository 不保存 backend 类型；删除时以同一 (id,generation) 在两个
 * 实际对象仓库中定位。正常路径只能命中一个，两个都命中代表控制面不变量被破坏。
 */
/**
 * 找出某个已发布 generation 的唯一 actual 归属。repository 只保存 canonical rule，
 * 故删除和补偿不能根据 fallback 字段猜测 backend：PREFER 规则可能在 validate 失败后
 * 已真实落到 software。两个 backend 都有或都没有该版本均是控制面不变量破坏。
 */
static int actual_backend_for(const struct dppd_control_service *service,
                              uint64_t rule_id, uint64_t generation,
                              enum dppd_plan_backend *backend_kind)
{
    bool has_hardware;
    bool has_software;

    if (service == NULL || backend_kind == NULL)
        return -EINVAL;
    has_hardware = dppd_rte_flow_backend_find_version(&service->rte_flow,
                                                        rule_id, generation) != NULL;
    has_software = dppd_software_backend_contains_version(&service->software,
                                                            rule_id, generation);
    if (has_hardware == has_software)
        return -EUCLEAN;
    *backend_kind = has_hardware ? DPPD_PLAN_BACKEND_RTE_FLOW :
                                  DPPD_PLAN_BACKEND_SOFTWARE;
    return 0;
}

static int remove_actual_rule(struct dppd_control_service *service,
                              uint64_t rule_id, uint64_t generation)
{
    struct dppd_flow_error flow_error;
    enum dppd_plan_backend backend_kind;
    int rc;

    /* 先定位再删除，避免将“对象不存在”降格成单条 delete 的幂等 no-op。 */
    rc = actual_backend_for(service, rule_id, generation, &backend_kind);
    if (rc != 0)
        return rc;
    if (backend_kind == DPPD_PLAN_BACKEND_RTE_FLOW)
        return dppd_rte_flow_backend_remove_version(&service->rte_flow,
                                                    rule_id, generation,
                                                    &flow_error);
    return dppd_software_backend_remove_version(&service->software,
                                                rule_id, generation);
}

/**
 * 恢复批量删除已成功摘除的 actual 对象。desired repository 尚未变更，所以必须用原始
 * rule/generation 重建同一版本；若 PMD 此时已不再接受该版本，宁可进入隔离也不能发布
 * 一个没有实际对象的 desired record。
 */
static int restore_actual_rules(struct dppd_control_service *service,
                                struct dppd_transaction_item *items,
                                uint32_t count)
{
    struct dppd_transaction transaction;
    struct dppd_transaction_backends backends;
    uint32_t i;
    int rc;

    if (count == 0)
        return 0;
    memset(&backends, 0, sizeof(backends));
    backends.software = dppd_software_transaction_backend(&service->software);
    backends.rte_flow = dppd_rte_flow_transaction_backend(&service->rte_flow);
    for (i = 0; i < count; ++i) {
        items[i].plan.rule_id = items[i].rule.id;
        items[i].plan.rule_generation = items[i].rule.generation;
        items[i].plan.install_port_id = items[i].rule.install_port_id;
    }
    rc = dppd_transaction_init(&transaction, service->next_transaction_id++, items, count);
    if (rc == 0)
        rc = dppd_transaction_run(&transaction, &backends);
    if (rc == 0)
        rc = dppd_transaction_finalize(&transaction, &backends);
    return rc;
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
    rc = dppd_software_backend_init(&service->software, rule_capacity + 1U);
    if (rc != 0) {
        (void)dppd_rte_flow_backend_fini(&service->rte_flow);
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
    dppd_software_backend_fini(&service->software);
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
        planner_context.software_equivalent =
            dppd_software_backend_rule_supported(&snapshot.rules[i]);
        items[i].rule = snapshot.rules[i];
        rc = dppd_plan_rule(&planner_context, &items[i].rule,
                            &items[i].plan);
        if (rc != 0)
            goto cleanup;
        if (items[i].plan.backend == DPPD_PLAN_BACKEND_RTE_FLOW &&
            items[i].rule.fallback == DPPD_FALLBACK_PREFER_HARDWARE &&
            planner_context.software_equivalent) {
            struct dppd_flow_error flow_error;

            /*
             * 恢复事务必须在进入 prepare 前完成每条可回退规则的 backend 选择。
             * 这里额外 validate 一次仅作无副作用的能力探测；失败后整个批次仍可
             * 与其他硬件规则一起原子提交，不会因为 net_ring 等 PMD 而拒绝启动。
             */
            rc = service->rte_flow.api.validate(
                items[i].plan.install_port_id, &items[i].rule, &flow_error);
            if (rc != 0) {
                planner_context.hardware_available = false;
                rc = dppd_plan_rule(&planner_context, &items[i].rule,
                                    &items[i].plan);
                if (rc != 0)
                    goto cleanup;
            }
        }
    }

    memset(&transaction, 0, sizeof(transaction));
    memset(&backends, 0, sizeof(backends));
    backends.software = dppd_software_transaction_backend(&service->software);
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
    if (snapshot.count != 0) {
        rc = dppd_transaction_finalize(&transaction, &backends);
        if (rc != 0)
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
    planner_context.software_equivalent =
        dppd_software_backend_rule_supported(&candidate);
    rc = dppd_plan_rule(&planner_context, &candidate, &result->plan);
    if (rc != 0)
        return rc;

    memset(&item, 0, sizeof(item));
    item.rule = candidate;
    item.plan = result->plan;
    memset(&backends, 0, sizeof(backends));
    backends.software = dppd_software_transaction_backend(&service->software);
    backends.rte_flow = dppd_rte_flow_transaction_backend(&service->rte_flow);
    result->transaction_id = service->next_transaction_id++;
    rc = dppd_transaction_init(&transaction, result->transaction_id, &item, 1);
    if (rc != 0)
        return rc;
    rc = dppd_transaction_run(&transaction, &backends);
    if (rc != 0 && candidate.fallback == DPPD_FALLBACK_PREFER_HARDWARE &&
        transaction.state == DPPD_TRANSACTION_FAILED &&
        item.state == DPPD_TRANSACTION_ITEM_FAILED && item.backend_token == 0 &&
        dppd_software_backend_rule_supported(&candidate)) {
        /*
         * 只在 validate 尚未预留/创建对象时回退，绝不把 create 或 rollback 的
         * 不确定失败伪装为 software 成功。这样 REQUIRE_HARDWARE 仍是硬边界，
         * PREFER_HARDWARE 也只在已证明可等价执行时才改变 backend。
         */
        planner_context.hardware_available = false;
        planner_context.software_equivalent = true;
        rc = dppd_plan_rule(&planner_context, &candidate, &result->plan);
        if (rc != 0)
            return rc;
        item.plan = result->plan;
        rc = dppd_transaction_init(&transaction, result->transaction_id,
                                   &item, 1);
        if (rc != 0)
            return rc;
        rc = dppd_transaction_run(&transaction, &backends);
    }
    if (rc != 0)
        return rc;

    if (updating) {
        int rollback_rc;

        /*
         * 新 generation 已创建后才删除旧对象；旧对象删除失败时撤销新对象，
         * repository 仍指向旧 generation，因此控制面不会发布半更新状态。
         */
        rc = remove_actual_rule(service, existing.id, existing.generation);
        if (rc != 0) {
            rollback_rc = remove_actual_rule(service, candidate.id,
                                             candidate.generation);
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
    rc = dppd_transaction_finalize(&transaction, &backends);
    if (rc != 0)
        return -EUCLEAN;
    return persist_current_repository(service);
}

/**
 * 批量新建规则的控制面原子边界。
 *
 * 首版不混入更新和删除：所有输入都必须是不存在的 rule，且 expected_generation 为 0。
 * 这样可在接触 backend 前一次性完成 ID、容量、计划和 fallback 预检；backend 事务成功
 * 后才连续发布 repository。任何 backend 失败都会撤销本批已创建对象。
 */
int dppd_control_create_batch(
    struct dppd_control_service *service,
    const struct dppd_control_batch_create_request *requests,
    uint32_t request_count,
    struct dppd_control_apply_result *results)
{
    struct dppd_transaction_item *items = NULL;
    struct dppd_transaction transaction;
    struct dppd_transaction_backends backends;
    uint64_t base_generation;
    uint64_t transaction_id;
    uint32_t i;
    uint32_t published_count = 0;
    int rc;

    if (service == NULL || service->topology == NULL || requests == NULL ||
        results == NULL || request_count == 0)
        return -EINVAL;
    rc = recovery_write_preflight(service);
    if (rc != 0)
        return rc;
    rc = persistence_write_preflight(service);
    if (rc != 0)
        return rc;
    if (request_count > service->rules.capacity -
                        dppd_rule_repository_count(&service->rules))
        return -ENOSPC;

    items = calloc(request_count, sizeof(*items));
    if (items == NULL)
        return -ENOMEM;
    /*
     * generation 按输入顺序预分配。后续 repository 也必须按同一顺序 apply，才能保证
     * 每条 actual rule 的 generation 与 desired record 完全一致，并便于调用方按结果数组
     * 建立稳定映射。
     */
    base_generation = dppd_rule_repository_generation(&service->rules);

    /*
     * 此循环不触碰 backend：先冻结本批输入的唯一性、乐观并发条件、generation 和
     * 执行计划，确保后续 transaction 不会因本地可预见错误出现半提交。
     */
    for (i = 0; i < request_count; ++i) {
        struct dppd_planner_context planner_context;
        struct dppd_rule existing;
        uint32_t previous;

        /* 首版批量接口只表达“新建”：不接受 ANY/指定旧 generation，避免混入更新语义后
         * 出现部分条目创建、部分条目覆盖的难以补偿状态。 */
        if (requests[i].rule.id == 0 ||
            requests[i].expected_generation != 0) {
            rc = -ESTALE;
            goto cleanup;
        }
        for (previous = 0; previous < i; ++previous) {
            if (requests[previous].rule.id == requests[i].rule.id) {
                rc = -EEXIST;
                goto cleanup;
            }
        }
        rc = dppd_rule_repository_get(&service->rules, requests[i].rule.id,
                                      &existing);
        if (rc != -ENOENT) {
            rc = rc == 0 ? -EEXIST : rc;
            goto cleanup;
        }

        memset(&results[i], 0, sizeof(results[i]));
        items[i].rule = requests[i].rule;
        /* install port 与单规则 API 同样由 request 外层字段统一覆盖。 */
        items[i].rule.install_port_id = requests[i].install_port_id;
        items[i].rule.generation = base_generation + i + 1U;
        memset(&planner_context, 0, sizeof(planner_context));
        planner_context.topology = service->topology;
        planner_context.install_port_id = requests[i].install_port_id;
        planner_context.hardware_available = true;
        planner_context.software_equivalent =
            dppd_software_backend_rule_supported(&items[i].rule);
        rc = dppd_plan_rule(&planner_context, &items[i].rule, &items[i].plan);
        if (rc != 0)
            goto cleanup;

        /*
         * PREFER 的硬件能力探测必须在批量 prepare 前完成。这样一个 net_ring 等
         * PMD 的 validate 失败只重规划该条 rule 为 software，不会中断同批其余
         * 硬件规则的原子 transaction；create/rollback 失败绝不走此降级分支。
         */
        if (items[i].plan.backend == DPPD_PLAN_BACKEND_RTE_FLOW &&
            items[i].rule.fallback == DPPD_FALLBACK_PREFER_HARDWARE &&
            planner_context.software_equivalent) {
            struct dppd_flow_error flow_error;

            rc = service->rte_flow.api.validate(items[i].plan.install_port_id,
                                                &items[i].rule, &flow_error);
            if (rc != 0) {
                planner_context.hardware_available = false;
                rc = dppd_plan_rule(&planner_context, &items[i].rule,
                                    &items[i].plan);
                if (rc != 0)
                    goto cleanup;
            }
        }
    }

    memset(&backends, 0, sizeof(backends));
    backends.software = dppd_software_transaction_backend(&service->software);
    backends.rte_flow = dppd_rte_flow_transaction_backend(&service->rte_flow);
    transaction_id = service->next_transaction_id++;
    rc = dppd_transaction_init(&transaction, transaction_id, items, request_count);
    if (rc != 0)
        goto cleanup;
    /* 此处先让所有 backend 完整提交，再一次性连续发布 desired repository。 */
    rc = dppd_transaction_run(&transaction, &backends);
    if (rc != 0) {
        /* rollback 也失败时，无法证明 actual 已恢复；进入隔离而不是继续接受写请求。 */
        if (transaction.rollback_code != 0) {
            service->recovery_state =
                DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED;
            service->recovery_last_error = transaction.rollback_code;
            rc = -EUCLEAN;
        }
        goto cleanup;
    }

    /*
     * 上面的全量预检保证这些 apply 只能依次创建，且分配的 generation 恰好等于
     * 预先写入 actual object 的值。若仍失败，actual/desired 已无法证明一致，先
     * 回滚整个已提交 transaction，再以 EUCLEAN fail-closed 返回。
     */
    for (i = 0; i < request_count; ++i) {
        struct dppd_rule_apply_result repository_result;

        rc = dppd_rule_repository_apply(&service->rules, &items[i].rule, 0,
                                        &repository_result);
        if (rc != 0 || repository_result.status != DPPD_RULE_CREATED ||
            repository_result.generation != items[i].rule.generation) {
            int rollback_rc;

            /*
             * repository 理论上不会失败；防御性地撤回此前已发布条目后再回滚 actual。
             * remove 会继续推进 repository generation，因此不能把它当作“时间倒流”；
             * 一旦任一补偿失败即记录 reconciliation_required，拒绝后续写入。
             */
            while (published_count > 0) {
                bool removed;
                uint64_t ignored_generation;

                --published_count;
                if (dppd_rule_repository_remove(
                        &service->rules, items[published_count].rule.id,
                        items[published_count].rule.generation, &removed,
                        &ignored_generation) != 0 || !removed) {
                    service->recovery_state =
                        DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED;
                    service->recovery_last_error = -EUCLEAN;
                }
            }
            rollback_rc = dppd_transaction_rollback_committed(&transaction,
                                                               &backends);
            if (rollback_rc != 0) {
                service->recovery_state =
                    DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED;
                service->recovery_last_error = rollback_rc;
            }
            rc = -EUCLEAN;
            goto cleanup;
        }
        published_count++;
        results[i].status = repository_result.status;
        results[i].generation = repository_result.generation;
        results[i].transaction_id = transaction_id;
        results[i].plan = items[i].plan;
    }
    /* 至此 actual 和 desired 均已发布，才可丢弃所有 backend 的 rollback token。 */
    rc = dppd_transaction_finalize(&transaction, &backends);
    if (rc != 0) {
        rc = -EUCLEAN;
        goto cleanup;
    }
    rc = persist_current_repository(service);

cleanup:
    free(items);
    return rc;
}

/**
 * 批量删除的原子边界与批量创建相反：desired record 在开始时必须都存在，先删除所有
 * actual 对象，随后才连续发布 repository 删除。没有把 DELETE 接入通用 transaction，
 * 是因为现有 transaction 的 prepare/commit 模型描述“创建对象”；这里显式保存原规则和
 * backend 类型，以便实际删除失败时可用创建事务补偿恢复。
 */
int dppd_control_remove_batch(
    struct dppd_control_service *service,
    const struct dppd_control_batch_remove_request *requests,
    uint32_t request_count,
    struct dppd_control_batch_remove_result *results)
{
    struct dppd_transaction_item *items;
    uint32_t removed_actual = 0;
    uint32_t i;
    int rc;

    if (service == NULL || requests == NULL || results == NULL || request_count == 0)
        return -EINVAL;
    rc = recovery_write_preflight(service);
    if (rc != 0)
        return rc;
    rc = persistence_write_preflight(service);
    if (rc != 0)
        return rc;
    items = calloc(request_count, sizeof(*items));
    if (items == NULL)
        return -ENOMEM;

    /*
     * 进入数据面前冻结全批前提：不允许 ANY/0、不允许重复 ID，并确认每个 desired
     * generation 仍对应唯一 actual backend。这样正常删除路径不含可预见失败点。
     */
    for (i = 0; i < request_count; ++i) {
        uint32_t previous;

        if (requests[i].rule_id == 0 || requests[i].expected_generation == 0 ||
            requests[i].expected_generation == DPPD_RULE_GENERATION_ANY) {
            rc = -ESTALE;
            goto cleanup;
        }
        for (previous = 0; previous < i; ++previous) {
            if (requests[previous].rule_id == requests[i].rule_id) {
                rc = -EEXIST;
                goto cleanup;
            }
        }
        rc = dppd_rule_repository_get(&service->rules, requests[i].rule_id,
                                      &items[i].rule);
        if (rc != 0)
            goto cleanup;
        if (items[i].rule.generation != requests[i].expected_generation) {
            rc = -ESTALE;
            goto cleanup;
        }
        rc = actual_backend_for(service, items[i].rule.id, items[i].rule.generation,
                                &items[i].plan.backend);
        if (rc != 0)
            goto cleanup;
        items[i].plan.rule_id = items[i].rule.id;
        items[i].plan.rule_generation = items[i].rule.generation;
        items[i].plan.install_port_id = items[i].rule.install_port_id;
    }

    for (i = 0; i < request_count; ++i) {
        rc = remove_actual_rule(service, items[i].rule.id, items[i].rule.generation);
        if (rc != 0) {
            int restore_rc = restore_actual_rules(service, items, removed_actual);

            /*
             * 此时 repository 尚完整；补偿成功即可向调用方返回原始 PMD/software 错误。
             * 只有补偿自身失败才失去 actual/desired 一致性证明，需要覆盖成 EUCLEAN。
             */
            if (restore_rc != 0) {
                service->recovery_state = DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED;
                service->recovery_last_error = restore_rc;
                rc = -EUCLEAN;
            }
            goto cleanup;
        }
        removed_actual++;
    }

    /*
     * repository remove 理论上不会失败，因为同一 control service 已完成精确预检且
     * 还未接受其他 mutation。若未来并发模型改变而这里失败，旧 actual 已被删、部分
     * desired 也可能已删，无法在不篡改 generation 的前提下安全补偿，只能隔离。
     */
    for (i = 0; i < request_count; ++i) {
        bool removed;
        uint64_t generation;

        rc = dppd_rule_repository_remove(&service->rules, items[i].rule.id,
                                         items[i].rule.generation, &removed,
                                         &generation);
        if (rc != 0 || !removed) {
            service->recovery_state = DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED;
            service->recovery_last_error = rc != 0 ? rc : -EUCLEAN;
            rc = -EUCLEAN;
            goto cleanup;
        }
        results[i].rule_id = items[i].rule.id;
        results[i].generation = generation;
    }
    rc = persist_current_repository(service);

cleanup:
    free(items);
    return rc;
}

int dppd_control_remove(struct dppd_control_service *service,
                        uint64_t rule_id,
                        uint64_t expected_generation,
                        bool *removed,
                        uint64_t *generation)
{
    struct dppd_rule existing;
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
    rc = remove_actual_rule(service, rule_id, existing.generation);
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
    if (dppd_software_backend_contains_version(&service->software, rule.id,
                                               rule.generation)) {
        rc = dppd_software_backend_query_count(&service->software, rule.id,
                                                rule.generation, &result->hits,
                                                &result->bytes);
    } else {
        handle = dppd_rte_flow_backend_find_version(&service->rte_flow, rule.id,
                                                     rule.generation);
        if (handle == NULL)
            return -EUCLEAN;
        if (!handle->has_count)
            return -ENODATA;
        rc = dppd_rte_flow_backend_query_count(&service->rte_flow, rule.id,
                                                rule.generation, &result->hits,
                                                &result->bytes, &flow_error);
    }
    if (rc != 0)
        return rc;
    result->rule_id = rule.id;
    result->generation = rule.generation;
    return 0;
}
