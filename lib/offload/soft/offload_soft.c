#include "backend.h"

int dppd_soft_backend_selftest(void)
{
    return dppd_offload_backend_get(DPPD_BACKEND_SOFT) != 0 ? 0 : -1;
}
