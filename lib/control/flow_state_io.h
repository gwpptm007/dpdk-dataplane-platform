#ifndef DPPD_FLOW_STATE_IO_H
#define DPPD_FLOW_STATE_IO_H

#include "dppd/flow_state.h"

/** 公共离线保存和在线服务共用同一写锁，在线保存借用已有锁而不再次锁住自己 */
int dppd_flow_state_lock(const char *path);
int dppd_flow_state_path(const char *path, char **output);
int dppd_flow_state_write(const char *path, int guard, const struct dppd_flow_state *state);

#endif
