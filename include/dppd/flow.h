#ifndef DPPD_FLOW_H
#define DPPD_FLOW_H

#include <stdint.h>

enum dppd_pipeline_path {
    DPPD_PATH_SW = 0,
    DPPD_PATH_HW_RTE,
    DPPD_PATH_TRANSFER,
};

enum dppd_fwd_decision {
    DPPD_FWD_DROP = 0,
    DPPD_FWD_TX,
    DPPD_FWD_OFFLOAD,
};

#endif
