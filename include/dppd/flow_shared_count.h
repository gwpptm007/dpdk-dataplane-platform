#ifndef DPPD_FLOW_SHARED_COUNT_H
#define DPPD_FLOW_SHARED_COUNT_H

#include "dppd/admission.h"
#include "dppd/shared_count.h"

struct dppd_flow_shared_count_driver {
    void *context;
    int (*create)(void *context, const struct dppd_shared_count_spec *spec, struct dppd_shared_count_handle *handle);
    int (*query)(void *context, const struct dppd_shared_count_handle *handle, uint64_t *hits, uint64_t *bytes);
    /** 成功必须清空 opaque，失败仍保留可重试的精确句柄 */
    int (*destroy)(void *context, struct dppd_shared_count_handle *handle);
};

struct dppd_flow_shared_count_lease {
    uint64_t owner, serial;
    uint32_t slot;
};

struct dppd_flow_shared_count_info {
    struct dppd_shared_count_spec spec;
    uint32_t references;
    bool residual;
};

struct dppd_flow_shared_counts;

/** 最多 64 个实际对象，引用池预分配且有界，依赖的配额对象必须晚于本库关闭 */
int dppd_flow_shared_counts_create(struct dppd_admission *admission, uint32_t lease_capacity,
    const struct dppd_flow_shared_count_driver *driver, struct dppd_flow_shared_counts **output);
int dppd_flow_shared_counts_close(struct dppd_flow_shared_counts **counts);
/** generation 由本库分配，删除重建不复用旧版本，部分创建残留返回 EUCLEAN 及可清理身份 */
int dppd_flow_shared_counts_add(struct dppd_flow_shared_counts *counts, uint64_t id, uint16_t port,
    enum dppd_rule_domain domain, struct dppd_flow_shared_count_info *info);
/** 启动时按旧版本升序重建，seed 保留已删除对象使用过的版本，防止重启后旧引用复活 */
int dppd_flow_shared_counts_replay_add(struct dppd_flow_shared_counts *counts,
    const struct dppd_shared_count_spec *spec, struct dppd_flow_shared_count_info *info);
int dppd_flow_shared_counts_seed(struct dppd_flow_shared_counts *counts, uint64_t next_generation);
int dppd_flow_shared_counts_get(struct dppd_flow_shared_counts *counts, uint64_t id, uint64_t generation,
    struct dppd_flow_shared_count_info *info);
int dppd_flow_shared_counts_query(struct dppd_flow_shared_counts *counts, uint64_t id, uint64_t generation,
    uint64_t *hits, uint64_t *bytes);
int dppd_flow_shared_counts_remove(struct dppd_flow_shared_counts *counts, uint64_t id, uint64_t generation);
/** 解析整条规则的精确共享引用，普通规则成功返回空 binding 和零凭据 */
int dppd_flow_shared_counts_bind(struct dppd_flow_shared_counts *counts, const struct dppd_rule *rule,
    struct dppd_flow_binding *binding, struct dppd_flow_shared_count_lease *lease);
int dppd_flow_shared_counts_unbind(struct dppd_flow_shared_counts *counts,
    const struct dppd_flow_shared_count_lease *lease);
struct dppd_flow_shared_count_driver dppd_native_flow_shared_count_driver(void);

#endif
