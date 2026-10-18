#ifndef DPPD_SOFTWARE_SHARED_H
#define DPPD_SOFTWARE_SHARED_H

#include <stdatomic.h>
#include "dppd/flow_shared_count.h"

/** 软件共享计数由管理库拥有，规则快照持有精确借用，旧读者退出前不能释放 */
struct dppd_software_shared_counter {
    atomic_uint_fast64_t hits, bytes;
};

struct dppd_flow_shared_count_driver dppd_software_shared_count_driver(void);

#endif
