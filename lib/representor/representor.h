#ifndef DPPD_REPRESENTOR_H
#define DPPD_REPRESENTOR_H
#include <stdint.h>
int dppd_representor_probe(uint16_t pf_port_id);
int dppd_representor_bind_vf(uint16_t repr_port_id, uint16_t vf_id);
#endif
