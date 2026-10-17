#define _GNU_SOURCE
#include <signal.h>
#include <unistd.h>
#define main business_main
#include "../../app/dppd/main.c"
#undef main

int __real_dppd_recovery_guard_created(struct dppd_recovery_guard *guard, uint64_t attempt, int create_error,
    enum dppd_recovery_evidence evidence, int observation_error, const struct dppd_recovery_filter *candidate);

/** 原生创建已经完成，暂停在结果写盘前，保证测试留下真正的 intent 窗口 */
int __wrap_dppd_recovery_guard_created(struct dppd_recovery_guard *guard, uint64_t attempt, int create_error,
    enum dppd_recovery_evidence evidence, int observation_error, const struct dppd_recovery_filter *candidate)
{
    char path[DPPD_STATE_PATH_CAPACITY + 32];
    if (create_error == 0 && snprintf(path, sizeof(path), "%s.business-window", guard->path) < (int)sizeof(path) &&
        unlink(path) == 0) {
        printf("business-window phase=intent attempt=%" PRIu64 "\n", attempt);
        fflush(stdout);
        raise(SIGSTOP);
    }
    return __real_dppd_recovery_guard_created(guard, attempt, create_error, evidence, observation_error, candidate);
}

int main(int argc, char **argv)
{
    return business_main(argc, argv);
}
