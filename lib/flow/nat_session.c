#include "nat_session.h"

#define DPPD_NAT_TABLE_SIZE 128
#define DPPD_NAT_BASE_PORT  40000U
#define DPPD_NAT_TIMEOUT_TSC 1000000ULL

struct dppd_nat_entry {
    struct dppd_nat_key key;
    uint16_t translated_src_port;
    uint64_t last_seen_tsc;
    int in_use;
};

static struct dppd_nat_entry g_nat_table[DPPD_NAT_TABLE_SIZE];
static uint16_t g_nat_next_port = DPPD_NAT_BASE_PORT;

static int dppd_nat_key_equal(const struct dppd_nat_key *a, const struct dppd_nat_key *b)
{
    return a->src_ip == b->src_ip &&
           a->dst_ip == b->dst_ip &&
           a->src_port == b->src_port &&
           a->dst_port == b->dst_port &&
           a->proto == b->proto;
}

int dppd_nat_translate(struct dppd_nat_key *key)
{
    unsigned int i;

    if (key == 0)
        return -1;

    for (i = 0; i < DPPD_NAT_TABLE_SIZE; ++i) {
        if (g_nat_table[i].in_use && dppd_nat_key_equal(&g_nat_table[i].key, key)) {
            g_nat_table[i].last_seen_tsc++;
            key->src_port = g_nat_table[i].translated_src_port;
            return 0;
        }
    }

    for (i = 0; i < DPPD_NAT_TABLE_SIZE; ++i) {
        if (!g_nat_table[i].in_use) {
            g_nat_table[i].in_use = 1;
            g_nat_table[i].key = *key;
            g_nat_table[i].translated_src_port = g_nat_next_port++;
            if (g_nat_next_port < DPPD_NAT_BASE_PORT)
                g_nat_next_port = DPPD_NAT_BASE_PORT;
            g_nat_table[i].last_seen_tsc = 1;
            key->src_port = g_nat_table[i].translated_src_port;
            return 0;
        }
    }

    return -1;
}

void dppd_nat_aging_run(uint64_t now_tsc)
{
    unsigned int i;

    for (i = 0; i < DPPD_NAT_TABLE_SIZE; ++i) {
        if (!g_nat_table[i].in_use)
            continue;
        if (now_tsc > g_nat_table[i].last_seen_tsc &&
            now_tsc - g_nat_table[i].last_seen_tsc > DPPD_NAT_TIMEOUT_TSC) {
            g_nat_table[i].in_use = 0;
        }
    }
}
