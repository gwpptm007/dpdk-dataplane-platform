#include "backend.h"

int dppd_rte_flow_backend_selftest(void)
{
    return dppd_offload_backend_get(DPPD_BACKEND_RTE_FLOW) != 0 ? 0 : -1;
}
