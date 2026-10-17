#ifndef DPPCTL_ROUTING_CLI_H
#define DPPCTL_ROUTING_CLI_H

#include "dppd/management.h"

/** 未识别的命令返回 ENOSYS，识别后即使参数错误也不能转到另一种命令解释 */
int dppctl_routing_build(int argc, char **argv, struct dppd_management_request *request,
    void (*initialize)(struct dppd_management_request *, enum dppd_management_operation));
bool dppctl_routing_print(const struct dppd_management_response *response);
void dppctl_routing_usage(const char *program);

#endif
