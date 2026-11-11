#ifndef DPPD_RUNTIME_H
#define DPPD_RUNTIME_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "dppd/config.h"
#include "dppd/device.h"
#include "dppd/pipeline.h"
#include "dppd/stats.h"
#include "dppd/conntrack.h"
#include "dppd/nd6.h"
#include "dppd/arp.h"
#include "dppd/device_recovery.h"

struct dppd_runtime;
struct dppd_software_backend;

/** 只记录本次新开启的组播接收端口，失败时不会关闭原来就已开启的接收模式 */
struct dppd_nd6_rx_preparation {
    uint16_t count;
    uint16_t ports[DPPD_MAX_PORTS];
};

/**
 * 记录工作线程实际走到的阶段，不能把主线程成功提交启动请求等同于线程已可转发
 * 主线程负责 IDLE/STARTING，工作线程完成读者注册后发布 RUNNING，退出时发布结束状态
 */
enum dppd_worker_state {
    DPPD_WORKER_IDLE = 0,
    DPPD_WORKER_STARTING,
    DPPD_WORKER_RUNNING,
    DPPD_WORKER_STOPPED,
    DPPD_WORKER_FAILED,
};

struct dppd_worker {
    /** 借用所属 runtime，以取得设备、转发配置、软件规则表和停止标记 */
    struct dppd_runtime *runtime;
    /** 执行业务循环的 DPDK lcore 编号，与 queue_id 分别编号 */
    unsigned int lcore_id;
    /** 实际选中的 NUMA 节点只用于诊断，负数表示拓扑未知 */
    int socket_id;
    /**
     * queue_id 同时是 RX/TX queue 编号和软件 QSBR reader id。runtime 在启动前一次性
     * 连续分配，运行中不得重新编号，否则 writer 将无法正确等待旧 snapshot 的 reader
     */
    uint16_t queue_id;
    /** 启动请求是否已成功提交，退出时据此等待对应 lcore 返回 */
    bool launched;
    /** 跨线程状态使用 release/acquire 传递，管理查询不直接访问 DPDK 的内部线程结构 */
    atomic_uint state;
    /** 线程完成一轮收发后低频发布进度，管理线程据此识别仍存活但不前进的线程 */
    atomic_uint_fast64_t heartbeat_ms;
    struct dppd_worker_stats stats;
    struct dppd_worker_stats port_stats[DPPD_MAX_PORTS];
};

struct dppd_runtime {
    struct dppd_config config;
    struct dppd_device_set devices;
    struct dppd_forwarding_snapshot snapshot;
    /**
     * control 初始化后绑定；NULL 表示只运行静态 port-pair 基线。该指针必须在 worker
     * 启动前设置，并在所有 worker 停止、注销 QSBR 后才允许 control 销毁其对象
     */
    struct dppd_software_backend *software_backend;
    /** 可选路由器由控制面持有，只在工作线程启动前绑定，退出并注销读者后才能释放 */
    struct dppd_ipv4_router *ipv4_router;
    /** 普通模式拥有会话表，管理模式借用 store 的稳定容器，销毁时按所有权决定是否释放 */
    struct dppd_conntrack *conntrack;
    bool conntrack_owned;
    /** 普通模式拥有 ND 表，统一管理模式借用 store 的稳定容器和等待队列 */
    struct dppd_nd6 *nd6;
    bool nd6_owned;
    /** 统一管理的 ARP 容器由 store 持有，runtime 仅在 worker 运行期间借用 */
    struct dppd_arp *arp;
    struct dppd_worker workers[DPPD_MAX_WORKERS];
    uint16_t nb_workers;
    /** 停止请求只通知线程结束循环，不表示线程已经退出；释放共享资源前仍需等待 */
    atomic_bool stop_requested;
    /** 本次 pdump IPC 初始化是否成功，退出时据此注销，不表示已经开始捕获报文 */
    bool pdump_initialized;
    bool workers_started;
    struct dppd_device_recovery device_recovery;
};

int dppd_runtime_init(struct dppd_runtime *runtime, const struct dppd_config *cfg);
/** 设置报文路径借用的软件 backend；不转移所有权，也不允许 worker 运行中热替换 */
void dppd_runtime_set_software_backend(struct dppd_runtime *runtime, struct dppd_software_backend *software_backend);
void dppd_runtime_set_ipv4_router(struct dppd_runtime *runtime, struct dppd_ipv4_router *router);
/** 只在 worker 停止且 runtime 没有自有会话表时借用容器，不转移所有权 */
int dppd_runtime_set_conntrack(struct dppd_runtime *runtime, struct dppd_conntrack *conntrack);
/** 只在 worker 停止且没有自有 ND 表时绑定，不转移容器和等待包的所有权 */
int dppd_runtime_set_nd6(struct dppd_runtime *runtime, struct dppd_nd6 *nd6);
/** worker 停止后准备 ND 所需的组播接收，失败会补偿，补偿无法确认时返回 EUCLEAN */
int dppd_runtime_prepare_nd6(struct dppd_runtime *runtime, const struct dppd_nd6_policy *policy,
    struct dppd_nd6_rx_preparation *preparation);
/** 策略尚未发布时撤销本次新增设置，策略已发布则保留接收模式直到设备关闭 */
int dppd_runtime_rollback_nd6(struct dppd_runtime *runtime, struct dppd_nd6_rx_preparation *preparation);
int dppd_runtime_start(struct dppd_runtime *runtime);
/** 检查已启动的线程是否异常结束；正常停止和设备移除由各自的退出流程处理 */
int dppd_runtime_check_workers(const struct dppd_runtime *runtime);
/** 使用单调时钟检查单个线程，正常暂停和设备门禁期间不报告卡死 */
bool dppd_runtime_worker_stalled(const struct dppd_runtime *runtime, uint16_t worker, uint64_t now_ms);
void dppd_runtime_request_stop(struct dppd_runtime *runtime);
int dppd_runtime_wait(struct dppd_runtime *runtime);
void dppd_runtime_stats_read(const struct dppd_runtime *runtime, struct dppd_stats_values *total);
int dppd_runtime_stats_query(const struct dppd_runtime *runtime, uint16_t port_id, uint16_t queue_id,
    struct dppd_stats_values *total);
void dppd_runtime_stats_dump(const struct dppd_runtime *runtime);
/** 停止线程并回收设备资源，返回清理错误，调用方应据此决定最终退出码 */
int dppd_runtime_destroy(struct dppd_runtime *runtime);
/** 工作线程入口，负责登记规则读者、处理收发循环，并在返回前注销读者身份 */
int dppd_worker_main(void *arg);

#endif
