#ifndef DPPCTL_TRANSFER_CLI_H
#define DPPCTL_TRANSFER_CLI_H

#include "dppd/management.h"

int dppctl_transfer_build(int argc, char **argv, struct dppd_management_request *request,
    void (*initialize)(struct dppd_management_request *, enum dppd_management_operation));
/** 返回零表示其他命令，返回一表示完整输出，损坏响应返回 EPROTO 供主入口报告失败 */
int dppctl_transfer_print(const struct dppd_management_response *response);
void dppctl_transfer_usage(const char *program);

#endif
