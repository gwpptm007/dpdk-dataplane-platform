#include "backend.h"
#include <stddef.h>

static int dppd_install_stub(uint16_t port_id, const struct dppd_flow_obj *flow, void **handle_out)
{
    (void)port_id;
    (void)flow;
    if (handle_out != NULL)
        *handle_out = 0;
    return 0;
}

static int dppd_remove_stub(uint16_t port_id, void *handle)
{
    (void)port_id;
    (void)handle;
    return 0;
}

static const struct dppd_offload_ops g_soft_ops = {
    .name = "soft",
    .install = dppd_install_stub,
    .remove = dppd_remove_stub,
};

static const struct dppd_offload_ops g_rte_flow_ops = {
    .name = "rte_flow",
    .install = dppd_install_stub,
    .remove = dppd_remove_stub,
};

static const struct dppd_offload_ops g_fdir_ops = {
    .name = "fdir",
    .install = dppd_install_stub,
    .remove = dppd_remove_stub,
};

const struct dppd_offload_ops *dppd_offload_backend_get(enum dppd_offload_backend backend)
{
    switch (backend) {
    case DPPD_BACKEND_SOFT:
        return &g_soft_ops;
    case DPPD_BACKEND_RTE_FLOW:
        return &g_rte_flow_ops;
    case DPPD_BACKEND_FDIR:
        return &g_fdir_ops;
    default:
        return 0;
    }
}
