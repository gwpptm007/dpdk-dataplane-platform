#include "route_lpm.h"
#include <stddef.h>

int dppd_route_lookup(uint32_t dst_be, uint32_t *nh_be, uint16_t *out_port)
{
    if (nh_be == NULL || out_port == NULL)
        return -1;

    /*
     * Phase 1 baseline:
     * treat the destination IP itself as next hop and always transmit from port 0.
     * This keeps the data path real, while control-plane driven route tables will be
     * introduced in the next iteration.
     */
    *nh_be = dst_be;
    *out_port = 0;
    return 0;
}
