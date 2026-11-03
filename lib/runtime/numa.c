#include "dppd/numa.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * 从 cpus 候选中选出 workers 个不同 lcore，将结果写入 selected，不创建或启动线程
 * 优先与更多网口同 NUMA 节点，再比较该节点已选数量，最后按 lcore 编号决定
 * 成功返回 0，候选不足返回 -ENOSPC，参数或重复 lcore 编号无效返回 -EINVAL
 */
int dppd_numa_select(const struct dppd_numa_cpu *cpus, uint32_t cpu_count, const int *port_sockets, uint16_t port_count,
    uint16_t workers, struct dppd_numa_cpu *selected)
{
    if (cpus == NULL || selected == NULL || port_sockets == NULL || port_count == 0 || workers == 0)
        return -EINVAL;
    if (cpu_count < workers)
        return -ENOSPC;
    /** 重复编号不是两个可用线程，先拒绝整份输入，避免静默重复绑定 */
    for (uint32_t i = 0; i < cpu_count; ++i)
        for (uint32_t j = 0; j < i; ++j)
            if (cpus[i].lcore_id == cpus[j].lcore_id)
                return -EINVAL;
    for (uint16_t queue = 0; queue < workers; ++queue) {
        uint32_t best = UINT32_MAX, best_local = 0, best_used = UINT32_MAX;
        for (uint32_t i = 0; i < cpu_count; ++i) {
            bool chosen = false;
            uint32_t local = 0, used = 0;
            for (uint16_t j = 0; j < queue; ++j) {
                chosen |= selected[j].lcore_id == cpus[i].lcore_id;
                used += cpus[i].socket_id >= 0 && selected[j].socket_id == cpus[i].socket_id;
            }
            if (chosen)
                continue;
            for (uint16_t port = 0; port < port_count; ++port)
                local += cpus[i].socket_id >= 0 && port_sockets[port] == cpus[i].socket_id;
            /** 相同拓扑总是产生相同布局，未知 socket 不假装与任何网卡同节点 */
            if (best == UINT32_MAX || local > best_local ||
                (local == best_local &&
                    (used < best_used || (used == best_used && cpus[i].lcore_id < cpus[best].lcore_id)))) {
                best = i;
                best_local = local;
                best_used = used;
            }
        }
        selected[queue] = cpus[best];
    }
    return 0;
}
