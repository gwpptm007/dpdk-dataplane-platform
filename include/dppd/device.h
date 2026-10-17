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
    bool configured_removal_interrupt;
    struct rte_ether_addr mac;
    bool configured;
    bool started;
    /** 主线程记录是否已登记移除回调，清理时据此先注销回调，再关闭设备 */
    bool removal_callback_registered;
    /** 新事件单独登记，普通启动继续沿用原有移除回调，不默认启用复位恢复 */
    bool reset_callback_registered;
    bool destroy_callback_registered;
    /** 设备移除后保持为真，链路恢复不能清除此标记，必须重启后重新初始化设备 */
    atomic_bool removed;
    atomic_uint link_state;
};

struct dppd_device_set {
    uint16_t nb_ports;
    /** 任一端口移除就通知全部工作线程结束，避免端口对只剩一侧继续收发 */
    atomic_bool removal_requested;
    /** 事件门禁只通知暂停收发，不表示工作线程已退出，也不证明旧驱动句柄已经失效 */
    atomic_bool event_tracking_enabled;
    atomic_bool event_io_quiesced;
    atomic_bool event_sequence_overflow;
    atomic_uint event_callbacks_in_flight;
    atomic_uint_fast64_t event_sequence;
    atomic_uint_fast64_t resumed_event_sequence;
    /** 位图使用配置数组的下标，不能把稀疏 ethdev 编号直接当作位移 */
    atomic_uint reset_event_ports;
    atomic_uint removal_event_ports;
    atomic_uint destroy_event_ports;
    struct dppd_port ports[DPPD_MAX_PORTS];
    struct rte_mempool *pools[RTE_MAX_NUMA_NODES];
    struct dppd_topology topology;
};

/** 位图保留本次设备实例的累计证据，序号标记是否出现新事件，不在读取或恢复时清空 */
struct dppd_device_event_snapshot {
    uint64_t sequence;
    uint32_t reset_ports, removal_ports, destroy_ports;
    uint32_t callbacks_in_flight;
    bool enabled, quiesced, overflowed;
};

enum dppd_device_reset_phase {
    DPPD_DEVICE_RESET_PRECHECK = 0,
    DPPD_DEVICE_RESET_STOP,
    DPPD_DEVICE_RESET_RESET,
    DPPD_DEVICE_RESET_VERIFY,
    DPPD_DEVICE_RESET_CONFIGURE,
    DPPD_DEVICE_RESET_START,
    DPPD_DEVICE_RESET_READY,
};

/** 位图按 devices->ports 下标记录已完成的驱动操作，失败不表示先前操作已撤销 */
struct dppd_device_reset_result {
    enum dppd_device_reset_phase phase;
    uint16_t port_id;
    int last_error;
    uint64_t sequence_after;
    uint32_t stopped_ports, reset_ports, configured_ports, started_ports;
};

int dppd_devices_init(struct dppd_device_set *devices, const struct dppd_config *cfg);
int dppd_devices_poll_links(struct dppd_device_set *devices);
/**
 * 必须由主线程显式启用，并在启动 worker 前完成登记
 * 登记期间也可能收到事件，失败会保留已登记回调和门禁，交给 stop 安全注销
 */
int dppd_devices_enable_event_tracking(struct dppd_device_set *devices);
/** 只读取原子证据，不调用 PMD，不确认清理，也不消费事件 */
void dppd_devices_event_snapshot(const struct dppd_device_set *devices, struct dppd_device_event_snapshot *snapshot);
/**
 * 只有主线程已等待全部 worker 退出、重新核对身份并完成恢复后才可调用
 * 这里只解除本进程收发门禁，不启动线程或端口，不证明硬件恢复
 * 期间有新事件则失败，RMV 和 DESTROY 的永久移除状态不能由本接口清除
 */
int dppd_devices_resume_after_event(struct dppd_device_set *devices, uint64_t expected_sequence);
/** 主线程主动暂停收发，不制造设备事件，也不替代停止并等待全部 worker */
int dppd_devices_pause_io(struct dppd_device_set *devices, uint64_t expected_sequence);
/**
 * 只核对本地保存的身份、完整端口组和原队列配置，不调用驱动、不暂停收发或修改输入
 * 主线程须在清理旧规则之前调用，成功后仍须执行管理口检查和复位时的实际设备核对
 */
int dppd_devices_reset_identity_ready(const struct dppd_device_set *devices, const struct dppd_config *cfg);
/**
 * 调用方必须先完成管理口保护检查、等待全部 worker 退出并清空拥有的 flow 与 shared 对象
 * 全组 stop 成功后才允许 reset，沿用原缓冲池、队列、RSS 和 offload，不关闭设备或释放池
 * 驱动失败可能已经改变设备，结果保留完成阶段与位图，不能当作失败无副作用直接重试
 * RESET 阶段可记录稳定的新 RESET 事件，其余阶段出现新事件立即失败
 * 成功仍保持收发暂停，不更新 live topology，也不确认外部对象已经清理
 * 主线程必须重新核对拓扑与 sequence_after，恢复对象后才能显式解除门禁
 */
int dppd_devices_reset_reconfigure(struct dppd_device_set *devices, const struct dppd_config *cfg,
    uint64_t expected_sequence, struct dppd_device_reset_result *result);
/** 与回调的 release 写入配对，让读者看到移除请求时也能看到先前发布的端口状态 */
static inline bool dppd_devices_removal_requested(const struct dppd_device_set *devices)
{
    return atomic_load_explicit(&devices->removal_requested, memory_order_acquire);
}
/** 默认实例只读取原有移除状态，显式启用后再同时检查复位门禁和在途回调 */
static inline bool dppd_devices_io_quiesced(const struct dppd_device_set *devices)
{
    if (dppd_devices_removal_requested(devices))
        return true;
    if (!atomic_load_explicit(&devices->event_tracking_enabled, memory_order_acquire))
        return false;
    return atomic_load_explicit(&devices->event_sequence_overflow, memory_order_acquire) ||
        atomic_load_explicit(&devices->event_io_quiesced, memory_order_acquire) ||
        atomic_load_explicit(&devices->event_callbacks_in_flight, memory_order_acquire) != 0 ||
        atomic_load_explicit(&devices->event_sequence, memory_order_acquire) !=
        atomic_load_explicit(&devices->resumed_event_sequence, memory_order_acquire);
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
 * 事件跟踪启用后拒绝清理 DESTROY 或身份无法核对的端口，不能把复用编号当作原设备
 */
int dppd_devices_stop(struct dppd_device_set *devices);
const struct dppd_port *dppd_devices_find(const struct dppd_device_set *devices, uint16_t port_id);

#endif
