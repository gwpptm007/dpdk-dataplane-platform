#include "pipeline_fwd.h"
#include "acl_rule.h"
#include "route_lpm.h"
#include <stddef.h>
#include "parser.h"

/*
 * pipeline_fwd.c 当前先做最小的软件转发判决：
 * - ARP 允许放行
 * - IPv4/UDP 进入 ACL + 路由查表路径
 * - 其他流量先不处理
 * 这样既能形成真正的 Phase 1 数据面闭环，又不会过早把控制面做复杂。
 */

static struct dppd_pipeline_state g_last_pipeline_state;

const struct dppd_pipeline_state *dppd_pipeline_last_state(void)
{
    return &g_last_pipeline_state;
}

enum dppd_pipeline_path dppd_select_pipeline_path(const struct dppd_parse_result *res)
{
    if (res == NULL)
        return DPPD_PATH_SW;

    if (res->meta.is_arp || res->meta.is_udp)
        return DPPD_PATH_SW;

    return DPPD_PATH_TRANSFER;
}

int dppd_pipeline_forward(const struct dppd_parse_result *res)
{
    struct dppd_acl_tuple tuple;

    if (res == NULL)
        return DPPD_FWD_DROP;

    if (res->meta.is_arp)
        return DPPD_FWD_TX;

    if (!res->meta.is_ipv4 || !res->meta.is_udp)
        return DPPD_FWD_DROP;

    tuple.src_ip_be = res->ipv4_src_be;
    tuple.dst_ip_be = res->ipv4_dst_be;
    tuple.src_port_be = res->l4_src_port_be;
    tuple.dst_port_be = res->l4_dst_port_be;
    tuple.proto = res->meta.l4_proto;

    if (dppd_acl_match(&tuple) != 0)
        return DPPD_FWD_DROP;

    if (dppd_route_lookup(res->ipv4_dst_be,
                          &g_last_pipeline_state.next_hop_be,
                          &g_last_pipeline_state.out_port) != 0)
        return DPPD_FWD_DROP;

    return DPPD_FWD_TX;
}
