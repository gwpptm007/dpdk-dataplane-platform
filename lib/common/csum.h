#ifndef DPPD_CSUM_H
#define DPPD_CSUM_H
#include <stdint.h>
uint16_t dppd_ipv4_csum16(const void *buf, uint32_t len);
#endif
