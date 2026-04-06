#ifndef DPPD_ACTION_H
#define DPPD_ACTION_H
#include <stdint.h>

enum dppd_action_type {
    DPPD_ACTION_RSS = 0,
    DPPD_ACTION_QUEUE,
    DPPD_ACTION_DROP,
    DPPD_ACTION_COUNT,
    DPPD_ACTION_MARK,
    DPPD_ACTION_NAT,
    DPPD_ACTION_REDIRECT_PORT,
};

struct dppd_action {
    enum dppd_action_type type;
    uint32_t queue_id;
    uint32_t counter_id;
    uint32_t mark_id;
};

#endif
