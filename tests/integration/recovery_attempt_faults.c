#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <rte_errno.h>
#include <rte_flow.h>
#include "dppd/rule.h"
#include "dppd/tap_owner.h"

/**
 * 仅由恢复测试显式预加载，真实 TAP 创建前后暂停整个测试进程，供脚本稳定注入 SIGKILL
 * 不链接到生产程序、不安装，也不替换内核规则；未选择场景时直接调用真实驱动
 */
struct rte_flow *rte_flow_create(uint16_t port, const struct rte_flow_attr *attr,
    const struct rte_flow_item pattern[], const struct rte_flow_action actions[], struct rte_flow_error *error)
{
    struct rte_flow *(*real_create)(uint16_t, const struct rte_flow_attr *,
        const struct rte_flow_item *, const struct rte_flow_action *, struct rte_flow_error *);
    void *symbol = dlsym(RTLD_NEXT, "rte_flow_create");
    const char *mode = getenv("DPPD_TEST_RECOVERY_WINDOW");
    if (symbol == NULL || (mode != NULL && strcmp(mode, "fail-empty") == 0)) {
        rte_errno = EIO;
        if (error != NULL)
            *error = (struct rte_flow_error){.type = RTE_FLOW_ERROR_TYPE_UNSPECIFIED,
                .message = "test recovery empty-handle create failure"};
        return NULL;
    }
    memcpy(&real_create, &symbol, sizeof(real_create));
    /** 模拟创建成功但标识没有进入内核，验证正式回读检查会使用真实 handle 回滚 */
    if (mode != NULL && strcmp(mode, "omit-owner") == 0) {
        struct rte_flow_action untagged[DPPD_RULE_MAX_ACTIONS + 2U];
        unsigned int count = 0;
        for (unsigned int index = 0; index < DPPD_RULE_MAX_ACTIONS + 2U; ++index) {
            if ((int)actions[index].type == DPPD_TAP_ACTION_OWNER_V1)
                continue;
            untagged[count++] = actions[index];
            if (actions[index].type == RTE_FLOW_ACTION_TYPE_END)
                return real_create(port, attr, pattern, untagged, error);
        }
        rte_errno = EINVAL;
        return NULL;
    }
    if (mode != NULL && strcmp(mode, "before") == 0)
        raise(SIGSTOP);
    struct rte_flow *flow = real_create(port, attr, pattern, actions, error);
    if (flow != NULL && mode != NULL && strcmp(mode, "after") == 0)
        raise(SIGSTOP);
    return flow;
}
