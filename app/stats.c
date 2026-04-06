#include "stats.h"
#include <stdio.h>
#include <string.h>

static struct dppd_lcore_stats g_stats;

void dppd_stats_reset(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
}

void dppd_stats_account_parse(int is_arp, int is_ipv4, int is_udp, int is_tcp)
{
    g_stats.rx_pkts++;
    if (is_arp)
        g_stats.arp_pkts++;
    if (is_ipv4)
        g_stats.ipv4_pkts++;
    if (is_udp)
        g_stats.udp_pkts++;
    if (is_tcp)
        g_stats.tcp_pkts++;
}

void dppd_stats_account_rx_drop(void)
{
    g_stats.rx_drop++;
}

void dppd_stats_account_fwd_tx(void)
{
    g_stats.tx_pkts++;
}

void dppd_stats_account_fwd_drop(void)
{
    g_stats.tx_drop++;
}

void dppd_stats_tick(void)
{
}

void dppd_stats_dump(void)
{
    printf("[dppd] stats rx=%llu tx=%llu rx_drop=%llu tx_drop=%llu arp=%llu ipv4=%llu udp=%llu tcp=%llu\n",
           (unsigned long long)g_stats.rx_pkts,
           (unsigned long long)g_stats.tx_pkts,
           (unsigned long long)g_stats.rx_drop,
           (unsigned long long)g_stats.tx_drop,
           (unsigned long long)g_stats.arp_pkts,
           (unsigned long long)g_stats.ipv4_pkts,
           (unsigned long long)g_stats.udp_pkts,
           (unsigned long long)g_stats.tcp_pkts);
}
