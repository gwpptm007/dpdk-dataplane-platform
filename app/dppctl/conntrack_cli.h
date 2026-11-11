#ifndef DPPCTL_CONNTRACK_CLI_H
#define DPPCTL_CONNTRACK_CLI_H
#include "dppd/management.h"

int dppctl_conntrack_build(int argc, char **argv, struct dppd_management_request *request,
    void (*initialize)(struct dppd_management_request *, enum dppd_management_operation));
bool dppctl_conntrack_print(const struct dppd_management_response *response);
int dppctl_conntrack_policy_read(const char *path, struct dppd_conntrack_policy *policy);
int dppctl_nd6_policy_read(const char *path, struct dppd_nd6_policy *policy);
int dppctl_arp_policy_read(const char *path, struct dppd_arp_policy *policy);
#endif
