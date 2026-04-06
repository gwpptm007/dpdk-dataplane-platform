#include "ctrl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int dppd_parse_u16(const char *text, unsigned short *out)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || out == NULL)
        return -1;

    value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value > 65535UL)
        return -1;

    *out = (unsigned short)value;
    return 0;
}

static int dppd_parse_u32(const char *text, unsigned int *out)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || out == NULL)
        return -1;

    value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value > 0xffffffffUL)
        return -1;

    *out = (unsigned int)value;
    return 0;
}

int dppd_parse_args(int argc, char **argv, struct dppd_app_config *cfg)
{
    int i;

    if (cfg == NULL)
        return -1;

    memset(cfg, 0, sizeof(*cfg));
    cfg->port_id = 0;
    cfg->nb_rxq = 1;
    cfg->nb_txq = 1;
    cfg->mbuf_count = 8192;
    cfg->mbuf_cache = 256;
    cfg->burst_size = 32;
    cfg->poll_loops = 1;
    cfg->promiscuous = true;
    cfg->stats_interval_s = 1;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            if (dppd_parse_u16(argv[++i], &cfg->port_id) != 0)
                return -1;
        } else if (strcmp(argv[i], "--rxq") == 0 && i + 1 < argc) {
            if (dppd_parse_u16(argv[++i], &cfg->nb_rxq) != 0)
                return -1;
        } else if (strcmp(argv[i], "--txq") == 0 && i + 1 < argc) {
            if (dppd_parse_u16(argv[++i], &cfg->nb_txq) != 0)
                return -1;
        } else if (strcmp(argv[i], "--mbufs") == 0 && i + 1 < argc) {
            if (dppd_parse_u32(argv[++i], &cfg->mbuf_count) != 0)
                return -1;
        } else if (strcmp(argv[i], "--cache") == 0 && i + 1 < argc) {
            if (dppd_parse_u16(argv[++i], &cfg->mbuf_cache) != 0)
                return -1;
        } else if (strcmp(argv[i], "--burst") == 0 && i + 1 < argc) {
            if (dppd_parse_u16(argv[++i], &cfg->burst_size) != 0)
                return -1;
        } else if (strcmp(argv[i], "--loops") == 0 && i + 1 < argc) {
            if (dppd_parse_u32(argv[++i], &cfg->poll_loops) != 0)
                return -1;
        } else if (strcmp(argv[i], "--stats-interval") == 0 && i + 1 < argc) {
            unsigned int tmp;
            if (dppd_parse_u32(argv[++i], &tmp) != 0)
                return -1;
            cfg->stats_interval_s = (int)tmp;
        } else if (strcmp(argv[i], "--promisc") == 0) {
            cfg->promiscuous = true;
        } else if (strcmp(argv[i], "--no-promisc") == 0) {
            cfg->promiscuous = false;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("usage: dppd [EAL args ... --] [--port N] [--rxq N] [--txq N] [--mbufs N] [--cache N] [--burst N] [--loops N] [--promisc|--no-promisc] [--stats-interval N]\n");
            return -1;
        } else {
            fprintf(stderr, "[dppd] unknown argument: %s\n", argv[i]);
            return -1;
        }
    }

    if (cfg->burst_size == 0)
        cfg->burst_size = 32;
    if (cfg->poll_loops == 0)
        cfg->poll_loops = 1;

    return 0;
}

void dppd_dump_config(const struct dppd_app_config *cfg)
{
    printf("[dppd] mode=%s port=%u rxq=%u txq=%u mbufs=%u cache=%u burst=%u loops=%u promisc=%s stats_interval=%ds\n",
#if DPPD_HAS_DPDK
           "dpdk",
#else
           "mock",
#endif
           cfg->port_id,
           cfg->nb_rxq,
           cfg->nb_txq,
           cfg->mbuf_count,
           cfg->mbuf_cache,
           cfg->burst_size,
           cfg->poll_loops,
           cfg->promiscuous ? "on" : "off",
           cfg->stats_interval_s);
}
