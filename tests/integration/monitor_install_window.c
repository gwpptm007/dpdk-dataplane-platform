#include <signal.h>
#include "dppd/recovery_guard.h"

int __real_dppd_recovery_guard_begin(struct dppd_recovery_guard *guard, uint16_t port, uint64_t rule,
    uint64_t generation, uint64_t *attempt);
int __wrap_dppd_recovery_guard_begin(struct dppd_recovery_guard *guard, uint16_t port, uint64_t rule,
    uint64_t generation, uint64_t *attempt);
int __real_dppd_recovery_guard_begin_owned(struct dppd_recovery_guard *guard, uint16_t port, uint64_t rule,
    uint64_t generation, uint64_t *attempt, uint8_t cookie[16], const struct dppd_tap_content *expected);
int __wrap_dppd_recovery_guard_begin_owned(struct dppd_recovery_guard *guard, uint16_t port, uint64_t rule,
    uint64_t generation, uint64_t *attempt, uint8_t cookie[16], const struct dppd_tap_content *expected);

/** 第一次监护检查之后、安装意图写盘之前暂停，仅测试入口带此包装 */
int __wrap_dppd_recovery_guard_begin(struct dppd_recovery_guard *guard, uint16_t port, uint64_t rule,
    uint64_t generation, uint64_t *attempt)
{
    raise(SIGSTOP);
    return __real_dppd_recovery_guard_begin(guard, port, rule, generation, attempt);
}

int __wrap_dppd_recovery_guard_begin_owned(struct dppd_recovery_guard *guard, uint16_t port, uint64_t rule,
    uint64_t generation, uint64_t *attempt, uint8_t cookie[16], const struct dppd_tap_content *expected)
{
    raise(SIGSTOP);
    return __real_dppd_recovery_guard_begin_owned(guard, port, rule, generation, attempt, cookie, expected);
}
