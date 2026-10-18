#ifndef DPPD_ADMISSION_H
#define DPPD_ADMISSION_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/config.h"

#define DPPD_ADMISSION_MAX_DEMANDS 64U
#define DPPD_ADMISSION_MAX_TICKETS 65536U
#define DPPD_ADMISSION_GLOBAL_PORT UINT16_MAX

/** 逻辑规则与实际流对象分别计费，更新时短暂共存的新旧流不能挤占成一份资源 */
enum dppd_resource_kind {
    DPPD_RESOURCE_RULE = 0,
    DPPD_RESOURCE_HARDWARE_FLOW,
    DPPD_RESOURCE_SOFTWARE_RULE,
    DPPD_RESOURCE_TEMPLATE_TABLE,
    DPPD_RESOURCE_SHARED_ACTION,
    DPPD_RESOURCE_FLOW_QUEUE,
    DPPD_RESOURCE_INFLIGHT_OP,
    DPPD_RESOURCE_COUNTER,
    DPPD_RESOURCE_AGING_OBJECT,
    DPPD_RESOURCE_KIND_COUNT,
};

struct dppd_port_quota {
    uint16_t port_id;
    uint64_t limits[DPPD_RESOURCE_KIND_COUNT];
};

/** 零额度明确禁止该资源，UINT64_MAX 也仍受整数溢出检查，不表示可以无限申请 */
struct dppd_admission_config {
    uint64_t limits[DPPD_RESOURCE_KIND_COUNT];
    uint32_t ticket_capacity, port_count;
    struct dppd_port_quota ports[DPPD_MAX_PORTS];
};

/** 指定端口时同时扣全局与端口额度，GLOBAL_PORT 只扣全局额度，重复需求会先合并 */
struct dppd_resource_demand {
    enum dppd_resource_kind kind;
    uint16_t port_id;
    uint64_t amount;
};

/** 编号同时绑定分配器实例与槽位，其他实例或已释放后复用的旧凭据都不能操作新资源 */
struct dppd_resource_ticket {
    uint64_t owner, serial;
    uint32_t slot;
};

struct dppd_quota_usage {
    uint64_t limits[DPPD_RESOURCE_KIND_COUNT];
    uint64_t reserved[DPPD_RESOURCE_KIND_COUNT];
    uint64_t committed[DPPD_RESOURCE_KIND_COUNT];
};

struct dppd_admission_snapshot {
    struct dppd_quota_usage global;
    struct {
        uint16_t port_id;
        struct dppd_quota_usage usage;
    } ports[DPPD_MAX_PORTS];
    uint64_t revision, reservations, rejections;
    uint32_t port_count, ticket_capacity, held_tickets, residual_tickets;
};

struct dppd_admission;

/** 配额对象不调用驱动，output 必须指向空指针，避免覆盖仍持有额度的旧实例 */
int dppd_admission_create(const struct dppd_admission_config *config, struct dppd_admission **output);
/** 调用方先停止所有使用者，仍有预留、安装或残留凭据时拒绝关闭 */
int dppd_admission_close(struct dppd_admission **admission);
/** 全批预检后一次预留，任何一项超限都不占额度；失败时输出凭据保持原样 */
int dppd_admission_reserve(struct dppd_admission *admission, const struct dppd_resource_demand *demands, uint32_t count,
    struct dppd_resource_ticket *ticket);
/** 安装成功才把预留移到已提交，不会释放任何额度，重复提交返回 EALREADY */
int dppd_admission_commit(struct dppd_admission *admission, const struct dppd_resource_ticket *ticket);
/** 只能取消尚未提交的预留，或释放已经确认清除的实际对象，残留凭据必须走专用恢复入口 */
int dppd_admission_release(struct dppd_admission *admission, const struct dppd_resource_ticket *ticket);
/** 未知驱动结果或补偿失败后锁住凭据，预留及实际资源仍继续占额度 */
int dppd_admission_mark_residual(struct dppd_admission *admission, const struct dppd_resource_ticket *ticket);
/** 只能在上层确认驱动清理完成后调用，不能用超时、设备停止或对象失联代替清理证明 */
int dppd_admission_release_recovered(struct dppd_admission *admission, const struct dppd_resource_ticket *ticket);
int dppd_admission_snapshot(struct dppd_admission *admission, struct dppd_admission_snapshot *snapshot);
/** 只能改额度且必须提供精确修订号，不能缩到当前预留与实际占用之下 */
int dppd_admission_set_limits(struct dppd_admission *admission, const struct dppd_admission_config *config,
    uint64_t expected_revision);

#endif
