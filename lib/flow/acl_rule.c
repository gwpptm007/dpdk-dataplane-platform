#include "acl_rule.h"

int dppd_acl_match(const struct dppd_acl_tuple *tuple)
{
    if (tuple == 0)
        return -1;

    /*
     * Phase 1 baseline policy:
     * - UDP is allowed by default
     * - TCP is denied by default
     * - other protocols are denied unless upper layer decides otherwise
     */
    if (tuple->proto == 17U)
        return 0;
    if (tuple->proto == 6U)
        return -1;
    return -1;
}
