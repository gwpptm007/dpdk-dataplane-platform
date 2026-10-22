#include "dppd/numa.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>

int main(void)
{
    struct dppd_numa_cpu cpus[] = {{7, 1}, {2, 0}, {5, 1}, {3, 0}, {9, -1}}, output[5];
    int ports[] = {1, 1, 0};
    assert(dppd_numa_select(cpus, 5, ports, 3, 3, output) == 0);
    assert(output[0].lcore_id == 5 && output[1].lcore_id == 7 && output[2].lcore_id == 2);
    /** 双节点各有同样多的端口时交替选核，稀疏 lcore 编号不会变成稀疏队列 */
    assert(dppd_numa_select(cpus, 4, ports + 1, 2, 4, output) == 0);
    assert(output[0].lcore_id == 2 && output[1].lcore_id == 5 && output[2].lcore_id == 3 && output[3].lcore_id == 7);
    int unknown[] = {-1};
    assert(dppd_numa_select(cpus, 1, unknown, 1, 1, output) == 0 && output[0].lcore_id == 7);
    assert(dppd_numa_select(cpus, 2, ports, 3, 3, output) == -ENOSPC);
    assert(dppd_numa_select(NULL, 2, ports, 3, 1, output) == -EINVAL);
    cpus[1].lcore_id = cpus[0].lcore_id;
    assert(dppd_numa_select(cpus, 5, ports, 3, 1, output) == -EINVAL);
    return 0;
}
