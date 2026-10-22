#ifndef DPPD_NUMA_H
#define DPPD_NUMA_H

#include <stdint.h>

/** 核编号来自用户允许的 EAL 集合，负 socket 表示拓扑未知 */
struct dppd_numa_cpu {
    unsigned int lcore_id;
    int socket_id;
};

/** 每个 worker 都扫描全部端口，先选本地端口最多的核，同分时均衡 socket */
int dppd_numa_select(const struct dppd_numa_cpu *cpus, uint32_t cpu_count, const int *port_sockets, uint16_t port_count,
    uint16_t workers, struct dppd_numa_cpu *selected);

#endif
