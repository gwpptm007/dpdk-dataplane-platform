#ifndef DPPD_CTRL_H
#define DPPD_CTRL_H

#include "dppd/app.h"

int dppd_parse_args(int argc, char **argv, struct dppd_app_config *cfg);
void dppd_dump_config(const struct dppd_app_config *cfg);

#endif
