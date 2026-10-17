/**
 * 只在测试进程显式加载，替换 indirect API 边界，不链接或安装到正式程序
 * 对象仅存在于测试进程堆内存，跨进程保护流程通过也不能作为 ASIC 残留清理证据
 */
#ifndef ALLOW_EXPERIMENTAL_API
#define ALLOW_EXPERIMENTAL_API
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rte_errno.h>
#include <rte_flow.h>

struct test_shared_count {
    uint16_t port_id;
};

static unsigned int live;

static int fail(struct rte_flow_error *error, int code)
{
    rte_errno = code;
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
        error->message = "test-only shared COUNT failure";
    }
    return -code;
}

struct rte_flow_action_handle *rte_flow_action_handle_create(uint16_t port_id,
    const struct rte_flow_indir_action_conf *conf, const struct rte_flow_action *action, struct rte_flow_error *error)
{
    const char *mode = getenv("DPPD_TEST_SHARED_RECOVERY_MODE");
    if (mode == NULL || conf == NULL || !conf->ingress || conf->egress || conf->transfer || action == NULL ||
        action->type != RTE_FLOW_ACTION_TYPE_COUNT) {
        fail(error, ENOTSUP);
        return NULL;
    }
    if (strcmp(mode, "create-failed") == 0) {
        fail(error, ENOSPC);
        return NULL;
    }
    struct test_shared_count *object = calloc(1, sizeof(*object));
    if (object == NULL) {
        fail(error, ENOMEM);
        return NULL;
    }
    object->port_id = port_id;
    ++live;
    return (struct rte_flow_action_handle *)object;
}

int rte_flow_action_handle_query(uint16_t port_id, const struct rte_flow_action_handle *handle, void *data,
    struct rte_flow_error *error)
{
    const struct test_shared_count *object = (const struct test_shared_count *)handle;
    if (object == NULL || object->port_id != port_id || data == NULL)
        return fail(error, EINVAL);
    struct rte_flow_query_count *count = data;
    *count = (struct rte_flow_query_count){.hits_set = 1, .bytes_set = 1};
    return 0;
}

int rte_flow_action_handle_destroy(uint16_t port_id, struct rte_flow_action_handle *handle,
    struct rte_flow_error *error)
{
    struct test_shared_count *object = (struct test_shared_count *)handle;
    if (object == NULL || object->port_id != port_id || live == 0)
        return fail(error, EINVAL);
    --live;
    free(object);
    fprintf(stderr, "[dppd-test] shared-count-destroy live=%u\n", live);
    return 0;
}
