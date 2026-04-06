#ifndef DPPD_PKT_REWRITE_H
#define DPPD_PKT_REWRITE_H

#include <stdint.h>
struct dppd_parse_result;
struct dppd_nat_key;

int dppd_rewrite_udp_ipv4(void *pkt,
                          unsigned int len,
                          const struct dppd_parse_result *res,
                          const struct dppd_nat_key *nat);

#endif
