#include "dppd/pipeline.h"
#include "dppd/software_backend.h"

#include <string.h>
#include <errno.h>

/**
 * 判断报文应丢弃还是转发，将原因或出口写入 decision，实际收发由 worker 执行
 * 判定顺序为畸形检查、软件策略、三层路由或端口配对，UNSUPPORTED 不等同于畸形
 */
void dppd_pipeline_decide_counted(const struct dppd_forwarding_snapshot *snapshot,
    struct dppd_software_backend *software_backend, struct dppd_ipv4_router *router, uint16_t ingress_port,
    int parse_status, const struct dppd_packet *packet, struct dppd_pipeline_decision *decision,
    struct dppd_software_count_batch *counts)
{
    /** 默认丢弃，后续检查通过才设为转发 */
    memset(decision, 0, sizeof(*decision));

    /** 畸形报文字段不可信，直接判定丢弃，禁止继续匹配规则 */
    if (parse_status == DPPD_PARSE_MALFORMED) {
        decision->action = DPPD_PACKET_DROP;
        decision->drop_reason = DPPD_DROP_MALFORMED;
        return;
    }
    /** 软件后端存在时检查规则动作，后端为空则继续后面的出口选择 */
    if (software_backend != NULL && packet != NULL) {
        struct dppd_software_decision software_decision;

        /**
         * software_backend.c 的 dppd_software_backend_decide_counted 匹配当前软件规则
         * software_decision 只记录规则动作，decision 还要结合后续路由或端口配对得到处理决定
         * 命中 DROP 时立即返回，保留策略丢弃原因，其余情况继续选择出口并携带 MARK
         */
        /** COUNT 按规则命中累计或归并，后续无路由或发送失败不撤销本次命中 */
        dppd_software_backend_decide_counted(software_backend, ingress_port, packet, &software_decision, counts);
        if (software_decision.drop) {
            decision->action = DPPD_PACKET_DROP;
            decision->drop_reason = DPPD_DROP_POLICY;
            return;
        }
        decision->has_mark = software_decision.has_mark;
        decision->mark_id = software_decision.mark_id;
    }
    /**
     * 启用三层路由时按目的 IP 查询路由和邻居，成功或失败都在本分支返回
     * router 为空时跳过本分支，继续查询下面的静态端口配对
     */
    if (router != NULL) {
        if (packet == NULL || (packet->l3_type != DPPD_L3_IPV4 && packet->l3_type != DPPD_L3_IPV6)) {
            decision->drop_reason = DPPD_DROP_IPV4_UNSUPPORTED;
            return;
        }
        bool ipv6 = packet->l3_type == DPPD_L3_IPV6;
        int rc = ipv6 ? dppd_ipv6_router_lookup(router, packet->ipv6_dst, &decision->ipv4)
                      : dppd_ipv4_router_lookup(router, packet->ipv4_dst_be, &decision->ipv4);
        if (rc != 0) {
            decision->drop_reason = rc == -ENOENT ? DPPD_DROP_NO_ROUTE
                : rc == -EHOSTUNREACH             ? DPPD_DROP_NO_NEIGHBOR
                : ipv6                            ? DPPD_DROP_IPV6_UNSUPPORTED
                                                  : DPPD_DROP_IPV4_UNSUPPORTED;
            return;
        }
        decision->action = DPPD_PACKET_FORWARD;
        decision->egress_port = decision->ipv4.port_id;
        decision->routed = true;
        return;
    }
    /**
     * 未启用三层路由时按端口配对选择出口，路由失败不回退到端口配对
     * snapshot.c 的 dppd_snapshot_lookup_peer 查到时写入出口并返回零
     */
    if (dppd_snapshot_lookup_peer(snapshot, ingress_port, &decision->egress_port) != 0) {
        decision->action = DPPD_PACKET_DROP;
        decision->drop_reason = DPPD_DROP_NO_ROUTE;
        return;
    }

    decision->action = DPPD_PACKET_FORWARD;
    decision->drop_reason = DPPD_DROP_NONE;
}

void dppd_pipeline_decide_routed(const struct dppd_forwarding_snapshot *snapshot,
    struct dppd_software_backend *software_backend, struct dppd_ipv4_router *router, uint16_t ingress_port,
    int parse_status, const struct dppd_packet *packet, struct dppd_pipeline_decision *decision)
{
    dppd_pipeline_decide_counted(snapshot, software_backend, router, ingress_port, parse_status, packet, decision,
        NULL);
}

void dppd_pipeline_decide(const struct dppd_forwarding_snapshot *snapshot,
    struct dppd_software_backend *software_backend, uint16_t ingress_port, int parse_status,
    const struct dppd_packet *packet, struct dppd_pipeline_decision *decision)
{
    dppd_pipeline_decide_routed(snapshot, software_backend, NULL, ingress_port, parse_status, packet, decision);
}
