#include "dppd/pipeline.h"

#include <stddef.h>
#include <string.h>

int dppd_snapshot_build_port_pairs(const struct dppd_config *cfg, struct dppd_forwarding_snapshot *snapshot)
{
    uint16_t i;

    if (cfg == NULL || snapshot == NULL || cfg->nb_ports > DPPD_MAX_PORTS || (cfg->nb_ports & 1U) != 0)
        return -1;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->generation = 1;
    snapshot->nb_peers = cfg->nb_ports;
    for (i = 0; i < cfg->nb_ports; i += 2U) {
        snapshot->peers[i].ingress_port = cfg->ports[i];
        snapshot->peers[i].egress_port = cfg->ports[i + 1U];
        snapshot->peers[i + 1U].ingress_port = cfg->ports[i + 1U];
        snapshot->peers[i + 1U].egress_port = cfg->ports[i];
    }
    return 0;
}

/**
 * 按入口端口查询静态配对出口，不检查目的 IP
 * 查到时写入 egress_port 并返回零，参数无效或没有配对时返回非零
 * 由 pipeline.c 调用，查询失败时由调用方决定丢弃
 */
int dppd_snapshot_lookup_peer(const struct dppd_forwarding_snapshot *snapshot, uint16_t ingress_port,
    uint16_t *egress_port)
{
    uint16_t i;

    if (snapshot == NULL || egress_port == NULL)
        return -1;
    for (i = 0; i < snapshot->nb_peers; ++i) {
        if (snapshot->peers[i].ingress_port == ingress_port) {
            *egress_port = snapshot->peers[i].egress_port;
            return 0;
        }
    }
    return -1;
}
