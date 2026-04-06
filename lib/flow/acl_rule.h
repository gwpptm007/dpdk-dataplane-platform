#ifndef DPPD_ACL_RULE_H
#define DPPD_ACL_RULE_H

#include <stdint.h>

struct dppd_acl_tuple {
    uint32_t src_ip_be;
    uint32_t dst_ip_be;
    uint16_t src_port_be;
    uint16_t dst_port_be;
    uint8_t proto;
};

int dppd_acl_match(const struct dppd_acl_tuple *tuple);

#endif
