#ifndef DPPD_COMMON_H
#define DPPD_COMMON_H

#include <stdbool.h>
#include <stdint.h>

#define DPPD_NAME_MAX 64
#define DPPD_MAX_PORTS 16
#define DPPD_MAX_QUEUES 128

struct dppd_mac_addr {
    uint8_t addr_bytes[6];
};

#endif
