#define _POSIX_C_SOURCE 200809L
#include "dppd/software_backend.h"

#include <stdlib.h>
#include <time.h>

void __real_dppd_software_backend_worker_quiescent(struct dppd_software_backend *backend, unsigned int worker_id);
void __wrap_dppd_software_backend_worker_quiescent(struct dppd_software_backend *backend, unsigned int worker_id);

/** 仅测试程序在真实 worker 首轮扫描后暂停，正式 dppd 不链接此故障边界 */
void __wrap_dppd_software_backend_worker_quiescent(struct dppd_software_backend *backend, unsigned int worker_id)
{
    __real_dppd_software_backend_worker_quiescent(backend, worker_id);
    if (worker_id == 0 && getenv("DPPD_TEST_WORKER_STALL") != NULL) {
        struct timespec pause = {.tv_nsec = 10000000L};
        for (;;)
            nanosleep(&pause, NULL);
    }
}
