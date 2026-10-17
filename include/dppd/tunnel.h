#ifndef DPPD_TUNNEL_H
#define DPPD_TUNNEL_H

#include "dppd/packet.h"

/**
 * 基础 IP 解析成功后识别 VXLAN、Geneve 和 GRE，输出只包含副本，不借用报文指针
 * 返回值只表示隧道观察结果，中转模式仍按外层策略处理，不作为解封装或内层放行许可
 * 分片不猜测隧道头，未知版本和关键选项明确返回 UNSUPPORTED，不递归解析嵌套隧道
 */
int dppd_tunnel_parse_buffer(const uint8_t *data, uint32_t available, uint32_t length, struct dppd_packet *packet);

#endif
