#include "dppd/control.h"
#include "dppd/persistence.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int control_flush_impl(struct dppd_control_service *service);

/** 阶段只影响当前管理请求的诊断上下文，不改变规划、事务或规则发布行为 */
static void observe_stage(struct dppd_control_service *service, enum dppd_rule_failure_stage stage)
{
    if (service->observation.active)
        service->observation.context.stage = stage;
}

/** 整体容量检查、整批账本发布和完整文件保存没有单个失败成员，明确清空成员身份 */
static void observe_batch(struct dppd_control_service *service, enum dppd_rule_failure_stage stage)
{
    observe_stage(service, stage);
    service->observation.context.rule_id = 0;
    service->observation.context.generation = 0;
    service->observation.context.port_known = false;
    service->observation.context.backend_known = false;
}

/** 在接触本条规则之前登记身份与计划，批量失败可以准确定位到失败成员 */
static void observe_rule(struct dppd_control_service *service, enum dppd_rule_failure_stage stage,
    const struct dppd_rule *rule, uint16_t port_id, const struct dppd_execution_plan *plan)
{
    struct dppd_rule_failure_event *context = &service->observation.context;

    if (!service->observation.active)
        return;
    context->stage = stage;
    context->rule_id = rule->id;
    context->generation = rule->generation;
    context->install_port_id = port_id;
    context->port_known = true;
    context->backend_known = plan != NULL;
    if (plan != NULL)
        context->backend = plan->backend;
}

/** 事务已回滚后条目状态会变化，失败阶段和下标仍由事务的独立记录提供 */
static void observe_transaction(struct dppd_control_service *service,
                                 const struct dppd_transaction *transaction)
{
    if (transaction->failure_code != 0 && transaction->failure_item < transaction->nb_items) {
        const struct dppd_transaction_item *item = &transaction->items[transaction->failure_item];

        observe_rule(service, transaction->failure_stage, &item->rule,
                      item->plan.install_port_id, &item->plan);
        service->observation.context.transaction_id = transaction->id;
        dppd_rule_observation_fault(&service->observation, transaction->failure_code);
    }
    if (transaction->rollback_code != 0) {
        uint64_t id = transaction->rollback_item < transaction->nb_items ?
            transaction->items[transaction->rollback_item].rule.id : 0;

        dppd_rule_observation_compensation(&service->observation, transaction->rollback_code, id);
    }
}

/** 无法证明对象与账本一致时封锁后续写入，原始失败已由观测上下文单独保留 */
static int isolate_control(struct dppd_control_service *service, int code)
{
    service->recovery_state = DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED;
    service->recovery_last_error = code;
    return -EUCLEAN;
}

/**
 * 把当前完整规则账本写入快照，未配置保存路径时直接成功返回
 * 保存发生在规则生效之后，写磁盘失败不能被解释为已经回滚规则
 * 此时设置 dirty 并返回 EUCLEAN，后续修改必须先补写成功，避免磁盘与内存越差越远
 */
static int persist_current_repository(struct dppd_control_service *service)
{
    struct dppd_rule_failure_event previous = service->observation.context;
    int rc;

    if (service->persistence_path == NULL)
        return 0;
    observe_stage(service, DPPD_RULE_STAGE_PERSIST);
    if (service->observation.context.rule_count > 1)
        observe_batch(service, DPPD_RULE_STAGE_PERSIST);
    rc = dppd_persistence_save(service->persistence_path, &service->rules);
    if (rc != 0) {
        dppd_rule_observation_fault(&service->observation, rc);
        service->persistence_dirty = true;
        service->persistence_last_error = rc;
        return -EUCLEAN;
    }
    service->persistence_dirty = false;
    service->persistence_last_error = 0;
    service->persisted_generation =
        dppd_rule_repository_generation(&service->rules);
    service->observation.context = previous;
    return 0;
}

/** 新修改开始前检查上次保存是否失败，必要时先补写当前已经生效的完整状态 */
static int persistence_write_preflight(struct dppd_control_service *service)
{
    /**
     * clean 状态不重复保存，dirty 状态先修复上次未完成的保存
     * 修复失败就拒绝本次修改，这里不会通过丢弃内存中的规则来消除 dirty
     */
    return service->persistence_dirty ?
        persist_current_repository(service) : 0;
}

/** 只有 READY 状态允许普通写入，恢复隔离和等待重启期间都返回 EUCLEAN */
static int recovery_write_preflight(const struct dppd_control_service *service)
{
    /**
     * 启动恢复或在线补偿失败后，实际对象与账本可能已经不一致
     * 此时禁止继续更新、删除或覆盖快照，只允许专用恢复流程清理本进程仍持有的对象
     */
    return service->recovery_state == DPPD_CONTROL_RECOVERY_READY ? 0 :
        -EUCLEAN;
}

/**
 * 找出一条已发布规则究竟安装在硬件还是软件中，规则账本本身不记录后端类型
 * PREFER 只表达“优先硬件”的意图，驱动校验失败后实际规则可能已经落到软件后端
 * 因此要在两个对象仓库中按同一个 ID 和版本查找，不能只根据策略字段猜测
 * 正常情况恰好找到一个；两个都有或两个都没有，都说明控制层约定已经被破坏
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

/**
 * 删除指定版本的实际对象，但不修改规则账本
 * 上层可先删除整批对象，确认全部成功后再统一发布账本变化
 */
static int remove_actual_rule(struct dppd_control_service *service,
                              uint64_t rule_id, uint64_t generation)
{
    struct dppd_flow_error flow_error;
    enum dppd_plan_backend backend_kind;
    int rc;

    /** 账本认为对象存在时，实际对象缺失就是错误，不能当作“已经删过了”的幂等成功 */
    rc = actual_backend_for(service, rule_id, generation, &backend_kind);
    if (rc != 0)
        return rc;
    if (service->observation.active) {
        service->observation.context.backend = backend_kind;
        service->observation.context.backend_known = true;
    }
    if (backend_kind == DPPD_PLAN_BACKEND_RTE_FLOW)
        return dppd_rte_flow_backend_remove_version(&service->rte_flow,
                                                    rule_id, generation,
                                                    &flow_error);
    return dppd_software_backend_remove_version(&service->software,
                                                rule_id, generation);
}

/**
 * 恢复批量删除或更新过程中已经删掉的那一部分旧对象
 * 账本尚未变化，所以要保留原 ID、版本、端口和后端，不能调用普通 apply 分配一个新版本
 * 这里通过创建事务重建对象，count 为零表示还没有删掉任何旧对象，无需补偿
 * 恢复失败时把错误交回上层，由上层决定隔离，不能假装账本中的规则已经重新存在
 */
static int restore_actual_rules(struct dppd_control_service *service,
                                struct dppd_transaction_item *items,
                                uint32_t count)
{
    struct dppd_transaction transaction = {0};
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
    if (rc != 0) {
        uint64_t id = transaction.failure_item < count ?
            items[transaction.failure_item].rule.id : items[0].rule.id;

        dppd_rule_observation_compensation(&service->observation, rc, id);
    }
    return rc;
}

int dppd_control_init(struct dppd_control_service *service,
                      const struct dppd_topology *topology,
                      uint32_t rule_capacity,
                      const struct dppd_flow_api *flow_api)
{
    int rc;

    if (service == NULL || topology == NULL ||
        rule_capacity == 0 || rule_capacity == UINT32_MAX ||
        topology->nb_endpoints > DPPD_MAX_PORTS)
        return -EINVAL;
    memset(service, 0, sizeof(*service));
    rc = dppd_rule_repository_init(&service->rules, rule_capacity);
    if (rc != 0)
        return rc;
    /**
     * 后端比规则账本多一个槽位，确保满表时仍可进行一次单规则的先建后删
     * 批量更新需要同时容纳多条新版本，仅多一个槽位并不保证满表时也能完成批量更新
     */
    rc = dppd_rte_flow_backend_init(&service->rte_flow,
                                    rule_capacity + 1U, flow_api);
    if (rc != 0) {
        dppd_rule_repository_destroy(&service->rules);
        return rc;
    }
    /** 启动时登记拓扑内的全部端口，画像查询即使尚无规则也能显示空的校验记录 */
    for (uint16_t i = 0; i < topology->nb_endpoints; ++i) {
        rc = dppd_flow_probe_cache_register(service->rte_flow.probes,
                                            topology->endpoints[i].ethdev_port_id);
        if (rc != 0) {
            (void)dppd_rte_flow_backend_fini(&service->rte_flow);
            dppd_rule_repository_destroy(&service->rules);
            return rc;
        }
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

/**
 * 先检查完整规则和端口拓扑，再做无安装副作用的驱动探测
 * 硬件探测会忽略用户的后端偏好，因此 software 规则也能询问相同内容的驱动支持情况
 */
int dppd_control_probe_rule(struct dppd_control_service *service, uint16_t install_port_id,
    const struct dppd_rule *rule, bool refresh, struct dppd_flow_probe_result *result)
{
    struct dppd_planner_context context = {0};
    struct dppd_execution_plan plan;
    struct dppd_rule candidate;
    char error[128];
    int rc;

    if (service == NULL || rule == NULL || result == NULL)
        return -EINVAL;
    memset(result, 0, sizeof(*result));
    if (recovery_write_preflight(service) != 0)
        return -EUCLEAN;
    if (rule->id == 0 || dppd_rule_validate(rule, error, sizeof(error)) != 0)
        return -EINVAL;
    candidate = *rule;
    candidate.install_port_id = install_port_id;
    candidate.generation = 0;
    candidate.fallback = DPPD_FALLBACK_REQUIRE_HARDWARE;
    context.topology = service->topology;
    context.install_port_id = install_port_id;
    context.hardware_available = true;
    rc = dppd_plan_rule(&context, &candidate, &plan);
    if (rc != 0)
        return rc;
    rc = dppd_flow_probe_cache_query(service->rte_flow.probes, install_port_id,
        &candidate, refresh, service->rte_flow.api.validate, result);
    if (rc == 0)
        result->software_equivalent = dppd_software_backend_rule_supported(&candidate);
    return rc;
}

int dppd_control_fini(struct dppd_control_service *service)
{
    int persistence_rc = 0;
    int rc;

    if (service == NULL)
        return -EINVAL;
    if (service->recovery_state == DPPD_CONTROL_RECOVERY_READY &&
        service->persistence_dirty)
        persistence_rc = control_flush_impl(service);
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

static int control_restore_impl(struct dppd_control_service *service,
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
        dppd_rte_flow_backend_count(&service->rte_flow) != 0 ||
        dppd_software_backend_count(&service->software) != 0)
        return -EBUSY;

    observe_stage(service, DPPD_RULE_STAGE_LOAD);
    rc = dppd_persistence_load(path, &snapshot);
    if (rc == -ENOENT) {
        /*
         * 显式配置了 state path 就要求启动时建立可持久化的空基线。若目录或权限
         * 不可用，daemon 直接启动失败，不能悄悄退化成仅内存模式。
         */
        observe_stage(service, DPPD_RULE_STAGE_PREFLIGHT);
        rc = dppd_control_persistence_attach(service, path);
        if (rc != 0)
            return rc;
        return control_flush_impl(service);
    }
    if (rc != 0)
        return rc;
    observe_stage(service, DPPD_RULE_STAGE_PREFLIGHT);
    service->observation.context.rule_count = snapshot.count;
    if (snapshot.count > service->rules.capacity) {
        rc = -ENOSPC;
        goto cleanup;
    }

    /* 在接触硬件前完成所有本地内存分配，避免提交后因 ENOMEM 进入模糊状态。 */
    observe_stage(service, DPPD_RULE_STAGE_PREPARE);
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
        observe_rule(service, DPPD_RULE_STAGE_PLAN, &items[i].rule,
                      planner_context.install_port_id, NULL);
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
            rc = dppd_rte_flow_backend_validate_rule(&service->rte_flow,
                items[i].plan.install_port_id, &items[i].rule, &flow_error);
            if (rc != 0) {
                planner_context.hardware_available = false;
                rc = dppd_plan_rule(&planner_context, &items[i].rule,
                                    &items[i].plan);
                if (rc != 0)
                    goto cleanup;
                service->observation.fallback_rules++;
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
            observe_transaction(service, &transaction);
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

    observe_batch(service, DPPD_RULE_STAGE_PUBLISH);
    rc = dppd_rule_repository_restore(&service->rules, snapshot.rules,
                                      snapshot.count,
                                      snapshot.repository_generation);
    if (rc != 0) {
        dppd_rule_observation_fault(&service->observation, rc);
        /*
         * restore 已全量预检且不分配，正常不应失败；仍保留防御性硬件回滚，避免
         * 未来 repository 实现变化时留下“硬件已装、desired 未发布”的对象。
         */
        if (snapshot.count != 0) {
            rollback_rc = dppd_transaction_rollback_committed(&transaction,
                                                               &backends);
            if (rollback_rc != 0)
                rc = -EUCLEAN;
            observe_transaction(service, &transaction);
        }
        goto cleanup;
    }
    service->observation.applied = snapshot.count != 0 || snapshot.repository_generation != 0;
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

static int control_flush_impl(struct dppd_control_service *service)
{
    int rc;

    if (service == NULL || service->persistence_path == NULL)
        return -EINVAL;
    if (recovery_write_preflight(service) != 0)
        return -EUCLEAN;
    observe_stage(service, DPPD_RULE_STAGE_PERSIST);
    rc = dppd_persistence_save(service->persistence_path, &service->rules);
    if (rc != 0) {
        dppd_rule_observation_fault(&service->observation, rc);
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

static int control_reconcile_impl(struct dppd_control_service *service)
{
    uint32_t residual_objects;
    int rc;

    if (service == NULL)
        return -EINVAL;
    if (service->recovery_state == DPPD_CONTROL_RECOVERY_READY ||
        service->recovery_state == DPPD_CONTROL_RECOVERY_RESTART_REQUIRED)
        return -EALREADY;

    observe_stage(service, DPPD_RULE_STAGE_RECONCILE);
    rc = dppd_rte_flow_backend_reconcile(&service->rte_flow,
                                         &residual_objects);
    if (rc != 0 || residual_objects != 0) {
        dppd_rule_observation_fault(&service->observation, rc != 0 ? rc : -EUCLEAN);
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

static int control_apply_impl(struct dppd_control_service *service,
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
            service->observation.unchanged = true;
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
        if (dppd_rule_repository_count(&service->rules) >= service->rules.capacity)
            return -ENOSPC;
    }

    candidate.generation = dppd_rule_repository_generation(&service->rules) + 1U;
    planner_context.topology = service->topology;
    planner_context.install_port_id = install_port_id;
    planner_context.hardware_available = true;
    planner_context.software_equivalent =
        dppd_software_backend_rule_supported(&candidate);
    observe_rule(service, DPPD_RULE_STAGE_PLAN, &candidate, install_port_id, NULL);
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
    service->observation.context.transaction_id = result->transaction_id;
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
        service->observation.fallback_rules++;
        item.plan = result->plan;
        rc = dppd_transaction_init(&transaction, result->transaction_id,
                                   &item, 1);
        if (rc != 0)
            return rc;
        rc = dppd_transaction_run(&transaction, &backends);
    }
    if (rc != 0) {
        observe_transaction(service, &transaction);
        if (transaction.rollback_code != 0)
            return isolate_control(service, transaction.rollback_code);
        return rc;
    }

    if (updating) {
        int rollback_rc;

        /*
         * 新 generation 已创建后才删除旧对象；旧对象删除失败时撤销新对象，
         * repository 仍指向旧 generation，因此控制面不会发布半更新状态。
         */
        observe_rule(service, DPPD_RULE_STAGE_REMOVE, &existing, existing.install_port_id, NULL);
        rc = remove_actual_rule(service, existing.id, existing.generation);
        if (rc != 0) {
            dppd_rule_observation_fault(&service->observation, rc);
            rollback_rc = remove_actual_rule(service, candidate.id,
                                             candidate.generation);
            dppd_rule_observation_compensation(&service->observation, rollback_rc, candidate.id);
            return rollback_rc == 0 ? rc : isolate_control(service, rollback_rc);
        }
    }

    observe_rule(service, DPPD_RULE_STAGE_PUBLISH, &candidate, install_port_id, &result->plan);
    rc = dppd_rule_repository_apply(&service->rules, &candidate,
                                    updating ? existing.generation : expected_generation,
                                    &repository_result);
    if (rc != 0) {
        /* 单控制线程下不应到达这里；到达即表示 actual/desired 已失配。 */
        dppd_rule_observation_fault(&service->observation, rc);
        return isolate_control(service, rc);
    }
    service->observation.applied = true;
    result->status = repository_result.status;
    result->generation = repository_result.generation;
    rc = dppd_transaction_finalize(&transaction, &backends);
    if (rc != 0) {
        dppd_rule_observation_fault(&service->observation, rc);
        return isolate_control(service, rc);
    }
    return persist_current_repository(service);
}

/**
 * 一次创建一组原先不存在的规则，避免逐条调用单规则接口造成部分创建成功
 *
 * 每条输入都必须使用新 ID 和 expected_generation=0，不混入更新或删除语义
 * 先检查容量并逐条形成计划，必要时通过无副作用的驱动校验决定是否走软件后端
 * 新对象安装由同一事务完成，事务成功后才按请求顺序写入规则账本
 * 安装失败会尝试撤销本批对象，撤销也失败则进入恢复隔离，不能声称已经全部回滚
 */
static int control_create_batch_impl(
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

    observe_stage(service, DPPD_RULE_STAGE_PREPARE);
    items = calloc(request_count, sizeof(*items));
    if (items == NULL)
        return -ENOMEM;
    /**
     * 先记下当前全局版本，后续按输入顺序为实际对象预分配连续的新版本
     * 写入账本时必须保持相同顺序，才能让账本版本与已经安装对象的版本一一对应
     */
    base_generation = dppd_rule_repository_generation(&service->rules);

    /**
     * 逐条检查 ID、创建条件和规划结果，暂不安装对象或预留资源
     * PREFER 条目可能在本循环调用驱动 validate，这只是能力探测，不是安装
     * 后面的条目即使检查失败，也不应留下前面条目的实际规则
     */
    for (i = 0; i < request_count; ++i) {
        struct dppd_planner_context planner_context;
        struct dppd_rule existing;
        uint32_t previous;

        observe_rule(service, DPPD_RULE_STAGE_PREFLIGHT, &requests[i].rule,
                      requests[i].install_port_id, NULL);
        service->observation.context.generation = requests[i].expected_generation;
        /**
         * 新建必须明确声明旧版本为零，不接受 ANY 或具体旧版本
         * 如果允许混入覆盖已有规则的请求，失败时还要恢复旧对象，不能复用纯创建的回滚逻辑
         */
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
        /** 外层安装端口是本次请求的明确目标，覆盖规则内部字段以保持接口含义一致 */
        items[i].rule.install_port_id = requests[i].install_port_id;
        items[i].rule.generation = base_generation + i + 1U;
        memset(&planner_context, 0, sizeof(planner_context));
        planner_context.topology = service->topology;
        planner_context.install_port_id = requests[i].install_port_id;
        planner_context.hardware_available = true;
        planner_context.software_equivalent =
            dppd_software_backend_rule_supported(&items[i].rule);
        observe_rule(service, DPPD_RULE_STAGE_PLAN, &items[i].rule,
                      planner_context.install_port_id, NULL);
        rc = dppd_plan_rule(&planner_context, &items[i].rule, &items[i].plan);
        if (rc != 0)
            goto cleanup;

        /**
         * 在事务准备资源之前决定最终后端，这样一批规则可以同时包含硬件和软件条目
         * 例如 net_ring 的校验不支持 flow，但软件可表达相同规则时，只把这一条改为软件
         * 创建或回滚阶段的错误不允许走这个分支，防止掩盖已经产生的安装副作用
         */
        if (items[i].plan.backend == DPPD_PLAN_BACKEND_RTE_FLOW &&
            items[i].rule.fallback == DPPD_FALLBACK_PREFER_HARDWARE &&
            planner_context.software_equivalent) {
            struct dppd_flow_error flow_error;

            rc = dppd_rte_flow_backend_validate_rule(&service->rte_flow, items[i].plan.install_port_id,
                                                &items[i].rule, &flow_error);
            if (rc != 0) {
                planner_context.hardware_available = false;
                rc = dppd_plan_rule(&planner_context, &items[i].rule,
                                    &items[i].plan);
                if (rc != 0)
                    goto cleanup;
                service->observation.fallback_rules++;
            }
        }
    }

    memset(&backends, 0, sizeof(backends));
    backends.software = dppd_software_transaction_backend(&service->software);
    backends.rte_flow = dppd_rte_flow_transaction_backend(&service->rte_flow);
    transaction_id = service->next_transaction_id++;
    service->observation.context.transaction_id = transaction_id;
    rc = dppd_transaction_init(&transaction, transaction_id, items, request_count);
    if (rc != 0)
        goto cleanup;
    /** 先让本批所有实际对象安装完成，再在同一控制线程中连续发布对应的账本记录 */
    rc = dppd_transaction_run(&transaction, &backends);
    if (rc != 0) {
        observe_transaction(service, &transaction);
        /** 撤销也失败时可能仍有残留对象，记录恢复错误并封锁后续普通写入 */
        if (transaction.rollback_code != 0) {
            service->recovery_state =
                DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED;
            service->recovery_last_error = transaction.rollback_code;
            rc = -EUCLEAN;
        }
        goto cleanup;
    }

    /**
     * 全批检查和串行执行保证这里应当只产生 CREATED，且版本与预分配值一致
     * 如果仍出现异常，撤回已经写入的本批记录并尝试回滚实际对象，统一返回 EUCLEAN
     * 结果数组只有整个函数成功时才可作为成功回执，不能只看其中已填好的前几项
     */
    for (i = 0; i < request_count; ++i) {
        struct dppd_rule_apply_result repository_result;

        observe_rule(service, DPPD_RULE_STAGE_PUBLISH, &items[i].rule,
                      items[i].plan.install_port_id, &items[i].plan);
        rc = dppd_rule_repository_apply(&service->rules, &items[i].rule, 0,
                                        &repository_result);
        if (rc != 0 || repository_result.status != DPPD_RULE_CREATED ||
            repository_result.generation != items[i].rule.generation) {
            int rollback_rc;

            dppd_rule_observation_fault(&service->observation, rc != 0 ? rc : -EUCLEAN);
            /**
             * published_count 只记录当前批次已经写入账本的条数，逆序移除这部分记录
             * 删除记录仍会推进全局版本，所以撤回记录不等于把历史版本号恢复到原值
             * 任一账本撤回或实际对象回滚失败，都要进入恢复隔离
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
            observe_transaction(service, &transaction);
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
    service->observation.applied = true;
    /** 实际规则和账本均已写好，不再需要本次回滚凭据，先结束事务临时资源再保存快照 */
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
 * 一次删除一组已经存在且版本精确匹配的规则
 * 先保存原规则和实际后端，再删除全部实际对象，最后才从账本中连续移除记录
 * 如果实际删除到一半失败，账本仍保留全部旧记录，可按原版本重建已经删掉的对象
 * 通用事务接口描述的是对象创建，因此这里使用显式删除循环，并用创建事务完成补偿
 */
static int control_remove_batch_impl(
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
    observe_stage(service, DPPD_RULE_STAGE_PREPARE);
    items = calloc(request_count, sizeof(*items));
    if (items == NULL)
        return -ENOMEM;

    /**
     * 删除任何实际对象前先检查整批：旧版本必须精确、ID 不重复、对应对象只存在于一个后端
     * 同时把规则和后端归属保存下来，后续即使对象已删除也知道应该怎样重建
     */
    for (i = 0; i < request_count; ++i) {
        uint32_t previous;
        struct dppd_rule target = {.id = requests[i].rule_id,
                                   .generation = requests[i].expected_generation};

        observe_rule(service, DPPD_RULE_STAGE_PREFLIGHT, &target, 0, NULL);
        service->observation.context.port_known = false;
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

    /** removed_actual 只在删除成功后增加，失败时只恢复前面确实已经删掉的对象 */
    for (i = 0; i < request_count; ++i) {
        observe_rule(service, DPPD_RULE_STAGE_REMOVE, &items[i].rule,
                      items[i].rule.install_port_id, &items[i].plan);
        rc = remove_actual_rule(service, items[i].rule.id, items[i].rule.generation);
        if (rc != 0) {
            int restore_rc;

            dppd_rule_observation_fault(&service->observation, rc);
            restore_rc = restore_actual_rules(service, items, removed_actual);

            /**
             * 账本还没修改，补偿成功后实际对象又与旧账本一致，可以返回最初的删除错误
             * 补偿也失败时已经不能证明一致性，改为返回 EUCLEAN 并记录具体恢复错误
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

    /**
     * 实际对象全部删除后才从账本移除记录，每次删除都会推进全局版本
     * 精确预检和串行调用保证正常情况下不会失败；若仍失败，不能简单重新创建来冒充旧版本
     * 因为部分账本记录也可能已经消失，此时只能进入隔离，由恢复流程统一处理
     */
    for (i = 0; i < request_count; ++i) {
        bool removed;
        uint64_t generation;

        observe_rule(service, DPPD_RULE_STAGE_PUBLISH, &items[i].rule,
                      items[i].rule.install_port_id, &items[i].plan);
        rc = dppd_rule_repository_remove(&service->rules, items[i].rule.id,
                                         items[i].rule.generation, &removed,
                                         &generation);
        if (rc != 0 || !removed) {
            dppd_rule_observation_fault(&service->observation, rc != 0 ? rc : -EUCLEAN);
            service->recovery_state = DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED;
            service->recovery_last_error = rc != 0 ? rc : -EUCLEAN;
            rc = -EUCLEAN;
            goto cleanup;
        }
        results[i].rule_id = items[i].rule.id;
        results[i].generation = generation;
    }
    service->observation.applied = true;
    rc = persist_current_repository(service);

cleanup:
    free(items);
    return rc;
}

/**
 * 把一组已有规则作为同一笔更新处理，避免调用方看到只更新了一半的规则清单
 *
 * 可以把 repository 理解为“系统希望保留的规则账本”，把 actual 对象理解为
 * “硬件或软件中已经安装的规则”。只有新规则全部装好、旧规则全部删完之后
 * 才能整批改写账本。此前任何一步失败，都优先撤销新规则并恢复已删除的旧规则
 *
 * 本函数由管理主线程串行调用，旧对象与新计划全为软件时整批只发布一次快照
 * 涉及硬件时仍采用逐条安装与补偿流程，不保证所有报文在同一时刻切换规则
 * 返回 EUCLEAN 时要进一步区分恢复隔离和保存失败，不能直接认定本次更新没有生效
 */
static int control_update_batch_impl(
    struct dppd_control_service *service,
    const struct dppd_control_batch_update_request *requests,
    uint32_t request_count,
    struct dppd_control_apply_result *results)
{
    /**
     * items 保存待安装的新版本，old 保存失败补偿所需的旧规则及原后端类型
     * candidates 是交给规则账本的完整新内容，expected 是用户读取过的精确旧版本
     * 这些数组使用相同下标，保证请求、安装计划和返回结果始终一一对应
     */
    struct dppd_transaction_item items[DPPD_CONTROL_BATCH_UPDATE_MAX] = {0};
    struct dppd_transaction_item old[DPPD_CONTROL_BATCH_UPDATE_MAX] = {0};
    struct dppd_rule candidates[DPPD_CONTROL_BATCH_UPDATE_MAX];
    uint64_t expected[DPPD_CONTROL_BATCH_UPDATE_MAX];
    bool may_fallback[DPPD_CONTROL_BATCH_UPDATE_MAX] = {false};
    bool all_old_software = true, atomic_software;
    struct dppd_transaction transaction;
    struct dppd_transaction_backends backends = {0};
    uint32_t hardware_needed = 0, software_needed = 0;
    uint32_t hardware_count, software_count;
    uint64_t base_generation;
    uint32_t i;
    int rc;

    if (service == NULL || service->topology == NULL || requests == NULL ||
        results == NULL || request_count < 2 ||
        request_count > DPPD_CONTROL_BATCH_UPDATE_MAX)
        return -EINVAL;
    memset(results, 0, request_count * sizeof(*results));
    /**
     * 已在恢复隔离中时禁止继续写入；上次保存失败时先把已生效状态重新写入磁盘
     * 两个检查都通过后才开始本次更新，防止在已有不一致上继续叠加新修改
     */
    rc = recovery_write_preflight(service);
    if (rc != 0)
        return rc;
    rc = persistence_write_preflight(service);
    if (rc != 0)
        return rc;
    base_generation = dppd_rule_repository_generation(&service->rules);
    /**
     * 每条规则需要一个连续新版本，版本号不能绕回零或占用 ANY 的特殊值
     * 事务编号也要保留有效空间，以便后续安装和失败补偿能够被明确识别
     */
    if (base_generation >= UINT64_MAX - request_count ||
        service->next_transaction_id == 0 ||
        service->next_transaction_id == UINT64_MAX)
        return -EOVERFLOW;

    /**
     * 第一轮只做本地检查，不调用网卡驱动 PMD
     * 例如第二条规则版本过期时，应立即拒绝整批，而不是先为第一条接触硬件
     * 0 和 ANY 都不代表用户明确确认过的旧版本，因此批量更新不接受这两个值
     */
    for (i = 0; i < request_count; ++i) {
        struct dppd_planner_context context = {0};
        char validation_error[128];
        uint32_t j;

        observe_rule(service, DPPD_RULE_STAGE_PREFLIGHT, &requests[i].rule,
                      requests[i].install_port_id, NULL);
        service->observation.context.generation = requests[i].expected_generation;
        if (requests[i].reserved != 0 || requests[i].rule.id == 0)
            return -EINVAL;
        if (requests[i].expected_generation == 0 ||
            requests[i].expected_generation == DPPD_RULE_GENERATION_ANY)
            return -ESTALE;
        for (j = 0; j < i; ++j) {
            if (requests[j].rule.id == requests[i].rule.id)
                return -EEXIST;
        }
        rc = dppd_rule_repository_get(&service->rules, requests[i].rule.id,
                                      &old[i].rule);
        if (rc != 0)
            return rc;
        if (old[i].rule.generation != requests[i].expected_generation)
            return -ESTALE;
        /**
         * 旧规则必须恰好存在于一个实际后端中，不能仅根据“优先硬件”策略猜归属
         * 同时存在于两个后端或两个后端都不存在，说明账本与安装状态已经失配
         * 此时进入隔离，不能继续更新并掩盖原来的问题
         */
        rc = actual_backend_for(service, old[i].rule.id, old[i].rule.generation,
                                &old[i].plan.backend);
        if (rc != 0)
            goto isolate;

        expected[i] = old[i].rule.generation;
        if (old[i].plan.backend != DPPD_PLAN_BACKEND_SOFTWARE)
            all_old_software = false;
        candidates[i] = requests[i].rule;
        candidates[i].install_port_id = requests[i].install_port_id;
        candidates[i].generation = base_generation + i + 1U;
        items[i].rule = candidates[i];
        /**
         * 匹配条件和动作来自请求，必须先检查数组长度和规则结构
         * 后面的软件能力判断会遍历数组，先校验可以避免非法长度导致越界读取
         */
        if (dppd_rule_validate(&candidates[i], validation_error,
                               sizeof(validation_error)) != 0)
            return -EINVAL;
        context.topology = service->topology;
        context.install_port_id = requests[i].install_port_id;
        context.hardware_available = true;
        context.software_equivalent =
            dppd_software_backend_rule_supported(&candidates[i]);
        observe_rule(service, DPPD_RULE_STAGE_PLAN, &items[i].rule,
                      context.install_port_id, NULL);
        rc = dppd_plan_rule(&context, &items[i].rule, &items[i].plan);
        if (rc != 0)
            return rc;
        may_fallback[i] = items[i].plan.backend == DPPD_PLAN_BACKEND_RTE_FLOW &&
            candidates[i].fallback == DPPD_FALLBACK_PREFER_HARDWARE &&
            context.software_equivalent;
        if (items[i].plan.backend == DPPD_PLAN_BACKEND_RTE_FLOW)
            hardware_needed++;
        if (items[i].plan.backend == DPPD_PLAN_BACKEND_SOFTWARE || may_fallback[i])
            software_needed++;
    }

    /**
     * 硬件及混合路径需要新旧版本暂时共存，因此预检临时空间
     * 旧对象全为软件且所有目标都可能走软件时，允许稍后确认能否整表替换
     * 如果最终规划为混合路径，仍须在安装之前复查软件临时空间
     * 容量不足不触发降级，硬件目标的本地容量仍在 PMD 校验之前保守检查
     */
    hardware_count = dppd_rte_flow_backend_count(&service->rte_flow);
    software_count = dppd_software_backend_count(&service->software);
    observe_batch(service, DPPD_RULE_STAGE_PREFLIGHT);
    if (hardware_count > service->rte_flow.capacity ||
        software_count > service->software.capacity) {
        rc = -EUCLEAN;
        goto isolate;
    }
    if (hardware_needed > service->rte_flow.capacity - hardware_count ||
        (software_needed > service->software.capacity - software_count &&
         !(all_old_software && software_needed == request_count)))
        return -ENOSPC;

    /**
     * 到这里整批输入和容量都已通过检查，才允许询问 PMD 是否支持这些规则
     * 只有尚未安装对象的 validate 失败，并且软件能表达相同语义时，才改走软件
     * 后面的创建、删除或回滚失败都不能再用软件回退来伪装成成功
     */
    for (i = 0; i < request_count; ++i) {
        if (may_fallback[i]) {
            struct dppd_flow_error error;

            rc = dppd_rte_flow_backend_validate_rule(&service->rte_flow, items[i].plan.install_port_id,
                                                &items[i].rule, &error);
            if (rc != 0) {
                struct dppd_planner_context context = {0};

                context.topology = service->topology;
                context.install_port_id = requests[i].install_port_id;
                context.software_equivalent = true;
                rc = dppd_plan_rule(&context, &items[i].rule, &items[i].plan);
                if (rc != 0)
                    return rc;
                service->observation.fallback_rules++;
            }
        }
    }
    /**
     * 确认最终计划是否允许整批软件替换，否则按实际软件目标数复查临时空间
     * 纯软件路径复用旧槽位，硬件或混合路径继续由事务逐条安装与补偿
     */
    atomic_software = all_old_software;
    software_needed = 0;
    for (i = 0; i < request_count; ++i) {
        if (items[i].plan.backend == DPPD_PLAN_BACKEND_SOFTWARE)
            software_needed++;
        else
            atomic_software = false;
    }
    if (!atomic_software && software_needed > service->software.capacity - software_count)
        return -ENOSPC;
    backends.software = dppd_software_transaction_backend(&service->software);
    backends.rte_flow = dppd_rte_flow_transaction_backend(&service->rte_flow);
    rc = dppd_transaction_init(&transaction, service->next_transaction_id++,
                               items, request_count);
    if (rc != 0)
        return rc;
    if (atomic_software) {
        /** 整批软件更新在一个函数里分配并发布，无法定位成员时明确报告批次而不猜 ID */
        observe_stage(service, DPPD_RULE_STAGE_COMMIT);
        service->observation.context.rule_id = 0;
        service->observation.context.generation = 0;
        service->observation.context.port_known = false;
        service->observation.context.backend = DPPD_PLAN_BACKEND_SOFTWARE;
        service->observation.context.backend_known = true;
        service->observation.context.transaction_id = transaction.id;
        rc = dppd_software_backend_update_batch(&service->software, candidates,
                                                expected, request_count);
        if (rc != 0)
            return rc;
        observe_stage(service, DPPD_RULE_STAGE_PUBLISH);
        rc = dppd_rule_repository_update_batch(&service->rules, candidates,
                                               expected, request_count);
        if (rc != 0)
            goto isolate;
        goto committed;
    }
    rc = dppd_transaction_run(&transaction, &backends);
    if (rc != 0) {
        observe_transaction(service, &transaction);
        if (transaction.rollback_code == 0)
            return rc;
        rc = transaction.rollback_code;
        goto isolate;
    }

    /**
     * 全部新对象安装成功后再逐条删除旧对象，这时规则账本仍完整保留旧版本
     * 删除下标 i 失败，意味着只有前 i 条旧对象已经删除，补偿只恢复这部分
     */
    for (i = 0; i < request_count; ++i) {
        observe_rule(service, DPPD_RULE_STAGE_REMOVE, &old[i].rule,
                      old[i].rule.install_port_id, &old[i].plan);
        service->observation.context.transaction_id = transaction.id;
        rc = remove_actual_rule(service, old[i].rule.id, old[i].rule.generation);
        if (rc != 0) {
            int rollback_rc;
            int restore_rc;

            dppd_rule_observation_fault(&service->observation, rc);
            /**
             * 先撤销本批全部新版本，再按原规则、原版本和原后端恢复已删除的旧对象
             * 即使撤新失败也继续尝试恢复旧对象，以尽量减少缺失的规则
             * 两种补偿都成功才返回原始删除错误，否则返回 EUCLEAN 并封锁后续更新
             */
            rollback_rc = dppd_transaction_rollback_committed(&transaction, &backends);
            observe_transaction(service, &transaction);
            restore_rc = restore_actual_rules(service, old, i);
            if (rollback_rc == 0 && restore_rc == 0)
                return rc;
            rc = rollback_rc != 0 ? rollback_rc : restore_rc;
            goto isolate;
        }
    }

    /**
     * 新旧对象的切换全部成功后，最后整批改写账本
     * 仓库接口会先复查所有条件，再执行不分配内存的替换循环
     * 内容没有变化的规则也获得新版本，这样整批结果始终使用连续的版本号
     */
    observe_batch(service, DPPD_RULE_STAGE_PUBLISH);
    rc = dppd_rule_repository_update_batch(&service->rules, candidates,
                                           expected, request_count);
    if (rc != 0) {
        /**
         * 正常串行流程下不应失败；如果复查仍失败，旧实际对象已经被删除
         * 此时不能靠重新 apply 冒充恢复旧版本，只释放事务临时资源并进入隔离
         */
        (void)dppd_transaction_finalize(&transaction, &backends);
        goto isolate;
    }
    /** 账本已经完整发布，之后的临时资源收尾错误也必须如实报告为已经生效 */
    service->observation.applied = true;
    rc = dppd_transaction_finalize(&transaction, &backends);
    if (rc != 0)
        goto isolate;
committed:
    service->observation.applied = true;
    for (i = 0; i < request_count; ++i) {
        results[i].status = DPPD_RULE_UPDATED;
        results[i].generation = candidates[i].generation;
        results[i].transaction_id = transaction.id;
        results[i].plan = items[i].plan;
    }
    /**
     * 保存失败时整批更新已经在内存和实际后端生效，不能再声称已经回滚
     * 保存函数会设置 dirty 状态并返回 EUCLEAN，后续写入要先修复持久化状态
     */
    return persist_current_repository(service);

isolate:
    /**
     * 隔离表示“已经无法证明账本和实际规则一致”，普通管理请求将被拒绝
     * 主循环随后停止软件工作线程，只保留恢复查询和清理重试入口
     * 底层错误另存于 recovery_last_error，便于区分创建、删除或补偿失败
     */
    dppd_rule_observation_fault(&service->observation, rc);
    service->recovery_state = DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED;
    service->recovery_last_error = rc;
    return -EUCLEAN;
}

static int control_remove_impl(struct dppd_control_service *service,
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
    if (rc == -ENOENT) {
        rc = dppd_rule_repository_remove(&service->rules, rule_id,
                                           expected_generation, removed,
                                           generation);
        service->observation.unchanged = rc == 0;
        return rc;
    }
    if (rc != 0)
        return rc;
    if (expected_generation != DPPD_RULE_GENERATION_ANY &&
        expected_generation != existing.generation)
        return -ESTALE;
    observe_rule(service, DPPD_RULE_STAGE_REMOVE, &existing, existing.install_port_id, NULL);
    rc = remove_actual_rule(service, rule_id, existing.generation);
    if (rc != 0)
        return rc;
    observe_stage(service, DPPD_RULE_STAGE_PUBLISH);
    rc = dppd_rule_repository_remove(&service->rules, rule_id,
                                      existing.generation, removed, generation);
    if (rc != 0 || !*removed) {
        dppd_rule_observation_fault(&service->observation, rc != 0 ? rc : -EUCLEAN);
        return isolate_control(service, rc != 0 ? rc : -EUCLEAN);
    }
    service->observation.applied = true;
    return persist_current_repository(service);
}

/**
 * 先以规则账本确定当前版本，再读取两个后端的安装记录，不能仅凭 prefer 策略猜测实际位置
 * 缺失或重复安装都属于账本与实际对象失配，保留原状态并报告错误，由已有恢复流程处理
 */
int dppd_control_rule_status(const struct dppd_control_service *service,
                              uint64_t rule_id, uint64_t expected_generation,
                              struct dppd_control_rule_status *result)
{
    struct dppd_rule_install_info hardware, software;
    const struct dppd_rule_install_info *installed;
    struct dppd_rule rule;
    bool has_count = false;
    uint16_t action;
    int hardware_rc, software_rc, rc;

    if (service == NULL || rule_id == 0 || expected_generation == 0 || result == NULL)
        return -EINVAL;
    memset(result, 0, sizeof(*result));
    if (recovery_write_preflight(service) != 0)
        return -EUCLEAN;
    rc = dppd_rule_repository_get(&service->rules, rule_id, &rule);
    if (rc != 0)
        return rc;
    if (expected_generation != DPPD_RULE_GENERATION_ANY &&
        expected_generation != rule.generation)
        return -ESTALE;

    /** 这两个入口只读取安装记录，不借用会顺带回收旧快照的 contains 或 COUNT 查询 */
    hardware_rc = dppd_rte_flow_backend_install_info(
        &service->rte_flow, rule.id, rule.generation, &hardware);
    software_rc = dppd_software_backend_install_info(
        &service->software, rule.id, rule.generation, &software);
    if ((hardware_rc != 0 && hardware_rc != -ENOENT) ||
        (software_rc != 0 && software_rc != -ENOENT) ||
        (hardware_rc == 0) == (software_rc == 0))
        return -EUCLEAN;
    installed = hardware_rc == 0 ? &hardware : &software;
    for (action = 0; action < rule.nb_actions; ++action) {
        if (rule.actions[action].type == DPPD_ACTION_COUNT)
            has_count = true;
    }
    if (installed->rule_id != rule.id || installed->generation != rule.generation ||
        installed->install_port_id != rule.install_port_id ||
        installed->has_count != has_count || installed->commit_rule_count == 0 ||
        (hardware_rc == 0 && rule.fallback == DPPD_FALLBACK_SOFTWARE_ONLY) ||
        (software_rc == 0 && rule.fallback == DPPD_FALLBACK_REQUIRE_HARDWARE))
        return -EUCLEAN;

    result->installation = *installed;
    result->backend = hardware_rc == 0 ? DPPD_PLAN_BACKEND_RTE_FLOW :
                                        DPPD_PLAN_BACKEND_SOFTWARE;
    result->fallback = rule.fallback;
    dppd_control_persistence_status(service, &result->persistence);
    return 0;
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

/** 公开入口统一开始记录，空或已释放的 service 不参与指标，非法请求仍保留原返回值 */
static bool begin_observed(struct dppd_control_service *service,
    enum dppd_rule_operation operation, uint64_t rule_id, uint64_t generation,
    uint32_t count, bool port_known, uint16_t port_id)
{
    struct dppd_rule_failure_event *context;

    if (service == NULL || service->rules.records == NULL)
        return false;
    dppd_rule_observation_begin(&service->observation, operation);
    context = &service->observation.context;
    context->rule_id = rule_id;
    context->generation = generation;
    context->rule_count = count;
    context->install_port_id = port_id;
    context->port_known = port_known;
    if (operation != DPPD_RULE_OPERATION_RECONCILE &&
        service->recovery_state != DPPD_CONTROL_RECOVERY_READY)
        context->stage = DPPD_RULE_STAGE_ISOLATION;
    return true;
}

/** 只在整笔公开操作结束时更新累计值，内部成员和补偿事务不重复增加请求计数 */
static int finish_observed(struct dppd_control_service *service, bool observed, int result)
{
    if (observed)
        dppd_rule_observation_finish(&service->observation, result);
    return result;
}

/** 单条 apply 的计数涵盖新建、更新和幂等重放，不改变内部事务的成功条件 */
int dppd_control_apply(struct dppd_control_service *service, uint16_t port_id,
    const struct dppd_rule *rule, uint64_t expected, struct dppd_control_apply_result *result)
{
    bool observed = begin_observed(service, DPPD_RULE_OPERATION_APPLY,
        rule != NULL ? rule->id : 0, expected, 1, true, port_id);
    int rc = control_apply_impl(service, port_id, rule, expected, result);

    return finish_observed(service, observed, rc);
}

/** 批量整体失败时 ID 可为零，逐条检查和事务会在能够定位成员时补齐准确身份 */
int dppd_control_create_batch(struct dppd_control_service *service,
    const struct dppd_control_batch_create_request *requests, uint32_t count,
    struct dppd_control_apply_result *results)
{
    bool observed = begin_observed(service, DPPD_RULE_OPERATION_CREATE_BATCH, 0, 0, count, false, 0);
    int rc = control_create_batch_impl(service, requests, count, results);

    return finish_observed(service, observed, rc);
}

/** 纯软件整批发布和硬件逐条事务共同按一次更新请求计数 */
int dppd_control_update_batch(struct dppd_control_service *service,
    const struct dppd_control_batch_update_request *requests, uint32_t count,
    struct dppd_control_apply_result *results)
{
    bool observed = begin_observed(service, DPPD_RULE_OPERATION_UPDATE_BATCH, 0, 0, count, false, 0);
    int rc = control_update_batch_impl(service, requests, count, results);

    return finish_observed(service, observed, rc);
}

/** 删除不分配新的规则版本来补偿，观测层同样不参与对象或版本的分配 */
int dppd_control_remove(struct dppd_control_service *service, uint64_t id, uint64_t expected,
    bool *removed, uint64_t *generation)
{
    bool observed = begin_observed(service, DPPD_RULE_OPERATION_REMOVE, id, expected, 1, false, 0);
    int rc = control_remove_impl(service, id, expected, removed, generation);

    return finish_observed(service, observed, rc);
}

/** 整批删除只增加一笔请求，恢复旧对象的创建事务保留为补偿信息 */
int dppd_control_remove_batch(struct dppd_control_service *service,
    const struct dppd_control_batch_remove_request *requests, uint32_t count,
    struct dppd_control_batch_remove_result *results)
{
    bool observed = begin_observed(service, DPPD_RULE_OPERATION_REMOVE_BATCH, 0, 0, count, false, 0);
    int rc = control_remove_batch_impl(service, requests, count, results);

    return finish_observed(service, observed, rc);
}

/** 启动重放算一笔操作，缺文件时建立空快照的内部保存不会再增加一笔 flush */
int dppd_control_persistence_restore(struct dppd_control_service *service, const char *path)
{
    bool observed = begin_observed(service, DPPD_RULE_OPERATION_RESTORE, 0, 0, 0, false, 0);
    int rc = control_restore_impl(service, path);

    return finish_observed(service, observed, rc);
}

/** 显式保存本身属于可失败请求，退出清理中的内部保存不计入公开请求 */
int dppd_control_persistence_flush(struct dppd_control_service *service)
{
    bool observed = begin_observed(service, DPPD_RULE_OPERATION_FLUSH, 0, 0, 0, false, 0);
    int rc = control_flush_impl(service);

    return finish_observed(service, observed, rc);
}

/** 清理重试单独记录底层错误，成功重试仍不会清掉导致隔离的历史失败 */
int dppd_control_reconciliation_retry(struct dppd_control_service *service)
{
    bool observed = begin_observed(service, DPPD_RULE_OPERATION_RECONCILE, 0, 0, 0, false, 0);
    int rc = control_reconcile_impl(service);

    return finish_observed(service, observed, rc);
}

/** 管理线程直接复制完成态指标，telemetry 线程只能读取另行发布的值快照 */
void dppd_control_rule_metrics(const struct dppd_control_service *service,
                                struct dppd_rule_metrics *metrics)
{
    if (metrics == NULL)
        return;
    memset(metrics, 0, sizeof(*metrics));
    if (service != NULL)
        *metrics = service->observation.metrics;
}
