#ifndef DPPCTL_ROUTING_CLI_H
#define DPPCTL_ROUTING_CLI_H

#include "dppd/management.h"

/** 同一文件解析器供独立路由命令与 managed 持久请求使用，解析失败时不发送管理消息 */
int dppctl_routing_read(const char *path, struct dppd_ipv4_configuration *configuration);

/** 未识别的命令返回 ENOSYS，识别后即使参数错误也不能转到另一种命令解释 */
int dppctl_routing_build(int argc, char **argv, struct dppd_management_request *request,
    void (*initialize)(struct dppd_management_request *, enum dppd_management_operation));
bool dppctl_routing_print(const struct dppd_management_response *response);
void dppctl_routing_usage(const char *program);

#endif
