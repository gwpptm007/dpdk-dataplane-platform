#ifndef DPPCTL_JSON_H
#define DPPCTL_JSON_H

#include "dppd/management.h"

/** 在发送请求前确认 JSON 能表达该操作，防止写入已执行后才发现没有输出格式 */
bool dppctl_json_supported(enum dppd_management_operation operation);
/** JSON 独立于本机结构体布局，所有 64 位整数使用十进制字符串保留完整精度 */
int dppctl_json_response(const struct dppd_management_request *request,
    const struct dppd_management_response *response);

#endif
