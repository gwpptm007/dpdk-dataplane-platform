#include "dppd/pipeline.h"
#include "dppd/software_backend.h"

#include <string.h>

/**
 * 把解析结果、软件策略和静态端口配对合成为处理决定，不在这里发送或释放报文
 * 畸形报文最先丢弃；不支持解析的协议并不等于畸形，仍可按已有字段匹配或查路由
 */
void dppd_pipeline_decide(const struct dppd_forwarding_snapshot *snapshot,
                          struct dppd_software_backend *software_backend,
                          uint16_t ingress_port,
                          int parse_status,
                          const struct dppd_packet *packet,
                          struct dppd_pipeline_decision *decision)
{
    (void)packet;
    memset(decision, 0, sizeof(*decision));

    /**
     * 解析失败的 mbuf 没有可靠 L3/L4 字段，绝不能交给软件 classifier 继续解释；否则
     * 掩码比较可能把未初始化或截断字段误判为命中。这一分支优先于所有规则和路由。
     */
    if (parse_status == DPPD_PARSE_MALFORMED) {
        decision->action = DPPD_PACKET_DROP;
        decision->drop_reason = DPPD_DROP_MALFORMED;
        return;
    }
    if (software_backend != NULL && packet != NULL) {
        struct dppd_software_decision software_decision;

        /**
         * classifier 只给出策略结论，不负责决定 egress。DROP 立即返回，保证软件规则
         * 不会因后续路由查找失败而被重写为 NO_ROUTE；MARK 则随正常转发结果向下传递。
         */
        /** COUNT 在规则命中时已累计，因此后续无路由或发送失败也不会撤销这次计数 */
        dppd_software_backend_decide(software_backend, ingress_port, packet,
                                     &software_decision);
        if (software_decision.drop) {
            decision->action = DPPD_PACKET_DROP;
            decision->drop_reason = DPPD_DROP_POLICY;
            return;
        }
        decision->has_mark = software_decision.has_mark;
        decision->mark_id = software_decision.mark_id;
    }
    /** 没有 policy DROP 的报文才走静态 peer 查表，保持基线 dataplane 的转发拓扑不变。 */
    if (dppd_snapshot_lookup_peer(snapshot, ingress_port, &decision->egress_port) != 0) {
        decision->action = DPPD_PACKET_DROP;
        decision->drop_reason = DPPD_DROP_NO_ROUTE;
        return;
    }

    decision->action = DPPD_PACKET_FORWARD;
    decision->drop_reason = DPPD_DROP_NONE;
}
