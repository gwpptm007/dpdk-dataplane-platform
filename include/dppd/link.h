#ifndef DPPD_LINK_H
#define DPPD_LINK_H

enum dppd_link_state {
    DPPD_LINK_UNKNOWN = 0,
    DPPD_LINK_UP,
    DPPD_LINK_DOWN,
    DPPD_LINK_UNSUPPORTED,
};

static inline const char *dppd_link_state_name(unsigned int state)
{
    switch (state) {
    case DPPD_LINK_UP: return "up";
    case DPPD_LINK_DOWN: return "down";
    case DPPD_LINK_UNSUPPORTED: return "unsupported";
    default: return "unknown";
    }
}

#endif
