#ifndef DPPD_RUNTIME_H
#define DPPD_RUNTIME_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "dppd/config.h"
#include "dppd/device.h"
#include "dppd/pipeline.h"
#include "dppd/stats.h"

struct dppd_runtime;
struct dppd_software_backend;

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
    struct dppd_runtime *runtime;
    unsigned int lcore_id;
    /**
     * queue_id 同时是 RX/TX queue 编号和软件 QSBR reader id。runtime 在启动前一次性
     * 连续分配，运行中不得重新编号，否则 writer 将无法正确等待旧 snapshot 的 reader。
     */
    uint16_t queue_id;
    bool launched;
    /** 跨线程状态使用 release/acquire 传递，管理查询不直接访问 DPDK 的内部线程结构 */
    atomic_uint state;
    struct dppd_worker_stats stats;
    struct dppd_worker_stats port_stats[DPPD_MAX_PORTS];
};

struct dppd_runtime {
    struct dppd_config config;
    struct dppd_device_set devices;
    struct dppd_forwarding_snapshot snapshot;
    /**
     * control 初始化后绑定；NULL 表示只运行静态 port-pair 基线。该指针必须在 worker
     * 启动前设置，并在所有 worker 停止、注销 QSBR 后才允许 control 销毁其对象。
     */
    struct dppd_software_backend *software_backend;
    struct dppd_worker workers[DPPD_MAX_WORKERS];
    uint16_t nb_workers;
    /** 停止请求只通知线程结束循环，不表示线程已经退出；释放共享资源前仍需等待 */
    atomic_bool stop_requested;
    bool pdump_initialized;
    bool workers_started;
};

int dppd_runtime_init(struct dppd_runtime *runtime, const struct dppd_config *cfg);
/** 设置报文路径借用的软件 backend；不转移所有权，也不允许 worker 运行中热替换。 */
void dppd_runtime_set_software_backend(
    struct dppd_runtime *runtime,
    struct dppd_software_backend *software_backend);
int dppd_runtime_start(struct dppd_runtime *runtime);
/** 检查已启动的线程是否异常结束；正常停止和设备移除由各自的退出流程处理 */
int dppd_runtime_check_workers(const struct dppd_runtime *runtime);
void dppd_runtime_request_stop(struct dppd_runtime *runtime);
int dppd_runtime_wait(struct dppd_runtime *runtime);
void dppd_runtime_stats_read(const struct dppd_runtime *runtime,
                             struct dppd_stats_values *total);
int dppd_runtime_stats_query(const struct dppd_runtime *runtime,
                              uint16_t port_id, uint16_t queue_id,
                              struct dppd_stats_values *total);
void dppd_runtime_stats_dump(const struct dppd_runtime *runtime);
/** 停止线程并回收设备资源，返回清理错误，调用方应据此决定最终退出码 */
int dppd_runtime_destroy(struct dppd_runtime *runtime);
/** 工作线程入口，负责登记规则读者、处理收发循环，并在返回前注销读者身份 */
int dppd_worker_main(void *arg);

#endif
