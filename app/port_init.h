#ifndef DPPD_PORT_INIT_H
#define DPPD_PORT_INIT_H

#include <stdint.h>
#include <inttypes.h>
struct dppd_app_config;

int dppd_port_init(uint16_t port_id, const struct dppd_app_config *cfg);
int dppd_port_start(uint16_t port_id);
void dppd_port_stop(uint16_t port_id);
void *dppd_pktmbuf_pool_get(void);
uint64_t dppd_port_tx_offloads_get(void);
const struct dppd_app_config *dppd_port_cfg_get(void);

#endif
