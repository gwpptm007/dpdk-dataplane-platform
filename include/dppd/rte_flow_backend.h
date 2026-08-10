#ifndef DPPD_RTE_FLOW_BACKEND_H
#define DPPD_RTE_FLOW_BACKEND_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/offload.h"
#include "dppd/transaction.h"

struct dppd_flow_api {
    int (*validate)(uint16_t port_id, const struct dppd_rule *rule,
                    struct dppd_flow_error *error);
    int (*create)(uint16_t port_id, const struct dppd_rule *rule,
                  struct dppd_flow_handle *handle,
                  struct dppd_flow_error *error);
    int (*remove)(struct dppd_flow_handle *handle,
                  struct dppd_flow_error *error);
    /*
     * query_count 是可选能力：事务 CRUD 测试可不提供；真正执行计数查询时，
     * backend 会对缺失回调返回 -ENOTSUP，而不是伪造 0 命中。
     */
    int (*query_count)(const struct dppd_flow_handle *handle,
                       uint64_t *hits,
                       uint64_t *bytes,
                       struct dppd_flow_error *error);
};

struct dppd_rte_flow_object;

struct dppd_rte_flow_backend {
    struct dppd_rte_flow_object *objects;
    uint32_t capacity;
    uint32_t count;
    struct dppd_flow_api api;
};

/*
 * backend 同时保存已安装对象和事务 prepare 阶段的预留槽位。
 * 对象以 (rule_id, generation) 唯一标识，允许更新期间新旧代短暂共存。
 */
int dppd_rte_flow_backend_init(struct dppd_rte_flow_backend *backend,
                               uint32_t capacity,
                               const struct dppd_flow_api *api);
int dppd_rte_flow_backend_fini(struct dppd_rte_flow_backend *backend);
struct dppd_transaction_backend dppd_rte_flow_transaction_backend(
    struct dppd_rte_flow_backend *backend);
uint32_t dppd_rte_flow_backend_count(const struct dppd_rte_flow_backend *backend);
const struct dppd_flow_handle *dppd_rte_flow_backend_find(
    const struct dppd_rte_flow_backend *backend, uint64_t rule_id);
const struct dppd_flow_handle *dppd_rte_flow_backend_find_version(
    const struct dppd_rte_flow_backend *backend,
    uint64_t rule_id,
    uint64_t generation);
int dppd_rte_flow_backend_remove(struct dppd_rte_flow_backend *backend,
                                 uint64_t rule_id,
                                 struct dppd_flow_error *error);
int dppd_rte_flow_backend_remove_version(struct dppd_rte_flow_backend *backend,
                                         uint64_t rule_id,
                                         uint64_t generation,
                                         struct dppd_flow_error *error);
/*
 * 查询指定 (rule_id, generation) 的 COUNT action。返回值区分：
 * -ENOENT：对象不存在；-ENODATA：规则没有 COUNT；-ENOTSUP：backend 无查询实现。
 */
int dppd_rte_flow_backend_query_count(
    const struct dppd_rte_flow_backend *backend,
    uint64_t rule_id,
    uint64_t generation,
    uint64_t *hits,
    uint64_t *bytes,
    struct dppd_flow_error *error);

#endif
