#include "flow_obj.h"
#include <stddef.h>

int dppd_rte_flow_build(uint16_t port_id, const struct dppd_flow_obj *flow, void **flow_out)
{
    (void)port_id;
    if (flow == NULL)
        return -1;
    if (flow_out != NULL)
        *flow_out = 0;
    return 0;
}
