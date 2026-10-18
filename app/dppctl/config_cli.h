#ifndef DPPCTL_CONFIG_CLI_H
#define DPPCTL_CONFIG_CLI_H

#include "dppd/management.h"
int dppctl_config_build(int argc, char **argv, struct dppd_management_request *request,
    int (*rule)(int, char **, struct dppd_management_request *),
    void (*initialize)(struct dppd_management_request *, enum dppd_management_operation));
int dppctl_config_print(const struct dppd_management_response *response);

#endif
