#ifndef DPPD_FLOW_TEMPLATE_H
#define DPPD_FLOW_TEMPLATE_H

#include "dppd/admission.h"
#include "dppd/rule.h"

#define DPPD_FLOW_TEMPLATE_LIMIT 64U
#define DPPD_FLOW_TEMPLATE_LEASE_LIMIT 65536U

/** 示例给出层次、掩码和固定动作，匹配值可变，置位的动作参数也可变 */
struct dppd_flow_template_spec {
    struct dppd_rule example;
    uint32_t capacity, dynamic_actions;
};

/** 三种句柄分开保存，创建或逆序销毁失败时仍能定位已经存在的每个对象 */
struct dppd_flow_template_parts {
    void *pattern, *actions, *table;
};

struct dppd_flow_template_driver {
    void *context;
    int (*create)(void *context, const struct dppd_flow_template_spec *spec, struct dppd_flow_template_parts *parts);
    int (*destroy)(void *context, uint16_t port_id, struct dppd_flow_template_parts *parts);
};

struct dppd_flow_template_id {
    uint64_t owner, serial;
    uint32_t slot;
};

/** 每次借用拥有独立凭据，重复归还、跨实例归还和旧槽位凭据不会减少新引用 */
struct dppd_flow_template_lease {
    uint64_t owner, serial;
    uint32_t slot;
};

struct dppd_flow_template_info {
    struct dppd_flow_template_spec spec;
    struct dppd_flow_template_parts parts;
    uint32_t references;
    bool residual;
};

struct dppd_flow_templates;

/** 不传驱动表示正式软件模板；配额对象须晚于模板库销毁，关闭前须停止所有调用者 */
int dppd_flow_templates_create(struct dppd_admission *admission, uint32_t lease_capacity,
    const struct dppd_flow_template_driver *driver, struct dppd_flow_templates **output);
int dppd_flow_templates_close(struct dppd_flow_templates **templates);
/** 失败后若残留句柄则返回 EUCLEAN 并输出可恢复的身份，普通失败保持输出不变 */
int dppd_flow_template_create(struct dppd_flow_templates *templates, const struct dppd_flow_template_spec *spec,
    struct dppd_flow_template_id *id);
int dppd_flow_template_get(struct dppd_flow_templates *templates, const struct dppd_flow_template_id *id,
    struct dppd_flow_template_info *info);
/** 有引用时拒绝删除，残留模板只允许重试销毁，不允许继续借用 */
int dppd_flow_template_destroy(struct dppd_flow_templates *templates, const struct dppd_flow_template_id *id);
int dppd_flow_template_acquire(struct dppd_flow_templates *templates, const struct dppd_flow_template_id *id,
    const struct dppd_rule *rule, struct dppd_flow_template_lease *lease);
int dppd_flow_template_release(struct dppd_flow_templates *templates, const struct dppd_flow_template_lease *lease);
/** 比较语义字段，不比较结构体填充字节，也不把匹配值误当成模板掩码 */
bool dppd_flow_template_matches(const struct dppd_flow_template_spec *spec, const struct dppd_rule *rule);

/** 24.11 原生适配不自动改变端口模式，调用方须在 ethdev configure 后、任何 flow 创建前执行 */
int dppd_native_flow_configure(uint16_t port_id, uint16_t queues, uint32_t depth, uint32_t counters, uint32_t aging);
struct dppd_flow_template_driver dppd_native_flow_template_driver(void);

#endif
