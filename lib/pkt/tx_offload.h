#ifndef DPPD_TX_OFFLOAD_H
#define DPPD_TX_OFFLOAD_H
struct dppd_parse_result;
void dppd_prepare_tx_checksum_flags(void *mbuf, const struct dppd_parse_result *res);
#endif
