#ifndef DPPD_BACKEND_H
#define DPPD_BACKEND_H

#include <stdint.h>
#include "flow_obj.h"

struct dppd_offload_ops {
    const char *name;
    int (*install)(uint16_t port_id, const struct dppd_flow_obj *flow, void **handle_out);
    int (*remove)(uint16_t port_id, void *handle);
};

const struct dppd_offload_ops *dppd_offload_backend_get(enum dppd_offload_backend backend);

#endif
