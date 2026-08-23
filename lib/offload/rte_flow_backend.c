#include "dppd/rte_flow_backend.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct dppd_rte_flow_object {
    bool occupied;
    bool installed;
    uint64_t rule_id;
    uint64_t generation;
    struct dppd_flow_handle handle;
};

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

static struct dppd_rte_flow_object *reserve_object(
    struct dppd_rte_flow_backend *backend,
    uint64_t rule_id,
    uint64_t generation)
{
    uint32_t i;

    /* 同一 rule ID 的不同 generation 可共存，便于先建新规则再撤旧规则。 */
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

static void release_object(struct dppd_rte_flow_backend *backend,
                           struct dppd_rte_flow_object *object)
{
    memset(object, 0, sizeof(*object));
    backend->count--;
}

static int transaction_validate(void *context,
                                const struct dppd_transaction_item *item)
{
    struct dppd_rte_flow_backend *backend = context;
    struct dppd_flow_error error;

    if (item->plan.backend != DPPD_PLAN_BACKEND_RTE_FLOW)
        return -EINVAL;
    return backend->api.validate(item->plan.install_port_id, &item->rule, &error);
}

static int transaction_prepare(void *context,
                               const struct dppd_transaction_item *item,
                               uintptr_t *token)
{
    struct dppd_rte_flow_backend *backend = context;
    struct dppd_rte_flow_object *object;

    if (token == NULL)
        return -EINVAL;
    if (item->rule.generation == 0)
        return -EINVAL;
    if (find_object(backend, item->rule.id, item->rule.generation) != NULL)
        return -EEXIST;
    object = reserve_object(backend, item->rule.id, item->rule.generation);
    if (object == NULL)
        return -ENOSPC;
    *token = (uintptr_t)object;
    return 0;
}

static int transaction_commit(void *context,
                              const struct dppd_transaction_item *item,
                              uintptr_t token)
{
    struct dppd_rte_flow_backend *backend = context;
    struct dppd_rte_flow_object *object = (struct dppd_rte_flow_object *)token;
    struct dppd_flow_error error;
    int rc;

    if (object == NULL || !object->occupied || object->installed)
        return -EINVAL;
    rc = backend->api.create(item->plan.install_port_id, &item->rule,
                             &object->handle, &error);
    if (rc == 0)
        object->installed = true;
    return rc;
}

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
        rc = backend->api.remove(&object->handle, &error);
    if (rc == 0)
        release_object(backend, object);
    return rc;
}

int dppd_rte_flow_backend_init(struct dppd_rte_flow_backend *backend,
                               uint32_t capacity,
                               const struct dppd_flow_api *api)
{
    static const struct dppd_flow_api default_api = {
        .validate = dppd_flow_validate,
        .create = dppd_flow_create,
        .remove = dppd_flow_remove,
        .query_count = dppd_flow_query_count,
    };

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
    return 0;
}

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

        if (!object->occupied || !object->installed)
            continue;
        rc = backend->api.remove(&object->handle, &error);
        if (rc == 0)
            release_object(backend, object);
        else if (result == 0)
            result = rc;
    }
    if (result != 0)
        return result;
    free(backend->objects);
    memset(backend, 0, sizeof(*backend));
    return result;
}

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

uint32_t dppd_rte_flow_backend_count(const struct dppd_rte_flow_backend *backend)
{
    return backend == NULL ? 0 : backend->count;
}

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
    rc = backend->api.remove(&object->handle, error);
    if (rc == 0)
        release_object(backend, object);
    return rc;
}


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
    rc = backend->api.remove(&object->handle, error);
    if (rc == 0)
        release_object(backend, object);
    return rc;
}

int dppd_rte_flow_backend_reconcile(struct dppd_rte_flow_backend *backend,
                                    uint32_t *residual_objects)
{
    struct dppd_flow_error error;
    uint32_t i;
    int first_error = 0;

    if (backend == NULL || backend->objects == NULL ||
        residual_objects == NULL)
        return -EINVAL;

    /*
     * prepare 后但 create 尚未留下 handle 的槽位可直接归还；有 handle 的槽位必须
     * 通过同一 PMD remove 回收，不能只清内存标记而把真实硬件 flow 遗留在设备中。
     */
    for (i = 0; i < backend->capacity; ++i) {
        struct dppd_rte_flow_object *object = &backend->objects[i];
        int rc = 0;

        if (!object->occupied)
            continue;
        if (object->installed || object->handle.flow != NULL)
            rc = backend->api.remove(&object->handle, &error);
        if (rc == 0)
            release_object(backend, object);
        else if (first_error == 0)
            first_error = rc;
    }
    *residual_objects = backend->count;
    return first_error;
}

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
    /*
     * 在调用 PMD 前检查 handle 元数据，可为“不含 COUNT”提供稳定错误，
     * 避免不同驱动分别返回 ENOENT、EINVAL 或 ENOTSUP。
     */
    if (!handle->has_count)
        return -ENODATA;
    if (backend->api.query_count == NULL)
        return -ENOTSUP;
    return backend->api.query_count(handle, hits, bytes, error);
}
