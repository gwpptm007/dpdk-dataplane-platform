#ifndef DPPD_PIPELINE_FWD_H
#define DPPD_PIPELINE_FWD_H

#include <stdint.h>
#include "dppd/flow.h"

struct dppd_parse_result;
struct dppd_pipeline_state {
    uint32_t next_hop_be;
    uint16_t out_port;
};

enum dppd_pipeline_path dppd_select_pipeline_path(const struct dppd_parse_result *res);
int dppd_pipeline_forward(const struct dppd_parse_result *res);
const struct dppd_pipeline_state *dppd_pipeline_last_state(void);

#endif
