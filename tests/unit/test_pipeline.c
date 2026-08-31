#include <assert.h>
#include <string.h>
#include "dppd/pipeline.h"

int main(void)
{
    struct dppd_config config;
    struct dppd_forwarding_snapshot snapshot;
    struct dppd_pipeline_decision decision;
    struct dppd_packet packet;
    uint16_t peer;

    memset(&config, 0, sizeof(config));
    config.ports[0] = 3;
    config.ports[1] = 7;
    config.ports[2] = 8;
    config.ports[3] = 9;
    config.nb_ports = 4;
    assert(dppd_snapshot_build_port_pairs(&config, &snapshot) == 0);
    assert(snapshot.generation == 1);
    assert(dppd_snapshot_lookup_peer(&snapshot, 3, &peer) == 0 && peer == 7);
    assert(dppd_snapshot_lookup_peer(&snapshot, 7, &peer) == 0 && peer == 3);
    assert(dppd_snapshot_lookup_peer(&snapshot, 8, &peer) == 0 && peer == 9);
    assert(dppd_snapshot_lookup_peer(&snapshot, 99, &peer) != 0);

    memset(&packet, 0, sizeof(packet));
    dppd_pipeline_decide(&snapshot, NULL, 3, DPPD_PARSE_OK, &packet, &decision);
    assert(decision.action == DPPD_PACKET_FORWARD);
    assert(decision.egress_port == 7);

    dppd_pipeline_decide(&snapshot, NULL, 3, DPPD_PARSE_MALFORMED, &packet, &decision);
    assert(decision.action == DPPD_PACKET_DROP);
    assert(decision.drop_reason == DPPD_DROP_MALFORMED);

    dppd_pipeline_decide(&snapshot, NULL, 99, DPPD_PARSE_OK, &packet, &decision);
    assert(decision.action == DPPD_PACKET_DROP);
    assert(decision.drop_reason == DPPD_DROP_NO_ROUTE);
    return 0;
}
