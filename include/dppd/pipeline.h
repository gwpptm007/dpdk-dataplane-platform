#ifndef DPPD_PIPELINE_H
#define DPPD_PIPELINE_H

#include <stdint.h>
#include "dppd/config.h"
#include "dppd/packet.h"

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
};

int dppd_snapshot_build_port_pairs(const struct dppd_config *cfg,
                                   struct dppd_forwarding_snapshot *snapshot);
int dppd_snapshot_lookup_peer(const struct dppd_forwarding_snapshot *snapshot,
                              uint16_t ingress_port,
                              uint16_t *egress_port);
void dppd_pipeline_decide(const struct dppd_forwarding_snapshot *snapshot,
                          uint16_t ingress_port,
                          int parse_status,
                          const struct dppd_packet *packet,
                          struct dppd_pipeline_decision *decision);

#endif

