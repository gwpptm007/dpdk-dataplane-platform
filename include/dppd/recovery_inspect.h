#ifndef DPPD_RECOVERY_INSPECT_H
#define DPPD_RECOVERY_INSPECT_H

#include "dppd/recovery_guard.h"

#define DPPD_RECOVERY_FILTER_LIMIT 256U
#define DPPD_RECOVERY_KIND_SIZE 32U

/** TC 句柄用于定位当前看到的对象，不代表项目拥有它，也不能由业务规则 ID 推算 */
struct dppd_recovery_filter {
    uint32_t parent, handle, chain;
    uint16_t priority, protocol;
    char kind[DPPD_RECOVERY_KIND_SIZE];
};

enum dppd_recovery_inspection_state {
    DPPD_INSPECT_UNAVAILABLE,
    DPPD_INSPECT_UNSUPPORTED,
    DPPD_INSPECT_NO_IDENTITY,
    DPPD_INSPECT_CONTEXT_MISMATCH,
    DPPD_INSPECT_IDENTITY_MISMATCH,
    DPPD_INSPECT_ABSENT,
    DPPD_INSPECT_PRESENT
};

/** 只检查本地 TAP 的 multiq 和 ingress，任何查询错误都丢弃不完整列表 */
struct dppd_recovery_inspection {
    enum dppd_recovery_inspection_state state;
    uint32_t count;
    struct dppd_recovery_filter filters[DPPD_RECOVERY_FILTER_LIMIT];
};

/** 在驱动安装前读取内核身份，无法确定为 TAP 时拒绝保存错误的定位信息 */
int dppd_recovery_tap_identity(uint32_t ifindex, struct dppd_recovery_identity *identity);
/** 只读查询，不删除内核对象、不确认清理，也不承诺多次内核查询构成原子快照 */
int dppd_recovery_inspect(const struct dppd_recovery_port *port,
    struct dppd_recovery_inspection *inspection);
/** 返回稳定的诊断名称，便于离线工具和测试区分缺失、未知与上下文不符 */
const char *dppd_recovery_inspection_name(enum dppd_recovery_inspection_state state);

#endif
