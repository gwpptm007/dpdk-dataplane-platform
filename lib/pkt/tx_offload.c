#include "tx_offload.h"
#include "parser.h"

#if DPPD_HAS_DPDK
#include <rte_mbuf.h>
#endif

void dppd_prepare_tx_checksum_flags(void *mbuf, const struct dppd_parse_result *res)
{
    (void)mbuf;
    (void)res;
#if DPPD_HAS_DPDK
    /*
     * Phase 1 keeps checksum generation in software via pkt_rewrite.c so that the
     * same packet path works even when the target NIC does not advertise checksum
     * offload capability. This hook is intentionally kept in place for the next
     * step where per-port tx offload masks will be applied.
     */
#endif
}
