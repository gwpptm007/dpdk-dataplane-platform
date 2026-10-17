/** DPDK 21.11 的 indirect 接口仍标为实验性，仅在此适配文件显式启用 */
#ifndef ALLOW_EXPERIMENTAL_API
#define ALLOW_EXPERIMENTAL_API
#endif
#include "dppd/offload.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <rte_errno.h>
#include <rte_flow.h>

static int report(struct dppd_flow_error *output, int rc, const char *operation, const struct rte_flow_error *error)
{
    if (output != NULL) {
        output->code = rc;
        const char *detail = error != NULL && error->message != NULL ? error->message : rte_strerror(-rc);
        snprintf(output->message, sizeof(output->message), "%s: %s", operation, detail);
    }
    return rc;
}

static bool valid_spec(const struct dppd_shared_count_spec *spec)
{
    return spec != NULL && spec->id != 0 && spec->id != UINT64_MAX && spec->generation != 0 &&
        spec->generation != UINT64_MAX && spec->install_port_id != UINT16_MAX &&
        spec->domain >= DPPD_RULE_DOMAIN_INGRESS && spec->domain <= DPPD_RULE_DOMAIN_TRANSFER;
}

int dppd_flow_shared_count_create(const struct dppd_shared_count_spec *spec, struct dppd_shared_count_handle *handle,
    struct dppd_flow_error *error)
{
    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (!valid_spec(spec) || handle == NULL)
        return report(error, -EINVAL, "shared count create", NULL);
    struct rte_flow_indir_action_conf attributes = {.ingress = spec->domain != DPPD_RULE_DOMAIN_EGRESS,
        .egress = spec->domain == DPPD_RULE_DOMAIN_EGRESS,
        .transfer = spec->domain == DPPD_RULE_DOMAIN_TRANSFER};
    /** 共享身份由 opaque 句柄确定，不把 64 位逻辑 ID 截断后当作驱动计数器编号 */
    const struct rte_flow_action_count count = {0};
    const struct rte_flow_action action = {.type = RTE_FLOW_ACTION_TYPE_COUNT, .conf = &count};
    struct rte_flow_error driver_error = {0};
    struct rte_flow_action_handle *opaque =
        rte_flow_action_handle_create(spec->install_port_id, &attributes, &action, &driver_error);
    if (opaque == NULL)
        return report(error, rte_errno != 0 ? -rte_errno : -EIO, "shared count create", &driver_error);
    *handle = (struct dppd_shared_count_handle){.spec = *spec, .opaque = opaque};
    return 0;
}

int dppd_flow_shared_count_query(const struct dppd_shared_count_handle *handle, uint64_t *hits, uint64_t *bytes,
    struct dppd_flow_error *error)
{
    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (handle == NULL || !valid_spec(&handle->spec) || handle->opaque == NULL || hits == NULL || bytes == NULL)
        return report(error, -EINVAL, "shared count query", NULL);
    struct rte_flow_query_count query = {0};
    struct rte_flow_error driver_error = {0};
    int rc = rte_flow_action_handle_query(handle->spec.install_port_id, handle->opaque, &query, &driver_error);
    if (rc != 0)
        return report(error, rc, "shared count query", &driver_error);
    if (!query.hits_set || !query.bytes_set) {
        driver_error.message = "driver did not provide both packet and byte counters";
        return report(error, -ENODATA, "shared count query", &driver_error);
    }
    *hits = query.hits;
    *bytes = query.bytes;
    return 0;
}

int dppd_flow_shared_count_destroy(struct dppd_shared_count_handle *handle, struct dppd_flow_error *error)
{
    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (handle == NULL || !valid_spec(&handle->spec) || handle->opaque == NULL)
        return report(error, -EINVAL, "shared count destroy", NULL);
    struct rte_flow_error driver_error = {0};
    int rc = rte_flow_action_handle_destroy(handle->spec.install_port_id, handle->opaque, &driver_error);
    if (rc != 0)
        return report(error, rc, "shared count destroy", &driver_error);
    memset(handle, 0, sizeof(*handle));
    return 0;
}
