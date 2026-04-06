#include "arp_table.h"
#include <string.h>

#define DPPD_ARP_TABLE_SIZE 16

static struct dppd_arp_entry g_arp_table[DPPD_ARP_TABLE_SIZE];

int dppd_arp_table_upsert(uint32_t ipv4_be, const uint8_t mac[6])
{
    unsigned int i;

    if (mac == NULL)
        return -1;

    for (i = 0; i < DPPD_ARP_TABLE_SIZE; ++i) {
        if (g_arp_table[i].ipv4_be == 0 || g_arp_table[i].ipv4_be == ipv4_be) {
            g_arp_table[i].ipv4_be = ipv4_be;
            memcpy(g_arp_table[i].mac, mac, 6);
            g_arp_table[i].last_seen_tsc++;
            return 0;
        }
    }

    return -1;
}
