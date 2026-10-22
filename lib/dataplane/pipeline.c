#include "dppd/pipeline.h"
#include "dppd/software_backend.h"

#include <string.h>
#include <errno.h>

/**
 * 判断报文应丢弃还是转发，将动作、原因和出口写入 decision
 * 无连接跟踪时的调用：worker.c/process_batch → routed_decide → 本函数
 * 返回：本函数 → routed_decide → process_batch，由后者按 decision 处理报文
 * 判定顺序为畸形检查、软件策略、三层路由或端口配对，UNSUPPORTED 不等同于畸形
 */
void dppd_pipeline_decide_counted(const struct dppd_forwarding_snapshot *snapshot,
    struct dppd_software_backend *software_backend, struct dppd_ipv4_router *router, uint16_t ingress_port,
    int parse_status, const struct dppd_packet *packet, struct dppd_pipeline_decision *decision,
    struct dppd_software_count_batch *counts)
{
    /**
     * memset 清零 → action = 0 → DROP，枚举定义在 pipeline.h
     * 初始保留 DROP，只有后续规则允许并选出出口才设置 FORWARD
     */
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
         * 软件规则 → software_decision；drop 为真时将最终 decision 设为 DROP/POLICY
         * 本例 IPv4 返回路径：本函数 → worker.c/routed_decide → process_batch
         *   → 更新丢弃统计 → 释放当前 mbuf → continue 下一个包
         * 未丢弃时携带 MARK 继续选出口；COUNT 按命中累计，后续失败不撤销
         */
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
     * router 非空 → 校验 IP 类型 → 按目的 IP 查询路由和下一跳邻居
     *   成功 → 写入 FORWARD、出口和 routed = true → 返回
     *   失败 → 保留默认 DROP，填写失败原因 → 返回
     * router 为空 → 查询下面的静态端口配对
     * 两种出口选择方式由 router 是否为空决定，路由失败不回退到端口配对
     */
    if (router != NULL) {
        if (packet == NULL || (packet->l3_type != DPPD_L3_IPV4 && packet->l3_type != DPPD_L3_IPV6)) {
            decision->drop_reason = DPPD_DROP_IPV4_UNSUPPORTED;
            return;
        }
        bool ipv6 = packet->l3_type == DPPD_L3_IPV6;
        /** 两种地址族共用 decision->ipv4 保存查询得到的出口、下一跳和目的 MAC */
        int rc = ipv6 ? dppd_ipv6_router_lookup(router, packet->ipv6_dst, &decision->ipv4)
                      : dppd_ipv4_router_lookup(router, packet->ipv4_dst_be, &decision->ipv4);
        /**
         * 查询失败时 action 仍为默认 DROP，这里把错误码转换成原因后返回 routed_decide
         * IPv6 示例：路由已命中，静态邻居未命中，rc 为 -EHOSTUNREACH
         *   decision->ipv4 已有出口和下一跳 → drop_reason 写为 NO_NEIGHBOR
         *   返回 worker.c/routed_decide → 若启用 ND，再尝试从动态邻居表取得 MAC
         * 此处的 DROP / NO_NEIGHBOR 尚未释放 mbuf，后续可能改为转发或等待邻居
         */
        if (rc != 0) {
            decision->drop_reason = rc == -ENOENT ? DPPD_DROP_NO_ROUTE
                : rc == -EHOSTUNREACH             ? DPPD_DROP_NO_NEIGHBOR
                : ipv6                            ? DPPD_DROP_IPV6_UNSUPPORTED
                                                  : DPPD_DROP_IPV4_UNSUPPORTED;
            return;
        }
        decision->action = DPPD_PACKET_FORWARD;
        decision->egress_port = decision->ipv4.port_id;
        /** 标记需按三层结果改写报文，实际改写和发送由后续调用方执行 */
        decision->routed = true;
        return;
    }
    /**
     * router 为空 → lib/control/snapshot.c 按入口查端口配对，写入 decision->egress_port
     * 查到后设置 FORWARD；未查到则 DROP/NO_ROUTE，三层路由失败不回退到此处
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
