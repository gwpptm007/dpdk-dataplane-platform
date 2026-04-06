#ifndef DPPD_WORKER_H
#define DPPD_WORKER_H

#include <stdint.h>

int dppd_worker_main(void *arg);
void dppd_worker_poll_once(uint16_t port_id, uint16_t queue_id);

#endif
