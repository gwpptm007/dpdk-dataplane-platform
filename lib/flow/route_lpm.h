#ifndef DPPD_ROUTE_LPM_H
#define DPPD_ROUTE_LPM_H
#include <stdint.h>
int dppd_route_lookup(uint32_t dst_be, uint32_t *nh_be, uint16_t *out_port);
#endif
