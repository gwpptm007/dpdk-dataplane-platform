#include "dppd/rte_flow_backend.h"
#include "dppd/install_time.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/**
 * 硬件对象仓库中的一个槽位，既可以表示准备阶段的预留，也可以表示已安装规则
 * occupied 与 installed 分开记录，才能分清“只占了位置”和“已经需要调用驱动删除”
 */
struct dppd_rte_flow_object {
    /** 槽位已经被某次事务占用，尚未归还 */
    bool occupied;
    /** 驱动的创建调用已经成功，实际对象由 handle 保存 */
    bool installed;
    uint64_t rule_id;
    uint64_t generation;
    /** 精确关联本次驱动调用的磁盘记录，业务 ID 和版本相同的补偿重建也不复用 */
    uint64_t recovery_attempt;
    uint8_t owner_cookie[DPPD_TAP_COOKIE_SIZE];
    struct dppd_flow_handle handle;
    /** 在 prepare 时保留安装意图，查询时据此核对驱动返回的 handle */
    struct dppd_rule_install_info installation;
};

/** 按规则 ID 和精确版本查找已占用槽位，允许找到尚未安装的 prepare 预留项 */
static struct dppd_rte_flow_object *find_object(
    struct dppd_rte_flow_backend *backend,
    uint64_t rule_id,
    uint64_t generation)
{
    uint32_t i;

    for (i = 0; i < backend->capacity; ++i) {
        if (backend->objects[i].occupied &&
            backend->objects[i].rule_id == rule_id &&
            backend->objects[i].generation == generation)
            return &backend->objects[i];
    }
    return NULL;
}

/** 提供只读的精确版本查找，是否已经安装由调用方进一步检查 */
static const struct dppd_rte_flow_object *find_object_const(
    const struct dppd_rte_flow_backend *backend,
    uint64_t rule_id,
    uint64_t generation)
{
    uint32_t i;

    for (i = 0; i < backend->capacity; ++i) {
        if (backend->objects[i].occupied &&
            backend->objects[i].rule_id == rule_id &&
            backend->objects[i].generation == generation)
            return &backend->objects[i];
    }
    return NULL;
}

/**
 * 从同一个 ID 的已占用槽位中找出版本号最大的对象，不区分预留还是已安装
 * 调用方不能把“最新槽位”直接当成可用硬件规则，仍要检查 installed
 */
static struct dppd_rte_flow_object *find_latest_object(
    struct dppd_rte_flow_backend *backend, uint64_t rule_id)
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
static const struct dppd_rte_flow_object *find_latest_object_const(
    const struct dppd_rte_flow_backend *backend, uint64_t rule_id)
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
static struct dppd_rte_flow_object *reserve_object(
    struct dppd_rte_flow_backend *backend,
    uint64_t rule_id,
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
static void release_object(struct dppd_rte_flow_backend *backend,
                           struct dppd_rte_flow_object *object)
{
    memset(object, 0, sizeof(*object));
    backend->count--;
}

/** 删除尝试可能影响共享资源，先清空所有探测结果，再保留原有驱动删除和失败重试语义 */
static int remove_object(struct dppd_rte_flow_backend *backend,
                          struct dppd_rte_flow_object *object,
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
int dppd_rte_flow_backend_validate_rule(struct dppd_rte_flow_backend *backend,
    uint16_t port_id, const struct dppd_rule *rule, struct dppd_flow_error *error)
{
    int rc;

    if (backend == NULL || backend->objects == NULL || backend->api.validate == NULL ||
        rule == NULL)
        return -EINVAL;
    (void)dppd_flow_probe_cache_clear(backend->probes, port_id);
    rc = backend->api.validate(port_id, rule, error);
    dppd_flow_probe_cache_observe(backend->probes, port_id, rule, rc);
    return rc;
}

/** 先确认规划选择了硬件后端，再调用驱动检查规则能力，这一步不预留槽位也不创建对象 */
static int transaction_validate(void *context,
                                const struct dppd_transaction_item *item)
{
    struct dppd_rte_flow_backend *backend = context;
    struct dppd_flow_error error;

    if (item->plan.backend != DPPD_PLAN_BACKEND_RTE_FLOW)
        return -EINVAL;
    return dppd_rte_flow_backend_validate_rule(backend, item->plan.install_port_id,
                                               &item->rule, &error);
}

/**
 * 检查版本有效且不存在重复对象后，预留一个槽位，把槽位地址作为 token 交给事务
 * 对象数组在后端生命周期中保持地址稳定，后续 commit 和 rollback 可用同一 token 找回它
 */
static int transaction_prepare(void *context,
                               const struct dppd_transaction_item *item,
                               uintptr_t *token)
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
    object = reserve_object(backend, item->rule.id, item->rule.generation);
    if (object == NULL)
        return -ENOSPC;
    object->installation.rule_id = item->rule.id;
    object->installation.generation = item->rule.generation;
    object->installation.install_port_id = item->plan.install_port_id;
    object->installation.commit_rule_count = 1;
    for (action = 0; action < item->rule.nb_actions; ++action) {
        if (item->rule.actions[action].type == DPPD_ACTION_COUNT)
            object->installation.has_count = true;
    }
    *token = (uintptr_t)object;
    return 0;
}

/**
 * 把预留槽位转换成已安装对象，只有驱动明确返回成功才设置 installed
 * 如果驱动报错但仍留下非空 handle，也不能丢弃它，后续回滚会继续尝试删除
 */
static int transaction_commit(void *context,
                              const struct dppd_transaction_item *item,
                              uintptr_t token)
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
        rc = backend->before_create(backend->before_create_context, item->plan.install_port_id,
            &item->rule, &object->recovery_attempt, object->owner_cookie);
        if (rc != 0)
            return rc;
    }
    /** 即使创建报错，也可能改变部分资源，不能继续沿用此前的探测答复 */
    dppd_flow_probe_cache_invalidate_all(backend->probes);
    /** 只测量驱动创建调用，校验、事务准备和之后的规则账本保存不算在内 */
    dppd_install_timer_start(&timer);
    if (dppd_tap_cookie_present(object->owner_cookie))
        rc = backend->api.create_owned == NULL ? -ENOTSUP : backend->api.create_owned(
            item->plan.install_port_id, &item->rule, object->owner_cookie, &object->handle, &error);
    else
        rc = backend->api.create(item->plan.install_port_id, &item->rule, &object->handle, &error);
    if (rc == 0) {
        object->installation.timing_available = dppd_install_timer_finish(
            &timer, &object->installation.install_duration_ns);
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
static int transaction_rollback(void *context,
                                const struct dppd_transaction_item *item,
                                uintptr_t token,
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
int dppd_rte_flow_backend_init(struct dppd_rte_flow_backend *backend,
                               uint32_t capacity,
                               const struct dppd_flow_api *api)
{
    static const struct dppd_flow_api default_api = {
        .validate = dppd_flow_validate,
        .create = dppd_flow_create,
        .create_owned = dppd_flow_create_owned,
        .remove = dppd_flow_remove,
        .query_count = dppd_flow_query_count,
    };
    int rc;

    if (backend == NULL || capacity == 0)
        return -EINVAL;
    memset(backend, 0, sizeof(*backend));
    backend->objects = calloc(capacity, sizeof(*backend->objects));
    if (backend->objects == NULL)
        return -ENOMEM;
    backend->capacity = capacity;
    backend->api = api == NULL ? default_api : *api;
    if (backend->api.validate == NULL || backend->api.create == NULL ||
        backend->api.remove == NULL) {
        free(backend->objects);
        memset(backend, 0, sizeof(*backend));
        return -EINVAL;
    }
    rc = dppd_flow_probe_cache_init(&backend->probes);
    if (rc != 0) {
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

        if (!object->occupied || (!object->installed && object->handle.flow == NULL))
            continue;
        rc = remove_object(backend, object, &error);
        if (rc == 0)
            release_object(backend, object);
        else if (result == 0)
            result = rc;
    }
    if (result != 0)
        return result;
    free(backend->objects);
    dppd_flow_probe_cache_destroy(backend->probes);
    memset(backend, 0, sizeof(*backend));
    return result;
}

/**
 * 把硬件仓库包装成通用事务接口，让控制层使用统一的四阶段操作
 * token 就是仓库槽位地址，没有另外分配的临时对象，所以无需独立 finalize 回调
 */
struct dppd_transaction_backend dppd_rte_flow_transaction_backend(
    struct dppd_rte_flow_backend *backend)
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
const struct dppd_flow_handle *dppd_rte_flow_backend_find(
    const struct dppd_rte_flow_backend *backend, uint64_t rule_id)
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
const struct dppd_flow_handle *dppd_rte_flow_backend_find_version(
    const struct dppd_rte_flow_backend *backend,
    uint64_t rule_id,
    uint64_t generation)
{
    const struct dppd_rte_flow_object *object;

    if (backend == NULL || backend->objects == NULL ||
        rule_id == 0 || generation == 0)
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
int dppd_rte_flow_backend_install_info(
    const struct dppd_rte_flow_backend *backend, uint64_t rule_id,
    uint64_t generation, struct dppd_rule_install_info *info)
{
    const struct dppd_rte_flow_object *object;

    if (backend == NULL || backend->objects == NULL || rule_id == 0 ||
        generation == 0 || info == NULL)
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
int dppd_rte_flow_backend_remove(struct dppd_rte_flow_backend *backend,
                                 uint64_t rule_id,
                                 struct dppd_flow_error *error)
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
int dppd_rte_flow_backend_remove_version(struct dppd_rte_flow_backend *backend,
                                         uint64_t rule_id,
                                         uint64_t generation,
                                         struct dppd_flow_error *error)
{
    struct dppd_rte_flow_object *object;
    int rc;

    if (backend == NULL || backend->objects == NULL ||
        rule_id == 0 || generation == 0)
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
int dppd_rte_flow_backend_reconcile(struct dppd_rte_flow_backend *backend,
                                    uint32_t *residual_objects)
{
    struct dppd_flow_error error;
    uint32_t i;
    int first_error = 0;

    if (backend == NULL || backend->objects == NULL ||
        residual_objects == NULL)
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
    *residual_objects = backend->count;
    return first_error;
}

/**
 * 查询已经安装的精确版本计数，不因该 ID 还有更新版本就替换查询对象
 * 对象不存在、没有 COUNT 动作、未提供查询接口分别返回不同错误，不能混成零命中
 */
int dppd_rte_flow_backend_query_count(
    const struct dppd_rte_flow_backend *backend,
    uint64_t rule_id,
    uint64_t generation,
    uint64_t *hits,
    uint64_t *bytes,
    struct dppd_flow_error *error)
{
    const struct dppd_flow_handle *handle;

    if (backend == NULL || backend->objects == NULL ||
        rule_id == 0 || generation == 0 || hits == NULL || bytes == NULL)
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
