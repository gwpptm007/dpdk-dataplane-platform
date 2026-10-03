#ifndef DPPD_DEVICE_H
#define DPPD_DEVICE_H

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <rte_config.h>
#include <rte_ether.h>
#include "dppd/config.h"
#include "dppd/capability.h"
#include "dppd/link.h"
#include "dppd/topology.h"

struct rte_mempool;

struct dppd_port_capabilities {
    uint16_t max_rx_queues;
    uint16_t max_tx_queues;
    uint16_t reta_size;
    uint64_t rss_offloads;
    uint64_t rx_offloads;
    uint64_t tx_offloads;
    uint64_t device_capabilities;
};

struct dppd_port {
    uint16_t port_id;
    uint16_t peer_port_id;
    int socket_id;
    struct dppd_port_capabilities capabilities;
    /** 启动时捕获的设备身份、固件和描述符信息，管理查询不重复访问驱动 */
    struct dppd_capability_identity identity;
    uint64_t configured_rss_hf;
    uint64_t configured_tx_offloads;
    struct rte_ether_addr mac;
    bool configured;
    bool started;
    /** 主线程记录是否已登记移除回调，清理时据此先注销回调，再关闭设备 */
    bool removal_callback_registered;
    /** 设备移除后保持为真，链路恢复不能清除此标记，必须重启后重新初始化设备 */
    atomic_bool removed;
    atomic_uint link_state;
};

struct dppd_device_set {
    uint16_t nb_ports;
    /** 任一端口移除就通知全部工作线程结束，避免端口对只剩一侧继续收发 */
    atomic_bool removal_requested;
    struct dppd_port ports[DPPD_MAX_PORTS];
    struct rte_mempool *pools[RTE_MAX_NUMA_NODES];
    struct dppd_topology topology;
};

int dppd_devices_init(struct dppd_device_set *devices, const struct dppd_config *cfg);
int dppd_devices_poll_links(struct dppd_device_set *devices);
/** 与回调的 release 写入配对，让读者看到移除请求时也能看到先前发布的端口状态 */
static inline bool dppd_devices_removal_requested(const struct dppd_device_set *devices)
{
    return atomic_load_explicit(&devices->removal_requested, memory_order_acquire);
}
/**
 * 只有未移除且链路可用的出口才允许发送
 * 初始不支持链路查询的 PMD 继续使用原有收发能力，不能把“不支持查询”当成链路断开
 */
static inline bool dppd_port_tx_available(const struct dppd_port *port)
{
    const unsigned int state = atomic_load_explicit(&port->link_state, memory_order_acquire);

    return !atomic_load_explicit(&port->removed, memory_order_acquire) &&
           (state == DPPD_LINK_UP || state == DPPD_LINK_UNSUPPORTED);
}
/**
 * 调用前必须等待所有工作线程退出，函数依次注销回调、关闭端口并释放共享缓冲池
 * 返回首个清理错误；无法确认端口已关闭时保留缓冲池，防止驱动访问已释放的报文内存
 */
int dppd_devices_stop(struct dppd_device_set *devices);
const struct dppd_port *dppd_devices_find(const struct dppd_device_set *devices,
                                          uint16_t port_id);

#endif
