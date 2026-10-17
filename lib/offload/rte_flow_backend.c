#include "dppd/rte_flow_backend.h"
#include "dppd/install_time.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct dppd_shared_count_object {
    bool used, installed;
    struct dppd_shared_count_spec spec;
    struct dppd_shared_count_handle handle;
    uint32_t references;
};

/**
 * 硬件对象仓库中的一个槽位，既可以表示准备阶段的预留，也可以表示已安装规则
 * occupied 与 installed 分开记录，才能分清“只占了位置”和“已经需要调用驱动删除”
 */
struct dppd_rte_flow_object {
    /** 槽位已经被某次事务占用，尚未归还 */
    bool occupied;
    /** 驱动的创建调用已经成功，实际对象由 handle 保存 */
    bool installed;
    /** 从准备阶段保留 AGE 意图，查询时核对 handle，不能靠驱动回填的标志补猜 */
    bool has_age;
    uint64_t rule_id;
    uint64_t generation;
    /** 精确关联本次驱动调用的磁盘记录，业务 ID 和版本相同的补偿重建也不复用 */
    uint64_t recovery_attempt;
    uint8_t owner_cookie[DPPD_TAP_COOKIE_SIZE];
    struct dppd_flow_handle handle;
    /** 在 prepare 时保留安装意图，查询时据此核对驱动返回的 handle */
    struct dppd_rule_install_info installation;
    /** 准备时借用，直到这个规则槽位真正释放才归还，回滚失败也继续保留 */
    struct dppd_shared_count_object *shared_refs[DPPD_RULE_MAX_ACTIONS];
    struct dppd_flow_binding binding;
};

static int shared_error(struct dppd_flow_error *error, int rc, const char *message)
{
    if (error != NULL) {
        error->code = rc;
        snprintf(error->message, sizeof(error->message), "shared count: %s", message);
    }
    return rc;
}

static bool shared_spec_equal(const struct dppd_shared_count_spec *left, const struct dppd_shared_count_spec *right)
{
    return left->id == right->id && left->generation == right->generation &&
        left->install_port_id == right->install_port_id && left->domain == right->domain;
}

static struct dppd_shared_count_object *find_shared(const struct dppd_rte_flow_backend *backend, uint64_t id)
{
    if (backend == NULL || backend->shared_counts == NULL)
        return NULL;
    for (uint32_t i = 0; i < DPPD_SHARED_COUNT_MAX; ++i)
        if (backend->shared_counts[i].used && backend->shared_counts[i].spec.id == id)
            return &backend->shared_counts[i];
    return NULL;
}

static int lookup_shared(const struct dppd_rte_flow_backend *backend, uint64_t id, uint64_t expected,
    struct dppd_shared_count_object **object)
{
    if (backend == NULL || backend->shared_counts == NULL || id == 0 || id == UINT64_MAX || expected == 0)
        return -EINVAL;
    struct dppd_shared_count_object *found = find_shared(backend, id);
    if (found == NULL)
        return -ENOENT;
    if (expected != UINT64_MAX && expected != found->spec.generation)
        return -ESTALE;
    *object = found;
    return 0;
}

/** 删除失败仍保存原句柄，驱动即使在报错前改写参数也不能破坏后续清理线索 */
static int destroy_shared(struct dppd_rte_flow_backend *backend, struct dppd_shared_count_object *object,
    struct dppd_flow_error *error)
{
    if (object->references != 0)
        return shared_error(error, -EBUSY, "object is still referenced by prepared or installed rules");
    if (object->handle.opaque == NULL)
        return shared_error(error, -EUCLEAN, "uncertain object has no handle for cleanup");
    if (backend->api.shared_count_destroy == NULL)
        return shared_error(error, -ENOTSUP, "driver does not provide indirect destroy");
    struct dppd_shared_count_handle next = object->handle;
    dppd_flow_probe_cache_invalidate_all(backend->probes);
    int rc = backend->api.shared_count_destroy(&next, error);
    if (rc == 0) {
        memset(object, 0, sizeof(*object));
        --backend->shared_count_count;
    }
    return rc;
}

/** 规则引用先释放，共享计数器后删除，某个对象失败也继续清理其他无依赖对象 */
static int cleanup_shared(struct dppd_rte_flow_backend *backend)
{
    int first_error = 0;
    for (uint32_t i = 0; i < DPPD_SHARED_COUNT_MAX; ++i) {
        if (!backend->shared_counts[i].used)
            continue;
        int rc = destroy_shared(backend, &backend->shared_counts[i], NULL);
        if (first_error == 0)
            first_error = rc;
    }
    return first_error;
}

static bool has_shared(const struct dppd_rule *rule)
{
    for (uint16_t i = 0; i < rule->nb_actions; ++i)
        if (rule->actions[i].type == DPPD_ACTION_SHARED_COUNT)
            return true;
    return false;
}

/** 只解析精确身份，不增加引用，不为了校验规则临时创建共享对象 */
static int resolve_shared(const struct dppd_rte_flow_backend *backend, uint16_t port_id, const struct dppd_rule *rule,
    struct dppd_flow_binding *binding, struct dppd_shared_count_object **references, struct dppd_flow_error *error)
{
    memset(binding, 0, sizeof(*binding));
    if (rule->nb_actions > DPPD_RULE_MAX_ACTIONS)
        return shared_error(error, -EINVAL, "invalid action count");
    for (uint16_t i = 0; i < rule->nb_actions; ++i) {
        if (references != NULL)
            references[i] = NULL;
        const struct dppd_action *action = &rule->actions[i];
        if (action->type != DPPD_ACTION_SHARED_COUNT)
            continue;
        if (backend->before_create != NULL)
            return shared_error(error, -ENOTSUP, "native recovery journal cannot track indirect objects");
        struct dppd_shared_count_object *shared = find_shared(backend, action->conf.shared_count.id);
        if (shared == NULL)
            return shared_error(error, -ENOENT, "referenced object does not exist");
        if (shared->spec.generation != action->conf.shared_count.generation)
            return shared_error(error, -ESTALE, "referenced generation changed");
        if (!shared->installed || shared->handle.opaque == NULL ||
            !shared_spec_equal(&shared->spec, &shared->handle.spec))
            return shared_error(error, -EUCLEAN, "referenced handle is uncertain");
        if (shared->spec.install_port_id != port_id || shared->spec.domain != rule->domain)
            return shared_error(error, -EXDEV, "object port or direction does not match the rule");
        binding->actions[i] = shared->handle.opaque;
        if (references != NULL)
            references[i] = shared;
    }
    return 0;
}

/** 按规则 ID 和精确版本查找已占用槽位，允许找到尚未安装的 prepare 预留项 */
static struct dppd_rte_flow_object *find_object(struct dppd_rte_flow_backend *backend, uint64_t rule_id,
    uint64_t generation)
{
    uint32_t i;

    for (i = 0; i < backend->capacity; ++i) {
        if (backend->objects[i].occupied && backend->objects[i].rule_id == rule_id &&
            backend->objects[i].generation == generation)
            return &backend->objects[i];
    }
    return NULL;
}

/** 提供只读的精确版本查找，是否已经安装由调用方进一步检查 */
static const struct dppd_rte_flow_object *find_object_const(const struct dppd_rte_flow_backend *backend,
    uint64_t rule_id, uint64_t generation)
{
    uint32_t i;

    for (i = 0; i < backend->capacity; ++i) {
        if (backend->objects[i].occupied && backend->objects[i].rule_id == rule_id &&
            backend->objects[i].generation == generation)
            return &backend->objects[i];
    }
    return NULL;
}

/**
 * 从同一个 ID 的已占用槽位中找出版本号最大的对象，不区分预留还是已安装
 * 调用方不能把“最新槽位”直接当成可用硬件规则，仍要检查 installed
 */
static struct dppd_rte_flow_object *find_latest_object(struct dppd_rte_flow_backend *backend, uint64_t rule_id)
{
    struct dppd_rte_flow_object *latest = NULL;
    uint32_t i;

    for (i = 0; i < backend->capacity; ++i) {
        struct dppd_rte_flow_object *object = &backend->objects[i];

        if (!object->occupied || object->rule_id != rule_id)
            continue;
        if (latest == NULL || object->generation > latest->generation)
            latest = object;
    }
    return latest;
}

/** 只读查找最大版本槽位，用于不修改对象内容的查询入口 */
static const struct dppd_rte_flow_object *find_latest_object_const(const struct dppd_rte_flow_backend *backend,
    uint64_t rule_id)
{
    const struct dppd_rte_flow_object *latest = NULL;
    uint32_t i;

    for (i = 0; i < backend->capacity; ++i) {
        const struct dppd_rte_flow_object *object = &backend->objects[i];

        if (!object->occupied || object->rule_id != rule_id)
            continue;
        if (latest == NULL || object->generation > latest->generation)
            latest = object;
    }
    return latest;
}

/**
 * 为一条规则预留仓库槽位，此时只记录 ID 和版本，还没有调用网卡安装接口
 * count 在预留时就增加，因此它表示已占用槽位总数，而不是单纯的成功安装数量
 */
static struct dppd_rte_flow_object *reserve_object(struct dppd_rte_flow_backend *backend, uint64_t rule_id,
    uint64_t generation)
{
    uint32_t i;

    /** 同一 ID 的相同版本不能重复占用，不同版本则允许共存，以支持先建新规则再删旧规则 */
    if (find_object(backend, rule_id, generation) != NULL)
        return NULL;
    for (i = 0; i < backend->capacity; ++i) {
        if (!backend->objects[i].occupied) {
            backend->objects[i].occupied = true;
            backend->objects[i].rule_id = rule_id;
            backend->objects[i].generation = generation;
            backend->count++;
            return &backend->objects[i];
        }
    }
    return NULL;
}

/**
 * 只归还本地仓库槽位，不负责删除硬件规则
 * 调用方必须先确认无需硬件清理或驱动删除已经成功，否则清空 handle 会丢失重试线索
 */
static void release_object(struct dppd_rte_flow_backend *backend, struct dppd_rte_flow_object *object)
{
    for (uint16_t i = 0; i < DPPD_RULE_MAX_ACTIONS; ++i)
        if (object->shared_refs[i] != NULL && object->shared_refs[i]->references != 0)
            --object->shared_refs[i]->references;
    memset(object, 0, sizeof(*object));
    backend->count--;
}

/** 删除尝试可能影响共享资源，先清空所有探测结果，再保留原有驱动删除和失败重试语义 */
static int remove_object(struct dppd_rte_flow_backend *backend, struct dppd_rte_flow_object *object,
    struct dppd_flow_error *error)
{
    dppd_flow_probe_cache_invalidate_all(backend->probes);
    int rc = backend->api.remove(&object->handle, error);
    if (backend->after_remove != NULL && object->recovery_attempt != 0)
        backend->after_remove(backend->before_create_context, object->recovery_attempt, rc);
    return rc;
}

/**
 * 正式事务的校验必须访问驱动，哪怕刚才的探测缓存记录了成功或不支持
 * 同一端口的旧诊断先失效，避免一次新的校验答复与旧缓存互相矛盾
 */
int dppd_rte_flow_backend_validate_rule(struct dppd_rte_flow_backend *backend, uint16_t port_id,
    const struct dppd_rule *rule, struct dppd_flow_error *error)
{
    int rc;

    if (backend == NULL || backend->objects == NULL || backend->api.validate == NULL || rule == NULL)
        return -EINVAL;
    (void)dppd_flow_probe_cache_clear(backend->probes, port_id);
    if (rule->nb_actions > DPPD_RULE_MAX_ACTIONS)
        return -EINVAL;
    if (has_shared(rule)) {
        struct dppd_flow_binding binding;
        rc = resolve_shared(backend, port_id, rule, &binding, NULL, error);
        if (rc == 0)
            rc = backend->api.validate_bound == NULL || backend->api.create_bound == NULL
                ? shared_error(error, -ENOTSUP, "driver does not provide bound rule operations")
                : backend->api.validate_bound(port_id, rule, &binding, error);
    } else
        rc = backend->api.validate(port_id, rule, error);
    dppd_flow_probe_cache_observe(backend->probes, port_id, rule, rc);
    return rc;
}

/** 先确认规划选择了硬件后端，再调用驱动检查规则能力，这一步不预留槽位也不创建对象 */
static int transaction_validate(void *context, const struct dppd_transaction_item *item)
{
    struct dppd_rte_flow_backend *backend = context;
    struct dppd_flow_error error;

    if (item->plan.backend != DPPD_PLAN_BACKEND_RTE_FLOW)
        return -EINVAL;
    return dppd_rte_flow_backend_validate_rule(backend, item->plan.install_port_id, &item->rule, &error);
}

/**
 * 检查版本有效且不存在重复对象后，预留一个槽位，把槽位地址作为 token 交给事务
 * 对象数组在后端生命周期中保持地址稳定，后续 commit 和 rollback 可用同一 token 找回它
 */
static int transaction_prepare(void *context, const struct dppd_transaction_item *item, uintptr_t *token)
{
    struct dppd_rte_flow_backend *backend = context;
    struct dppd_rte_flow_object *object;
    uint16_t action;

    if (token == NULL)
        return -EINVAL;
    if (item->rule.generation == 0)
        return -EINVAL;
    if (find_object(backend, item->rule.id, item->rule.generation) != NULL)
        return -EEXIST;
    struct dppd_flow_binding binding;
    struct dppd_shared_count_object *references[DPPD_RULE_MAX_ACTIONS] = {0};
    int rc = resolve_shared(backend, item->plan.install_port_id, &item->rule, &binding, references, NULL);
    if (rc != 0)
        return rc;
    for (action = 0; action < item->rule.nb_actions; ++action)
        if (references[action] != NULL && references[action]->references == UINT32_MAX)
            return -EOVERFLOW;
    object = reserve_object(backend, item->rule.id, item->rule.generation);
    if (object == NULL)
        return -ENOSPC;
    object->binding = binding;
    for (action = 0; action < item->rule.nb_actions; ++action)
        if (references[action] != NULL) {
            object->shared_refs[action] = references[action];
            ++references[action]->references;
        }
    object->installation.rule_id = item->rule.id;
    object->installation.generation = item->rule.generation;
    object->installation.install_port_id = item->plan.install_port_id;
    object->installation.commit_rule_count = 1;
    for (action = 0; action < item->rule.nb_actions; ++action) {
        if (item->rule.actions[action].type == DPPD_ACTION_COUNT)
            object->installation.has_count = true;
        if (item->rule.actions[action].type == DPPD_ACTION_AGE)
            object->has_age = true;
    }
    *token = (uintptr_t)object;
    return 0;
}

/**
 * 把预留槽位转换成已安装对象，只有驱动明确返回成功才设置 installed
 * 如果驱动报错但仍留下非空 handle，也不能丢弃它，后续回滚会继续尝试删除
 */
static int transaction_commit(void *context, const struct dppd_transaction_item *item, uintptr_t token)
{
    struct dppd_rte_flow_backend *backend = context;
    struct dppd_rte_flow_object *object = (struct dppd_rte_flow_object *)token;
    struct dppd_flow_error error;
    struct dppd_install_timer timer;
    int rc;

    if (object == NULL || !object->occupied || object->installed)
        return -EINVAL;
    /** 恢复记录落盘失败时不进入驱动，事务仍可撤销尚未安装的预留槽位 */
    if (backend->before_create != NULL) {
        rc = backend->before_create(backend->before_create_context, item->plan.install_port_id, &item->rule,
            &object->recovery_attempt, object->owner_cookie);
        if (rc != 0)
            return rc;
    }
    /** 即使创建报错，也可能改变部分资源，不能继续沿用此前的探测答复 */
    dppd_flow_probe_cache_invalidate_all(backend->probes);
    /** 只测量驱动创建调用，校验、事务准备和之后的规则账本保存不算在内 */
    dppd_install_timer_start(&timer);
    if (has_shared(&item->rule))
        rc = backend->api.create_bound == NULL ? -ENOTSUP
                                               : backend->api.create_bound(item->plan.install_port_id, &item->rule,
                                                     &object->binding, &object->handle, &error);
    else if (dppd_tap_cookie_present(object->owner_cookie))
        rc = backend->api.create_owned == NULL ? -ENOTSUP
                                               : backend->api.create_owned(item->plan.install_port_id, &item->rule,
                                                     object->owner_cookie, &object->handle, &error);
    else
        rc = backend->api.create(item->plan.install_port_id, &item->rule, &object->handle, &error);
    if (rc == 0) {
        object->installation.timing_available =
            dppd_install_timer_finish(&timer, &object->installation.install_duration_ns);
        object->installed = true;
        /** 复用刚完成的测量，不额外读时钟，也不因之后的事务回滚扣除这次成功创建 */
        dppd_rule_latency_record(&backend->latency, object->installation.timing_available,
            object->installation.install_duration_ns, 1);
    }
    /** 先完成驱动计时和 handle 登记，再保存观察结果，写盘失败不能把已创建对象遗忘 */
    if (backend->after_create != NULL && object->recovery_attempt != 0) {
        int recorded = backend->after_create(backend->before_create_context, object->recovery_attempt, rc);
        if (rc == 0)
            rc = recorded;
    }
    return rc;
}

/**
 * 撤销某个预留项或已经安装的规则，最终以槽位中的真实状态判断是否需要调用删除
 * 即使 commit 报错，只要留下非空 handle，也可能有资源需要清理
 * 删除失败就保留槽位和 handle，方便恢复隔离阶段继续重试，不能提前减少 count
 */
static int transaction_rollback(void *context, const struct dppd_transaction_item *item, uintptr_t token,
    bool commit_was_attempted)
{
    struct dppd_rte_flow_backend *backend = context;
    struct dppd_rte_flow_object *object = (struct dppd_rte_flow_object *)token;
    struct dppd_flow_error error;
    int rc = 0;

    (void)item;
    (void)commit_was_attempted;
    if (object == NULL || !object->occupied)
        return -EINVAL;
    if (object->installed || object->handle.flow != NULL)
        rc = remove_object(backend, object, &error);
    if (rc == 0)
        release_object(backend, object);
    return rc;
}

/**
 * 创建固定容量的本地对象仓库，并接入真实 flow API 或调用方提供的测试接口
 * 这里只分配管理槽位，不代表预留了网卡资源，硬件资源限制仍由驱动在操作时报告
 */
int dppd_rte_flow_backend_init(struct dppd_rte_flow_backend *backend, uint32_t capacity,
    const struct dppd_flow_api *api)
{
    static const struct dppd_flow_api default_api = {
        .validate = dppd_flow_validate,
        .create = dppd_flow_create,
        .create_owned = dppd_flow_create_owned,
        .remove = dppd_flow_remove,
        .query_count = dppd_flow_query_count,
        .query_age = dppd_flow_query_age,
        .shared_count_create = dppd_flow_shared_count_create,
        .shared_count_query = dppd_flow_shared_count_query,
        .shared_count_destroy = dppd_flow_shared_count_destroy,
        .validate_bound = dppd_flow_validate_bound,
        .create_bound = dppd_flow_create_bound,
    };
    int rc;

    if (backend == NULL || capacity == 0)
        return -EINVAL;
    memset(backend, 0, sizeof(*backend));
    backend->objects = calloc(capacity, sizeof(*backend->objects));
    if (backend->objects == NULL)
        return -ENOMEM;
    backend->capacity = capacity;
    backend->shared_counts = calloc(DPPD_SHARED_COUNT_MAX, sizeof(*backend->shared_counts));
    if (backend->shared_counts == NULL) {
        free(backend->objects);
        memset(backend, 0, sizeof(*backend));
        return -ENOMEM;
    }
    backend->api = api == NULL ? default_api : *api;
    if (backend->api.validate == NULL || backend->api.create == NULL || backend->api.remove == NULL) {
        free(backend->shared_counts);
        free(backend->objects);
        memset(backend, 0, sizeof(*backend));
        return -EINVAL;
    }
    rc = dppd_flow_probe_cache_init(&backend->probes);
    if (rc != 0) {
        free(backend->shared_counts);
        free(backend->objects);
        memset(backend, 0, sizeof(*backend));
        return rc;
    }
    return 0;
}

/**
 * 普通退出时尝试删除所有已安装或失败后仍有 handle 的对象，然后释放本地仓库
 * 某个删除失败时仍继续处理其他对象，但保留仓库并返回第一个错误，避免丢失失败 handle
 * 与隔离重试一样保留部分创建的清理线索，不能因 installed 为假就漏删并报告干净
 */
int dppd_rte_flow_backend_fini(struct dppd_rte_flow_backend *backend)
{
    struct dppd_flow_error error;
    uint32_t i;
    int result = 0;

    if (backend == NULL)
        return -EINVAL;
    for (i = 0; i < backend->capacity; ++i) {
        struct dppd_rte_flow_object *object = &backend->objects[i];
        int rc;

        if (!object->occupied)
            continue;
        rc = object->installed || object->handle.flow != NULL ? remove_object(backend, object, &error) : 0;
        if (rc == 0)
            release_object(backend, object);
        else if (result == 0)
            result = rc;
    }
    if (backend->shared_counts != NULL) {
        int rc = cleanup_shared(backend);
        if (result == 0)
            result = rc;
    }
    if (result != 0)
        return result;
    free(backend->shared_counts);
    free(backend->objects);
    dppd_flow_probe_cache_destroy(backend->probes);
    memset(backend, 0, sizeof(*backend));
    return result;
}

static int simulated_reset_error(struct dppd_flow_error *error, int rc, const char *message)
{
    if (error != NULL) {
        error->code = rc;
        snprintf(error->message, sizeof(error->message), "simulated reset: %s", message);
    }
    return rc;
}

/** 模拟复位是一次原子失效，真实硬件复位的部分成功语义不在这个接口的承诺内 */
int dppd_rte_flow_backend_simulate_reset(struct dppd_rte_flow_backend *backend, struct dppd_flow_error *error)
{
    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (backend == NULL || backend->objects == NULL || backend->shared_counts == NULL || backend->probes == NULL ||
        backend->capacity == 0)
        return simulated_reset_error(error, -EINVAL, "invalid backend");
    if (backend->before_create != NULL)
        return simulated_reset_error(error, -ENOTSUP, "native recovery journal cannot track simulated reset");
    if (backend->api.simulate_reset == NULL)
        return simulated_reset_error(error, -ENOTSUP, "driver does not provide simulation-only reset");
    int rc = backend->api.simulate_reset(error);
    if (rc != 0)
        return rc;
    /** 成功后旧句柄已经失效，不能再调用删除接口，也不逐条归还已失效的引用 */
    memset(backend->objects, 0, backend->capacity * sizeof(*backend->objects));
    memset(backend->shared_counts, 0, DPPD_SHARED_COUNT_MAX * sizeof(*backend->shared_counts));
    backend->count = 0;
    backend->shared_count_count = 0;
    dppd_flow_probe_cache_invalidate_all(backend->probes);
    return 0;
}

/**
 * 把硬件仓库包装成通用事务接口，让控制层使用统一的四阶段操作
 * token 就是仓库槽位地址，没有另外分配的临时对象，所以无需独立 finalize 回调
 */
struct dppd_transaction_backend dppd_rte_flow_transaction_backend(struct dppd_rte_flow_backend *backend)
{
    const struct dppd_transaction_backend operations = {
        .context = backend,
        .validate = transaction_validate,
        .prepare = transaction_prepare,
        .commit = transaction_commit,
        .rollback = transaction_rollback,
    };
    return operations;
}

/** 返回预留和已安装槽位的总占用数，控制层据此检查本地临时空间是否足够 */
uint32_t dppd_rte_flow_backend_count(const struct dppd_rte_flow_backend *backend)
{
    return backend == NULL ? 0 : backend->count;
}

/**
 * 查找指定 ID 的最新槽位，仅在该槽位已经安装成功时返回 handle
 * 若最新版本还在准备阶段，本接口返回空指针，不会自动回退到更旧的已安装版本
 */
const struct dppd_flow_handle *dppd_rte_flow_backend_find(const struct dppd_rte_flow_backend *backend, uint64_t rule_id)
{
    const struct dppd_rte_flow_object *object;

    if (backend == NULL || backend->objects == NULL || rule_id == 0)
        return NULL;
    object = find_latest_object_const(backend, rule_id);
    if (object == NULL || !object->installed)
        return NULL;
    return &object->handle;
}

/** 精确查找已经安装的指定版本，更新期间用它区分同一规则的新旧对象 */
const struct dppd_flow_handle *dppd_rte_flow_backend_find_version(const struct dppd_rte_flow_backend *backend,
    uint64_t rule_id, uint64_t generation)
{
    const struct dppd_rte_flow_object *object;

    if (backend == NULL || backend->objects == NULL || rule_id == 0 || generation == 0)
        return NULL;
    object = find_object_const(backend, rule_id, generation);
    if (object == NULL || !object->installed)
        return NULL;
    return &object->handle;
}

/**
 * 读取本进程保留的精确安装记录，查询本身不触碰网卡，也不自动修复异常对象
 * prepare 只占用槽位，还不能报告安装成功；已安装对象必须保留一个有效且身份相符的 handle
 */
int dppd_rte_flow_backend_install_info(const struct dppd_rte_flow_backend *backend, uint64_t rule_id,
    uint64_t generation, struct dppd_rule_install_info *info)
{
    const struct dppd_rte_flow_object *object;

    if (backend == NULL || backend->objects == NULL || rule_id == 0 || generation == 0 || info == NULL)
        return -EINVAL;
    memset(info, 0, sizeof(*info));
    object = find_object_const(backend, rule_id, generation);
    if (object == NULL || !object->installed)
        return -ENOENT;
    if (object->handle.flow == NULL || object->handle.rule_id != rule_id ||
        object->handle.rule_generation != generation ||
        object->handle.port_id != object->installation.install_port_id ||
        object->handle.has_count != object->installation.has_count)
        return -EUCLEAN;
    *info = object->installation;
    return 0;
}

/** 删除指定 ID 的最新槽位对应规则，仅在驱动删除成功后归还槽位 */
int dppd_rte_flow_backend_remove(struct dppd_rte_flow_backend *backend, uint64_t rule_id, struct dppd_flow_error *error)
{
    struct dppd_rte_flow_object *object;
    int rc;

    if (backend == NULL || backend->objects == NULL || rule_id == 0)
        return -EINVAL;
    object = find_latest_object(backend, rule_id);
    if (object == NULL || !object->installed)
        return -ENOENT;
    rc = remove_object(backend, object, error);
    if (rc == 0)
        release_object(backend, object);
    return rc;
}

/**
 * 删除精确的 ID/版本组合，避免新旧版本共存时误删另一个版本
 * 删除失败保留对象，调用方可报告错误或进入隔离后继续清理
 */
int dppd_rte_flow_backend_remove_version(struct dppd_rte_flow_backend *backend, uint64_t rule_id, uint64_t generation,
    struct dppd_flow_error *error)
{
    struct dppd_rte_flow_object *object;
    int rc;

    if (backend == NULL || backend->objects == NULL || rule_id == 0 || generation == 0)
        return -EINVAL;
    object = find_object(backend, rule_id, generation);
    if (object == NULL || !object->installed)
        return -ENOENT;
    rc = remove_object(backend, object, error);
    if (rc == 0)
        release_object(backend, object);
    return rc;
}

/**
 * 恢复隔离中的“尽力清理全部残留”入口，不按规则账本决定哪些对象要保留
 * 有对象删不掉时继续处理其余对象，最后同时返回第一个错误和剩余槽位数量
 * 成功清空后仍需要重新启动和重放快照，不能认为原业务规则已经恢复
 */
int dppd_rte_flow_backend_reconcile(struct dppd_rte_flow_backend *backend, uint32_t *residual_objects)
{
    struct dppd_flow_error error;
    uint32_t i;
    int first_error = 0;

    if (backend == NULL || backend->objects == NULL || residual_objects == NULL)
        return -EINVAL;

    /**
     * 只预留槽位但尚未留下 handle 的对象可以直接归还本地资源
     * 已安装或仍有 handle 的对象必须调用同一驱动删除，不能只清本地标记冒充清理成功
     */
    for (i = 0; i < backend->capacity; ++i) {
        struct dppd_rte_flow_object *object = &backend->objects[i];
        int rc = 0;

        if (!object->occupied)
            continue;
        if (object->installed || object->handle.flow != NULL)
            rc = remove_object(backend, object, &error);
        if (rc == 0)
            release_object(backend, object);
        else if (first_error == 0)
            first_error = rc;
    }
    int rc = cleanup_shared(backend);
    if (first_error == 0)
        first_error = rc;
    *residual_objects = backend->count + backend->shared_count_count;
    return first_error;
}

/**
 * 查询已经安装的精确版本计数，不因该 ID 还有更新版本就替换查询对象
 * 对象不存在、没有 COUNT 动作、未提供查询接口分别返回不同错误，不能混成零命中
 */
int dppd_rte_flow_backend_query_count(const struct dppd_rte_flow_backend *backend, uint64_t rule_id,
    uint64_t generation, uint64_t *hits, uint64_t *bytes, struct dppd_flow_error *error)
{
    const struct dppd_flow_handle *handle;

    if (backend == NULL || backend->objects == NULL || rule_id == 0 || generation == 0 || hits == NULL || bytes == NULL)
        return -EINVAL;
    handle = dppd_rte_flow_backend_find_version(backend, rule_id, generation);
    if (handle == NULL)
        return -ENOENT;
    /**
     * 先根据创建时记录的元数据检查有没有 COUNT 动作，统一返回 ENODATA
     * 这样客户端不会因为换了一种驱动，就把“规则没有计数器”误解为别的错误
     */
    if (!handle->has_count)
        return -ENODATA;
    if (backend->api.query_count == NULL)
        return -ENOTSUP;
    return backend->api.query_count(handle, hits, bytes, error);
}

static int age_query_error(struct dppd_flow_error *error, int rc, const char *message)
{
    if (error != NULL) {
        error->code = rc;
        snprintf(error->message, sizeof(error->message), "query age: %s", message);
    }
    return rc;
}

/** 只查询账本指定的已安装版本，准备中的槽位、新版本或异常 handle 都不能代替它 */
int dppd_rte_flow_backend_query_age(const struct dppd_rte_flow_backend *backend, uint64_t rule_id, uint64_t generation,
    struct dppd_flow_age_result *result, struct dppd_flow_error *error)
{
    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (backend == NULL || backend->objects == NULL || rule_id == 0 || generation == 0 || generation == UINT64_MAX ||
        result == NULL)
        return age_query_error(error, -EINVAL, "invalid backend, identity or output");
    const struct dppd_rte_flow_object *object = find_object_const(backend, rule_id, generation);
    if (object == NULL || !object->installed)
        return age_query_error(error, -ENOENT, "installed version not found");
    const struct dppd_flow_handle *handle = &object->handle;
    if (handle->flow == NULL || handle->rule_id != rule_id || handle->rule_generation != generation ||
        handle->port_id != object->installation.install_port_id || handle->has_age != object->has_age)
        return age_query_error(error, -EUCLEAN, "installed handle identity does not match");
    if (!object->has_age)
        return age_query_error(error, -ENODATA, "rule has no AGE action");
    if (backend->api.query_age == NULL)
        return age_query_error(error, -ENOTSUP, "driver does not provide AGE query");
    /** 自定义驱动接口也可能在报错前写入结果，先用局部副本接收，成功后才发布 */
    struct dppd_flow_age_result next = {0};
    int rc = backend->api.query_age(handle, &next, error);
    if (rc == 0) {
        if (!next.sec_since_last_hit_valid)
            next.sec_since_last_hit = 0;
        *result = next;
    }
    return rc;
}

/** 共享对象由独立 ID 和版本标识，驱动指针只留在后端，不进入规则定义或快照 */
int dppd_rte_flow_backend_shared_count_create(struct dppd_rte_flow_backend *backend,
    const struct dppd_shared_count_spec *spec, struct dppd_flow_error *error)
{
    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (backend == NULL || backend->shared_counts == NULL || spec == NULL || spec->id == 0 || spec->id == UINT64_MAX ||
        spec->generation == 0 || spec->generation == UINT64_MAX || spec->install_port_id == UINT16_MAX ||
        spec->domain < DPPD_RULE_DOMAIN_INGRESS || spec->domain > DPPD_RULE_DOMAIN_TRANSFER)
        return shared_error(error, -EINVAL, "invalid backend or object identity");
    if (backend->before_create != NULL)
        return shared_error(error, -ENOTSUP, "native recovery journal cannot track indirect objects");
    if (find_shared(backend, spec->id) != NULL)
        return shared_error(error, -EEXIST, "object ID already exists");
    if (backend->shared_count_count == DPPD_SHARED_COUNT_MAX)
        return shared_error(error, -ENOSPC, "object registry is full");
    /** 创建之前必须确认还有删除入口，查询可以缺失，但不能留下根本无法释放的对象 */
    if (backend->api.shared_count_create == NULL || backend->api.shared_count_destroy == NULL)
        return shared_error(error, -ENOTSUP, "driver does not provide indirect create and destroy");
    struct dppd_shared_count_object *slot = NULL;
    for (uint32_t i = 0; i < DPPD_SHARED_COUNT_MAX; ++i)
        if (!backend->shared_counts[i].used) {
            slot = &backend->shared_counts[i];
            break;
        }
    if (slot == NULL)
        return shared_error(error, -EUCLEAN, "registry occupancy is inconsistent");
    struct dppd_shared_count_handle next = {.spec = *spec};
    dppd_flow_probe_cache_invalidate_all(backend->probes);
    int rc = backend->api.shared_count_create(spec, &next, error);
    if (rc != 0 && next.opaque == NULL)
        return rc;
    /** 失败却留下句柄时保留清理线索，不能把这类部分成功对象当成可引用的正常对象 */
    bool installed = rc == 0 && next.opaque != NULL && shared_spec_equal(spec, &next.spec);
    next.spec = *spec;
    *slot = (struct dppd_shared_count_object){.used = true, .installed = installed, .spec = *spec, .handle = next};
    ++backend->shared_count_count;
    if (!installed)
        return shared_error(error, -EUCLEAN, "create left an uncertain indirect object");
    return 0;
}

int dppd_rte_flow_backend_shared_count_get(const struct dppd_rte_flow_backend *backend, uint64_t id, uint64_t expected,
    struct dppd_shared_count_info *info)
{
    if (info == NULL)
        return -EINVAL;
    struct dppd_shared_count_object *object;
    int rc = lookup_shared(backend, id, expected, &object);
    if (rc != 0)
        return rc;
    if (!object->installed || object->handle.opaque == NULL || !shared_spec_equal(&object->spec, &object->handle.spec))
        return -EUCLEAN;
    *info = (struct dppd_shared_count_info){.spec = object->spec, .references = object->references};
    return 0;
}

/** 用局部有序副本发布列表，空间不足或对象异常时不把半份结果交给快照 */
int dppd_rte_flow_backend_shared_count_list(const struct dppd_rte_flow_backend *backend,
    struct dppd_shared_count_spec *specs, uint32_t capacity, uint32_t *count)
{
    if (backend == NULL || backend->shared_counts == NULL || count == NULL || (capacity != 0 && specs == NULL))
        return -EINVAL;
    if (capacity < backend->shared_count_count)
        return -ENOSPC;
    struct dppd_shared_count_spec sorted[DPPD_SHARED_COUNT_MAX];
    uint32_t used = 0;
    for (uint32_t i = 0; i < DPPD_SHARED_COUNT_MAX; ++i) {
        const struct dppd_shared_count_object *object = &backend->shared_counts[i];
        if (!object->used)
            continue;
        if (!object->installed || object->handle.opaque == NULL ||
            !shared_spec_equal(&object->spec, &object->handle.spec))
            return -EUCLEAN;
        uint32_t at = used;
        while (at != 0 && sorted[at - 1].id > object->spec.id) {
            sorted[at] = sorted[at - 1];
            --at;
        }
        sorted[at] = object->spec;
        ++used;
    }
    if (used != 0)
        memcpy(specs, sorted, used * sizeof(*specs));
    *count = used;
    return 0;
}

int dppd_rte_flow_backend_shared_count_query(const struct dppd_rte_flow_backend *backend, uint64_t id,
    uint64_t expected, uint64_t *hits, uint64_t *bytes, struct dppd_flow_error *error)
{
    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (hits == NULL || bytes == NULL)
        return shared_error(error, -EINVAL, "missing query output");
    struct dppd_shared_count_object *object;
    int rc = lookup_shared(backend, id, expected, &object);
    if (rc != 0)
        return shared_error(error, rc, "requested identity was not found");
    if (!object->installed || object->handle.opaque == NULL || !shared_spec_equal(&object->spec, &object->handle.spec))
        return shared_error(error, -EUCLEAN, "queried handle is uncertain");
    if (backend->api.shared_count_query == NULL)
        return shared_error(error, -ENOTSUP, "driver does not provide indirect query");
    uint64_t next_hits = 0, next_bytes = 0;
    rc = backend->api.shared_count_query(&object->handle, &next_hits, &next_bytes, error);
    if (rc == 0) {
        *hits = next_hits;
        *bytes = next_bytes;
    }
    return rc;
}

int dppd_rte_flow_backend_shared_count_destroy(struct dppd_rte_flow_backend *backend, uint64_t id, uint64_t expected,
    struct dppd_flow_error *error)
{
    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (expected == UINT64_MAX)
        return shared_error(error, -EINVAL, "destroy requires an exact generation");
    struct dppd_shared_count_object *object;
    int rc = lookup_shared(backend, id, expected, &object);
    if (rc != 0)
        return shared_error(error, rc, "requested identity was not found");
    return destroy_shared(backend, object, error);
}

uint32_t dppd_rte_flow_backend_shared_count_count(const struct dppd_rte_flow_backend *backend)
{
    return backend == NULL ? 0 : backend->shared_count_count;
}
