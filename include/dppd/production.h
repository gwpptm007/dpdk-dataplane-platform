#ifndef DPPD_PRODUCTION_H
#define DPPD_PRODUCTION_H

#include <stdint.h>

struct dppd_runtime;
struct dppd_flow_management;
struct dppd_production;

enum dppd_production_event {
    DPPD_PRODUCTION_START,
    DPPD_PRODUCTION_READY,
    DPPD_PRODUCTION_ISOLATED,
    DPPD_PRODUCTION_STOP,
    DPPD_PRODUCTION_WORKER_TIMEOUT,
    DPPD_PRODUCTION_SNAPSHOT_FAILED
};

/** 固定事件名和值写成一行 JSON，不把用户路径或认证材料拼进日志 */
void dppd_production_log(enum dppd_production_event event, int error, uint64_t revision);
/** 日志是尽力交付，背压或写入错误会增加累计值，不影响策略持久回执 */
uint64_t dppd_production_log_dropped(void);
/** 绑定后仅管理线程发布配置副本，查询线程不访问仓库、磁盘或网卡句柄 */
int dppd_production_start(struct dppd_runtime *runtime, struct dppd_flow_management *flows,
    struct dppd_production **output);
/** 主循环调用，按查询意愿有界刷新规则计数，状态变化及时发布并记录 */
int dppd_production_poll(struct dppd_production *production, uint64_t now_ms);
/** 必须先解绑查询，再销毁 runtime 和 flows 借用的对象 */
void dppd_production_stop(struct dppd_production **production);

#endif
