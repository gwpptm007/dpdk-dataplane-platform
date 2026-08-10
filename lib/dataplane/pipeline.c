#include "dppd/pipeline.h"

#include <string.h>

void dppd_pipeline_decide(const struct dppd_forwarding_snapshot *snapshot,
                          uint16_t ingress_port,
                          int parse_status,
                          const struct dppd_packet *packet,
                          struct dppd_pipeline_decision *decision)
{
    (void)packet;
    memset(decision, 0, sizeof(*decision));

    if (parse_status == DPPD_PARSE_MALFORMED) {
        decision->action = DPPD_PACKET_DROP;
        decision->drop_reason = DPPD_DROP_MALFORMED;
        return;
    }
    if (dppd_snapshot_lookup_peer(snapshot, ingress_port, &decision->egress_port) != 0) {
        decision->action = DPPD_PACKET_DROP;
        decision->drop_reason = DPPD_DROP_NO_ROUTE;
        return;
    }

    decision->action = DPPD_PACKET_FORWARD;
    decision->drop_reason = DPPD_DROP_NONE;
}

