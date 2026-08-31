#ifndef DPPD_PIPELINE_H
#define DPPD_PIPELINE_H

#include <stdint.h>
#include "dppd/config.h"
#include "dppd/packet.h"

struct dppd_software_backend;

struct dppd_port_peer {
    uint16_t ingress_port;
    uint16_t egress_port;
};

struct dppd_forwarding_snapshot {
    uint64_t generation;
    uint16_t nb_peers;
    struct dppd_port_peer peers[DPPD_MAX_PORTS];
};

enum dppd_packet_action {
    DPPD_PACKET_DROP = 0,
    DPPD_PACKET_FORWARD,
};

enum dppd_drop_reason {
    DPPD_DROP_NONE = 0,
    DPPD_DROP_MALFORMED,
    DPPD_DROP_NO_ROUTE,
    DPPD_DROP_POLICY,
};

struct dppd_pipeline_decision {
    enum dppd_packet_action action;
    enum dppd_drop_reason drop_reason;
    uint16_t egress_port;
    /*
     * 软件 MARK 使用与硬件 flow mark 相同的 mbuf fdir id 槽位。只有 action 为 FORWARD
     * 时 worker 才会把它写入 mbuf；策略 DROP 已经终结报文，保留 mark 没有可观察价值。
     */
    bool has_mark;
    uint32_t mark_id;
};

int dppd_snapshot_build_port_pairs(const struct dppd_config *cfg,
                                   struct dppd_forwarding_snapshot *snapshot);
int dppd_snapshot_lookup_peer(const struct dppd_forwarding_snapshot *snapshot,
                              uint16_t ingress_port,
                              uint16_t *egress_port);
/**
 * 计算单个报文的最终处理结果，优先级严格为：畸形报文丢弃 > 软件策略 DROP > port-pair
 * 路由 > 无路由丢弃。software_backend 为空时保留历史静态转发行为；非空却无匹配时同样
 * 继续查路由。调用者负责在合适的 QSBR 静默点之外调用，不得在本函数中假定可回收旧规则。
 */
void dppd_pipeline_decide(const struct dppd_forwarding_snapshot *snapshot,
                          struct dppd_software_backend *software_backend,
                          uint16_t ingress_port,
                          int parse_status,
                          const struct dppd_packet *packet,
                          struct dppd_pipeline_decision *decision);

#endif
